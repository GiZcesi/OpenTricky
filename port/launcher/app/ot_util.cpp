/*
 * ot_util.cpp -- see ot_util.h.
 */
#include "ot_util.h"

#include <cstdio>
#include <cstring>

namespace {

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

} // namespace

/* ── What's new ─────────────────────────────────────────────────────── */

OtNotes ot_notes_parse(const std::string &text)
{
    OtNotes n;
    size_t a = text.find("<!-- launcher");
    if (a == std::string::npos) return n;
    size_t b = text.find("-->", a);
    if (b == std::string::npos) return n;
    std::string body = text.substr(a + 13, b - a - 13);
    static const char *const order[] = { "fixes", "new", "better" };
    std::vector<std::string> sec[3];
    size_t p = 0;
    while (p < body.size()) {
        size_t q = body.find('\n', p);
        if (q == std::string::npos) q = body.size();
        std::string line = trim(body.substr(p, q - p));
        p = q + 1;
        size_t c = line.find(':');
        if (line.empty() || c == std::string::npos) continue;
        std::string k = lower(trim(line.substr(0, c))), v = trim(line.substr(c + 1));
        if (v.empty()) continue;
        if (k == "version") n.version = v;
        else if (k == "title") n.title = v;
        else if (k == "card") n.card = lower(v);
        else
            for (int i = 0; i < 3; i++)
                if (k == order[i]) sec[i].push_back(v);
    }
    for (int i = 0; i < 3; i++) {
        if (sec[i].empty()) continue;
        OtNoteSection s;
        s.name = i == 0 ? "FIXES" : i == 1 ? "NEW" : "BETTER";
        s.items = sec[i];
        n.sections.push_back(s);
    }
    n.ok = !n.title.empty() || !n.sections.empty();
    return n;
}

/* ── Zip ────────────────────────────────────────────────────────────── */

namespace {

uint32_t crc32_of(const std::string &d)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : d) c = table[(c ^ b) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put16(std::string &o, uint32_t v) { o += (char)(v & 255); o += (char)((v >> 8) & 255); }
void put32(std::string &o, uint32_t v) { put16(o, v & 0xFFFF); put16(o, v >> 16); }

} // namespace

bool ot_zip_write(const std::string &path, const std::vector<OtZipEntry> &entries)
{
    SDL_Time now = 0;
    SDL_DateTime dt{};
    SDL_GetCurrentTime(&now);
    SDL_TimeToDateTime(now, &dt, true);
    uint32_t dos_time = ((uint32_t)dt.hour << 11) | ((uint32_t)dt.minute << 5) | ((uint32_t)dt.second / 2);
    uint32_t dos_date = ((uint32_t)(dt.year - 1980) << 9) | ((uint32_t)dt.month << 5) | (uint32_t)dt.day;

    std::string out, central;
    for (const OtZipEntry &e : entries) {
        uint32_t crc = crc32_of(e.data), size = (uint32_t)e.data.size(), off = (uint32_t)out.size();
        /* local header: version 2.0, flag bit 11 = UTF-8 names, method 0 (stored) */
        put32(out, 0x04034b50u); put16(out, 20); put16(out, 0x0800); put16(out, 0);
        put16(out, dos_time); put16(out, dos_date); put32(out, crc); put32(out, size); put32(out, size);
        put16(out, (uint32_t)e.name.size()); put16(out, 0);
        out += e.name;
        out += e.data;
        put32(central, 0x02014b50u); put16(central, 20); put16(central, 20); put16(central, 0x0800);
        put16(central, 0); put16(central, dos_time); put16(central, dos_date); put32(central, crc);
        put32(central, size); put32(central, size); put16(central, (uint32_t)e.name.size());
        put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0); put32(central, 0);
        put32(central, off);
        central += e.name;
    }
    uint32_t cd_off = (uint32_t)out.size();
    out += central;
    put32(out, 0x06054b50u); put16(out, 0); put16(out, 0);
    put16(out, (uint32_t)entries.size()); put16(out, (uint32_t)entries.size());
    put32(out, (uint32_t)central.size()); put32(out, cd_off); put16(out, 0);
    return SDL_SaveFile(path.c_str(), out.data(), out.size());
}

/* ── Files ──────────────────────────────────────────────────────────── */

bool ot_read_file(const std::string &path, std::string &out, size_t max_bytes)
{
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(path.c_str(), &pi) || pi.type != SDL_PATHTYPE_FILE || pi.size > max_bytes) return false;
    size_t n = 0;
    void *d = SDL_LoadFile(path.c_str(), &n);
    if (!d) return false;
    out.assign((const char *)d, n);
    SDL_free(d);
    return true;
}

bool ot_read_tail(const std::string &path, std::string &out, size_t max_bytes)
{
    SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return false;
    Sint64 size = SDL_GetIOSize(io);
    Sint64 from = size > (Sint64)max_bytes ? size - (Sint64)max_bytes : 0;
    SDL_SeekIO(io, from, SDL_IO_SEEK_SET);
    out.resize((size_t)(size - from));
    size_t got = out.empty() ? 0 : SDL_ReadIO(io, &out[0], out.size());
    out.resize(got);
    SDL_CloseIO(io);
    if (from > 0) out = "(... the start of the file is left out ...)\n" + out.substr(out.find('\n') + 1);
    return true;
}

std::string ot_dir_of(const std::string &path)
{
    size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? "" : path.substr(0, s + 1);
}

std::string ot_file_name(const std::string &path)
{
    size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? path : path.substr(s + 1);
}

std::string ot_scrub(const std::string &text, const std::vector<std::pair<std::string, std::string>> &folders)
{
    std::string t = text;
    auto replace_ci = [&t](const std::string &what, const std::string &with) {
        if (what.size() < 3) return;
        std::string lt = lower(t), lw = lower(what);
        size_t at = 0;
        std::string out;
        size_t last = 0;
        while ((at = lt.find(lw, at)) != std::string::npos) {
            out += t.substr(last, at - last);
            out += with;
            at += lw.size();
            last = at;
        }
        out += t.substr(last);
        t = out;
    };
    for (const auto &f : folders) {
        std::string a = f.first;
        while (!a.empty() && (a.back() == '\\' || a.back() == '/')) a.pop_back();
        if (a.size() < 4) continue;                 /* never a bare drive */
        replace_ci(a, f.second);
        for (char &c : a) if (c == '\\') c = '/';
        replace_ci(a, f.second);
    }
    const char *home = SDL_getenv("USERPROFILE");
    if (!home) home = SDL_getenv("HOME");
    const char *user = SDL_getenv("USERNAME");
    if (!user) user = SDL_getenv("USER");
    if (home && *home) {
        std::string h = home;
        replace_ci(h, "%USERPROFILE%");
        for (char &c : h) if (c == '\\') c = '/';
        replace_ci(h, "%USERPROFILE%");
    }
    if (user && *user) replace_ci(user, "<user>");
    return t;
}

/* ── Crash reports ──────────────────────────────────────────────────── */

namespace {

struct Scan {
    SDL_Time since;
    std::string root;
    OtCrashReport best;
};

SDL_EnumerationResult SDLCALL scan_one(void *ud, const char *dirname, const char *fname)
{
    Scan *s = (Scan *)ud;
    std::string full = std::string(dirname) + fname;
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(full.c_str(), &pi) || pi.type != SDL_PATHTYPE_DIRECTORY) return SDL_ENUM_CONTINUE;
    SDL_Time t = pi.create_time > pi.modify_time ? pi.create_time : pi.modify_time;
    if (t < s->since || (s->best.found && t <= s->best.time)) return SDL_ENUM_CONTINUE;
    /* only the game's folders: <date>_<time>_<kind> with a report.txt */
    std::string rep = full + "/report.txt";
    if (!SDL_GetPathInfo(rep.c_str(), nullptr)) return SDL_ENUM_CONTINUE;
    s->best.found = true;
    s->best.dir = full + "\\";
    s->best.name = fname;
    s->best.time = t;
    std::string n = fname;
    s->best.kind = n.find("freeze") != std::string::npos ? "freeze" : n.find("crash") != std::string::npos ? "crash" : "";
    return SDL_ENUM_CONTINUE;
}

} // namespace

OtCrashReport ot_find_crash_report(const std::string &game_dir, SDL_Time since)
{
    Scan s;
    s.since = since;
    std::vector<std::string> roots = { game_dir + "CrashReports\\" };
    if (const char *la = SDL_getenv("LOCALAPPDATA")) roots.push_back(std::string(la) + "\\SSX Tricky\\CrashReports\\");
    for (const std::string &r : roots) SDL_EnumerateDirectory(r.c_str(), scan_one, &s);
    return s.best;
}

std::string ot_when(SDL_Time t)
{
    SDL_DateTime d{}, n{};
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    if (!SDL_TimeToDateTime(t, &d, true) || !SDL_TimeToDateTime(now, &n, true)) return "";
    char b[64];
    int day_t = d.year * 400 + d.month * 32 + d.day, day_n = n.year * 400 + n.month * 32 + n.day;
    if (day_t == day_n) snprintf(b, sizeof b, "today %02d:%02d", d.hour, d.minute);
    else if (day_n - day_t == 1)
        snprintf(b, sizeof b, "yesterday %02d:%02d", d.hour, d.minute);
    else snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", d.year, d.month, d.day, d.hour, d.minute);
    return b;
}
