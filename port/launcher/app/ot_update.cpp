/*
 * ot_update.cpp -- see ot_update.h.
 */
#include "ot_update.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace {

const long long k_cache_max_age_s = 24 * 3600;     /* one check a day at most */
const char k_cache_magic[] = "OpenTricky update cache 1";

double now_ms() { return (double)SDL_GetTicksNS() / 1e6; }

/* ── A small JSON reader: just enough for the releases list ─────────── */

struct Json {
    const std::string &s;
    size_t p = 0;
    bool bad = false;

    void ws() { while (p < s.size() && (unsigned char)s[p] <= ' ') p++; }
    bool eat(char c) { ws(); if (p < s.size() && s[p] == c) { p++; return true; } return false; }

    static void put_utf8(std::string &o, unsigned cp)
    {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    }
    bool hex4(unsigned &v)
    {
        if (p + 4 > s.size()) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = s[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string &o)
    {
        o.clear();
        if (!eat('"')) return false;
        while (p < s.size()) {
            char c = s[p++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (p >= s.size()) return false;
            char e = s[p++];
            switch (e) {
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                    p += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(o, cp);
                break;
            }
            default: o += e; break;          /* \" \\ \/ */
            }
        }
        return false;
    }
    /* Skips any value; a string or a boolean is also given back as text. */
    bool value(std::string *text, int depth = 0)
    {
        ws();
        if (p >= s.size() || depth > 64) return false;
        char c = s[p];
        if (c == '"') { std::string t; if (!str(t)) return false; if (text) *text = t; return true; }
        if (c == '{' || c == '[') {
            char close = c == '{' ? '}' : ']';
            p++;
            if (eat(close)) return true;
            for (;;) {
                if (c == '{') { std::string k; if (!str(k) || !eat(':')) return false; }
                if (!value(nullptr, depth + 1)) return false;
                if (eat(',')) continue;
                return eat(close);
            }
        }
        size_t a = p;
        while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']' && (unsigned char)s[p] > ' ') p++;
        if (p == a) return false;
        if (text) *text = s.substr(a, p - a);
        return true;
    }
};

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

/* Markdown kept to its words: **bold**, `code`, [text](link). */
std::string plain(const std::string &in)
{
    std::string o;
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '*' || c == '`' || (c == '_' && (i == 0 || in[i - 1] == ' ' || i + 1 == in.size() || in[i + 1] == ' '))) continue;
        if (c == '[') {
            size_t e = in.find("](", i);
            size_t f = e == std::string::npos ? e : in.find(')', e);
            if (f != std::string::npos) { o += in.substr(i + 1, e - i - 1); i = f; continue; }
        }
        o += c;
    }
    return trim(o);
}

/* ── Cache file ──────────────────────────────────────────────────────── */

struct Cache {
    long long time = 0;
    std::string etag, skip, json;
};

bool cache_read(const std::string &file, Cache &c)
{
    std::string t;
    if (file.empty() || !ot_read_file(file, t, 8u << 20)) return false;
    if (t.compare(0, sizeof k_cache_magic - 1, k_cache_magic) != 0) return false;
    size_t p = 0;
    while (p < t.size()) {
        size_t q = t.find('\n', p);
        if (q == std::string::npos) q = t.size();
        std::string line = t.substr(p, q - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        p = q + 1;
        if (line.rfind("time: ", 0) == 0) c.time = atoll(line.c_str() + 6);
        else if (line.rfind("etag: ", 0) == 0) c.etag = line.substr(6);
        else if (line.rfind("skip: ", 0) == 0) c.skip = line.substr(6);
        else if (line == "json:") { c.json = p < t.size() ? t.substr(p) : ""; break; }
    }
    return true;
}

bool cache_write(const std::string &file, const Cache &c)
{
    if (file.empty()) return false;
    std::string t = std::string(k_cache_magic) + "\n";
    t += "# The launcher's last look at the list of OpenTricky releases on GitHub\n";
    t += "# (one a day at most). Deleting this file is harmless.\n";
    t += "time: " + std::to_string(c.time) + "\n";
    if (!c.etag.empty()) t += "etag: " + c.etag + "\n";
    if (!c.skip.empty()) t += "skip: " + c.skip + "\n";
    t += "json:\n" + c.json;
    std::string tmp = file + ".tmp";
    SDL_IOStream *io = SDL_IOFromFile(tmp.c_str(), "wb");
    if (!io) return false;
    bool ok = SDL_WriteIO(io, t.data(), t.size()) == t.size();
    ok = SDL_CloseIO(io) && ok;
    if (!ok) { SDL_RemovePath(tmp.c_str()); return false; }
    if (!SDL_RenamePath(tmp.c_str(), file.c_str())) { SDL_RemovePath(tmp.c_str()); return false; }
    return true;
}

/* ── HTTPS GET (WinHTTP) ─────────────────────────────────────────────── */

#ifdef _WIN32
std::wstring wide(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
#endif

} // namespace

OtHttp ot_http_get(const OtHttpRequest &q)
{
    OtHttp r;
#ifdef _WIN32
    std::wstring wurl = wide(q.url);
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof u;
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    u.lpszHostName = host; u.dwHostNameLength = 255;
    u.lpszUrlPath = path; u.dwUrlPathLength = 2047;
    u.lpszExtraInfo = extra; u.dwExtraInfoLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &u)) {
        r.error = "not an address";
        return r;
    }
    /* HTTPS only; plain HTTP only to this computer, for the project's tests. */
    bool loopback = wcscmp(host, L"127.0.0.1") == 0 || _wcsicmp(host, L"localhost") == 0;
    bool secure = u.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && !(q.allow_loopback_http && loopback && u.nScheme == INTERNET_SCHEME_HTTP)) {
        r.error = "not an https address";
        return r;
    }
    std::wstring object = std::wstring(path) + extra;
    HINTERNET s = WinHttpOpen(wide(q.user_agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(wide(q.user_agent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { r.error = "WinHttpOpen " + std::to_string(GetLastError()); return r; }
    WinHttpSetTimeouts(s, q.timeout_ms, q.timeout_ms, q.timeout_ms, q.timeout_ms);
    /* GitHub's downloads redirect to its file host: https to https only. */
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    HINTERNET h = c ? WinHttpOpenRequest(c, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    SDL_IOStream *out = nullptr;
    if (h) {
        DWORD no_cookies = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(h, WINHTTP_OPTION_DISABLE_FEATURE, &no_cookies, sizeof no_cookies);
        std::wstring hdr = wide(q.headers);
        if (!q.etag.empty()) hdr += L"If-None-Match: " + wide(q.etag) + L"\r\n";
        if (WinHttpSendRequest(h, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), hdr.empty() ? 0 : (DWORD)-1L,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(h, nullptr)) {
            DWORD st = 0, sz = sizeof st;
            WinHttpQueryHeaders(h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
            r.status = (int)st;
            wchar_t et[256] = {};
            DWORD el = sizeof et;
            if (WinHttpQueryHeaders(h, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, et, &el, WINHTTP_NO_HEADER_INDEX)) {
                char b[256];
                int n = WideCharToMultiByte(CP_UTF8, 0, et, -1, b, sizeof b, nullptr, nullptr);
                if (n > 0) r.etag = b;
            }
            unsigned long long len = 0;
            DWORD ls = sizeof len;
            wchar_t lt[32] = {};
            DWORD lts = sizeof lt;
            if (WinHttpQueryHeaders(h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lt, &lts, WINHTTP_NO_HEADER_INDEX))
                len = wcstoull(lt, nullptr, 10);
            (void)ls;
            r.length = (long long)len;
            if (r.status == 200 && !q.file.empty()) {
                out = SDL_IOFromFile(q.file.c_str(), "wb");
                if (!out) r.error = "cannot write " + q.file;
            }
            std::vector<char> buf(1 << 16);
            while (r.error.empty()) {
                if (q.cancel && q.cancel->load()) { r.error = "cancelled"; break; }
                DWORD got = 0;
                if (!WinHttpReadData(h, buf.data(), (DWORD)buf.size(), &got)) { r.error = "read " + std::to_string(GetLastError()); break; }
                if (got == 0) break;
                r.received += got;
                if (out) {
                    if (SDL_WriteIO(out, buf.data(), got) != got) { r.error = "cannot write " + q.file; break; }
                } else {
                    r.body.append(buf.data(), got);
                    if (r.body.size() > q.max_body) { r.error = "answer too long"; break; }
                }
                if (q.progress) q.progress(r.received, r.length);
            }
            /* A connection cut before the announced length is an error, not a short file. */
            if (r.error.empty() && r.length > 0 && r.received != r.length) r.error = "connection cut";
        } else {
            r.error = "no answer (" + std::to_string(GetLastError()) + ")";
        }
    } else {
        r.error = "WinHttpOpenRequest " + std::to_string(GetLastError());
    }
    if (out && !SDL_CloseIO(out) && r.error.empty()) r.error = "cannot write " + q.file;
    if (h) WinHttpCloseHandle(h);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
#else
    r.error = "no HTTPS client on this system yet";
#endif
    return r;
}

namespace {

struct Http {
    int status = 0;
    std::string body, etag, error;
};

Http https_get(const std::string &url, const std::string &etag, const std::string &agent, int timeout_ms, bool loopback)
{
    OtHttpRequest q;
    q.url = url;
    q.etag = etag;
    q.user_agent = agent;
    q.timeout_ms = timeout_ms;
    q.allow_loopback_http = loopback;
    q.headers = "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    OtHttp r = ot_http_get(q);
    Http o;
    o.status = r.status;
    o.body = std::move(r.body);
    o.etag = r.etag;
    o.error = r.error;
    return o;
}

} // namespace

/* ── Versions ─────────────────────────────────────────────────────────── */

bool ot_version_parse(const std::string &s, int out[3])
{
    size_t p = 0;
    if (p < s.size() && (s[p] == 'v' || s[p] == 'V')) p++;
    for (int i = 0; i < 3; i++) {
        if (p >= s.size() || s[p] < '0' || s[p] > '9') return false;
        long v = 0;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9' && v < 1000000) v = v * 10 + (s[p++] - '0');
        out[i] = (int)v;
        if (i < 2) {
            if (p >= s.size() || s[p] != '.') return false;
            p++;
        }
    }
    return p == s.size() || s[p] == '-' || s[p] == '+';     /* the suffix ("-unstable", "-dev") is ignored */
}

int ot_version_cmp(const std::string &a, const std::string &b)
{
    int x[3], y[3];
    bool ka = ot_version_parse(a, x), kb = ot_version_parse(b, y);
    if (!ka || !kb) return (int)ka - (int)kb;
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

/* ── Releases ─────────────────────────────────────────────────────────── */

namespace {

/* "assets": [{ "name": ..., "browser_download_url": ..., "size": ... }, ...] */
bool parse_assets(Json &j, std::vector<OtAsset> &out)
{
    if (!j.eat('[')) return j.value(nullptr);      /* null or anything else: no assets */
    if (j.eat(']')) return true;
    for (;;) {
        if (!j.eat('{')) return false;
        OtAsset a;
        if (!j.eat('}')) {
            for (;;) {
                std::string k, v;
                if (!j.str(k) || !j.eat(':')) return false;
                bool want = k == "name" || k == "browser_download_url" || k == "size";
                if (!j.value(want ? &v : nullptr)) return false;
                if (k == "name") a.name = v;
                else if (k == "browser_download_url") a.url = v;
                else if (k == "size") a.size = atoll(v.c_str());
                if (j.eat(',')) continue;
                if (!j.eat('}')) return false;
                break;
            }
        }
        out.push_back(a);
        if (j.eat(',')) continue;
        return j.eat(']');
    }
}

} // namespace

bool ot_releases_parse(const std::string &json, std::vector<OtRelease> &out)
{
    out.clear();
    Json j{ json };
    if (!j.eat('[')) return false;
    if (j.eat(']')) return true;
    for (;;) {
        if (!j.eat('{')) return false;
        OtRelease r;
        if (!j.eat('}')) {
            for (;;) {
                std::string k, v;
                if (!j.str(k) || !j.eat(':')) return false;
                if (k == "assets") {
                    if (!parse_assets(j, r.assets)) return false;
                    if (j.eat(',')) continue;
                    if (!j.eat('}')) return false;
                    break;
                }
                bool want = k == "tag_name" || k == "name" || k == "html_url" || k == "body" || k == "prerelease" || k == "draft";
                if (!j.value(want ? &v : nullptr)) return false;
                if (k == "tag_name") r.tag = v;
                else if (k == "name") r.name = v == "null" ? "" : v;
                else if (k == "html_url") r.url = v;
                else if (k == "body") r.body = v == "null" ? "" : v;
                else if (k == "prerelease") r.prerelease = v == "true";
                else if (k == "draft") r.draft = v == "true";
                if (j.eat(',')) continue;
                if (!j.eat('}')) return false;
                break;
            }
        }
        out.push_back(r);
        if (j.eat(',')) continue;
        return j.eat(']');
    }
}

int ot_update_pick(const std::vector<OtRelease> &rel, bool unstable)
{
    int best_stable = -1, best_any = -1;
    for (int i = 0; i < (int)rel.size(); i++) {
        const OtRelease &r = rel[i];
        int v[3];
        if (r.draft || !ot_version_parse(r.tag, v)) continue;
        /* the newest by version; the list's order (newest first) breaks a tie */
        if (best_any < 0 || ot_version_cmp(r.tag, rel[best_any].tag) > 0) best_any = i;
        if (!r.prerelease && (best_stable < 0 || ot_version_cmp(r.tag, rel[best_stable].tag) > 0)) best_stable = i;
    }
    if (unstable) return best_any;
    return best_stable >= 0 ? best_stable : best_any;
}

OtNotes ot_update_notes(const OtRelease &r)
{
    OtNotes n = ot_notes_parse(r.body);
    if (n.version.empty()) {
        int v[3];
        if (ot_version_parse(r.tag, v)) n.version = std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]);
    }
    if (n.ok) return n;
    /* No block: the first three bullets under a "Highlights" heading. */
    n.title = plain(r.name.empty() ? r.tag : r.name);
    OtNoteSection sec;
    sec.name = "HIGHLIGHTS";
    bool in = false;
    size_t p = 0;
    const std::string &b = r.body;
    while (p < b.size() && sec.items.size() < 3) {
        size_t q = b.find('\n', p);
        if (q == std::string::npos) q = b.size();
        std::string line = trim(b.substr(p, q - p));
        p = q + 1;
        bool heading = !line.empty() && line[0] == '#';
        if (heading || (line.size() > 4 && line.compare(0, 2, "**") == 0 && line.compare(line.size() - 2, 2, "**") == 0)) {
            if (in) break;
            std::string h = line;
            for (char &c : h) c = (char)tolower((unsigned char)c);
            in = h.find("highlights") != std::string::npos;
            continue;
        }
        if (in && line.size() > 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            std::string item = plain(line.substr(2));
            if (!item.empty()) sec.items.push_back(item);
        }
    }
    if (!sec.items.empty()) n.sections.push_back(sec);
    n.ok = !n.title.empty() || !n.sections.empty();
    return n;
}

/* ── The check ────────────────────────────────────────────────────────── */

OtUpdateResult ot_update_check(const OtUpdateRequest &req)
{
    OtUpdateResult res;
    double t0 = now_ms();
    auto done = [&](bool ok, const std::string &why, const std::string &json) {
        res.ok = ok && ot_releases_parse(json, res.releases);
        res.source = ok && !res.ok ? "not a list of releases" : why;
        res.ms = now_ms() - t0;
        return res;
    };
    if (!req.json_file.empty()) {
        std::string t;
        if (!ot_read_file(req.json_file, t, 8u << 20)) return done(false, "test file missing", "");
        return done(true, "file", t);
    }
    Cache c;
    bool have = cache_read(req.cache_file, c);
    res.skip = c.skip;
    long long now = (long long)time(nullptr);
    if (have && !c.json.empty() && c.time <= now && now - c.time < k_cache_max_age_s)
        return done(true, "cache", c.json);
    Http h = https_get(req.url, have ? c.etag : "", req.user_agent, req.timeout_ms, req.allow_loopback_http);
    if (h.status == 304 && have && !c.json.empty()) {
        c.time = now;
        cache_write(req.cache_file, c);
        return done(true, "not modified", c.json);
    }
    if (h.status != 200) {
        /* No answer, the API's hourly limit (403 / 429), an error: nothing
         * shown, and the next start asks again. */
        return done(false, h.error.empty() ? "HTTP " + std::to_string(h.status) : h.error, "");
    }
    std::vector<OtRelease> check;
    if (!ot_releases_parse(h.body, check)) return done(false, "not a list of releases", "");
    c.time = now;
    c.etag = h.etag;
    c.json = h.body;
    if (!req.cache_file.empty() && !cache_write(req.cache_file, c)) res.source = "network (cache not written)";
    else res.source = "network";
    res.ok = true;
    res.releases = check;
    res.ms = now_ms() - t0;
    return res;
}

std::string ot_update_read_skip(const std::string &cache_file)
{
    Cache c;
    return cache_read(cache_file, c) ? c.skip : "";
}

bool ot_update_write_skip(const std::string &cache_file, const std::string &version)
{
    Cache c;
    if (!cache_read(cache_file, c)) c = Cache{};      /* no cache yet: one with no list (time 0 = to check) */
    c.skip = version;
    return cache_write(cache_file, c);
}
