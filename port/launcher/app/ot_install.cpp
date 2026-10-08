/*
 * ot_install.cpp -- see ot_install.h.
 */
#include "ot_install.h"
#include "ot_update.h"
#include "ot_util.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/* ── SHA-256 (FIPS 180-4) ────────────────────────────────────────────── */

struct Sha256 {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t buf[64];
    size_t n = 0;
    uint64_t len = 0;

    static uint32_t rotr(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
    void block(const uint8_t *p)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add(const void *data, size_t size)
    {
        const uint8_t *p = (const uint8_t *)data;
        len += size;
        while (size) {
            size_t t = 64 - n < size ? 64 - n : size;
            memcpy(buf + n, p, t);
            n += t; p += t; size -= t;
            if (n == 64) { block(buf); n = 0; }
        }
    }
    std::string hex()
    {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        add(&pad, 1);
        uint8_t z = 0;
        while (n != 56) add(&z, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
        add(l, 8);
        char out[65];
        for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

/* ── CRC-32 (zip) ───────────────────────────────────────────────────── */

uint32_t crc32_of(const uint8_t *d, size_t n)
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
    for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ── Inflate (RFC 1951), after the structure of zlib's puff.c ───────── */

struct Inflate {
    const uint8_t *in;
    size_t inlen, pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool err = false;
    std::vector<uint8_t> &out;
    size_t outmax;

    struct Huff { short count[16]; short symbol[288]; };

    Inflate(const uint8_t *i, size_t n, std::vector<uint8_t> &o, size_t max) : in(i), inlen(n), out(o), outmax(max) {}

    int bits(int need)
    {
        uint32_t val = bitbuf;
        while (bitcnt < need) {
            if (pos >= inlen) { err = true; return 0; }
            val |= (uint32_t)in[pos++] << bitcnt;
            bitcnt += 8;
        }
        bitbuf = val >> need;
        bitcnt -= need;
        return (int)(val & ((1u << need) - 1));
    }
    bool stored()
    {
        bitbuf = 0;
        bitcnt = 0;
        if (pos + 4 > inlen) return false;
        unsigned len = in[pos] | in[pos + 1] << 8, nlen = in[pos + 2] | in[pos + 3] << 8;
        pos += 4;
        if (len != (~nlen & 0xffff) || pos + len > inlen || out.size() + len > outmax) return false;
        out.insert(out.end(), in + pos, in + pos + len);
        pos += len;
        return true;
    }
    int decode(const Huff &h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; len++) {
            code |= bits(1);
            if (err) return -1;
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
    static int construct(Huff &h, const short *length, int n)
    {
        for (int len = 0; len <= 15; len++) h.count[len] = 0;
        for (int s = 0; s < n; s++) h.count[length[s]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len <= 15; len++) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;
        }
        short offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h.count[len]);
        for (int s = 0; s < n; s++)
            if (length[s] != 0) h.symbol[offs[length[s]]++] = (short)s;
        return left;
    }
    bool codes(const Huff &lencode, const Huff &distcode)
    {
        static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
        for (;;) {
            int sym = decode(lencode);
            if (sym < 0 || err) return false;
            if (sym < 256) {
                if (out.size() >= outmax) return false;
                out.push_back((uint8_t)sym);
            } else if (sym == 256) {
                return true;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                int len = lbase[sym] + bits(lext[sym]);
                int ds = decode(distcode);
                if (ds < 0 || ds >= 30 || err) return false;
                size_t dist = (size_t)(dbase[ds] + bits(dext[ds]));
                if (err || dist > out.size() || out.size() + len > outmax) return false;
                size_t from = out.size() - dist;
                for (int i = 0; i < len; i++) out.push_back(out[from + i]);
            }
        }
    }
    bool fixed()
    {
        static Huff lencode, distcode;
        static bool ready = false;
        if (!ready) {
            short lengths[288];
            int s = 0;
            for (; s < 144; s++) lengths[s] = 8;
            for (; s < 256; s++) lengths[s] = 9;
            for (; s < 280; s++) lengths[s] = 7;
            for (; s < 288; s++) lengths[s] = 8;
            construct(lencode, lengths, 288);
            for (s = 0; s < 30; s++) lengths[s] = 5;
            construct(distcode, lengths, 30);
            ready = true;
        }
        return codes(lencode, distcode);
    }
    bool dynamic()
    {
        static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
        short lengths[320];
        Huff lencode, distcode;
        int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (err || nlen > 286 || ndist > 30) return false;
        int i = 0;
        for (; i < ncode; i++) lengths[order[i]] = (short)bits(3);
        for (; i < 19; i++) lengths[order[i]] = 0;
        if (err || construct(lencode, lengths, 19) != 0) return false;
        int index = 0;
        while (index < nlen + ndist) {
            int sym = decode(lencode);
            if (sym < 0 || err) return false;
            if (sym < 16) {
                lengths[index++] = (short)sym;
            } else {
                short len = 0;
                int rep;
                if (sym == 16) {
                    if (index == 0) return false;
                    len = lengths[index - 1];
                    rep = 3 + bits(2);
                } else if (sym == 17) {
                    rep = 3 + bits(3);
                } else {
                    rep = 11 + bits(7);
                }
                if (err || index + rep > nlen + ndist) return false;
                while (rep--) lengths[index++] = len;
            }
        }
        if (lengths[256] == 0) return false;
        int e = construct(lencode, lengths, nlen);
        if (e < 0 || (e > 0 && nlen - lencode.count[0] != 1)) return false;
        e = construct(distcode, lengths + nlen, ndist);
        if (e < 0 || (e > 0 && ndist - distcode.count[0] != 1)) return false;
        return codes(lencode, distcode);
    }
    bool run()
    {
        int last;
        do {
            last = bits(1);
            int type = bits(2);
            if (err) return false;
            bool ok = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : false;
            if (!ok) return false;
        } while (!last);
        return true;
    }
};

/* ── Files (Windows) ───────────────────────────────────────────────────── */

#ifdef _WIN32
std::wstring W(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string U(const std::wstring &w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
bool exists(const std::string &p) { return GetFileAttributesW(W(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_dir(const std::string &p)
{
    DWORD a = GetFileAttributesW(W(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool mkdirs(const std::string &p)
{
    if (p.empty() || is_dir(p)) return true;
    size_t s = p.find_last_of("\\/", p.size() - 2);
    if (s != std::string::npos && s > 2) mkdirs(p.substr(0, s));
    return CreateDirectoryW(W(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
std::vector<std::string> list_dir(const std::string &dir)
{
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) out.push_back(U(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
bool remove_any(const std::string &p)
{
    if (!exists(p)) return true;
    if (!is_dir(p)) {
        SetFileAttributesW(W(p).c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(W(p).c_str()) != 0;
    }
    bool ok = true;
    for (const std::string &n : list_dir(p)) ok = remove_any(p + "\\" + n) && ok;
    return RemoveDirectoryW(W(p).c_str()) && ok;
}
bool copy_any(const std::string &src, const std::string &dst)
{
    if (!is_dir(src)) return CopyFileW(W(src).c_str(), W(dst).c_str(), FALSE) != 0;
    bool ok = mkdirs(dst);
    for (const std::string &n : list_dir(src)) ok = copy_any(src + "\\" + n, dst + "\\" + n) && ok;
    return ok;
}
bool move_any(const std::string &src, const std::string &dst)
{
    return MoveFileExW(W(src).c_str(), W(dst).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
#else
bool exists(const std::string &p) { return SDL_GetPathInfo(p.c_str(), nullptr); }
bool is_dir(const std::string &p) { SDL_PathInfo i; return SDL_GetPathInfo(p.c_str(), &i) && i.type == SDL_PATHTYPE_DIRECTORY; }
bool mkdirs(const std::string &p) { return SDL_CreateDirectory(p.c_str()); }
std::vector<std::string> list_dir(const std::string &) { return {}; }
bool remove_any(const std::string &) { return false; }
bool copy_any(const std::string &, const std::string &) { return false; }
bool move_any(const std::string &, const std::string &) { return false; }
#endif

std::string lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool ends_with(const std::string &s, const char *e)
{
    size_t n = strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}

const char k_launcher[] = "OpenTricky.exe";
const char k_game[] = "SSX Tricky.exe";

/* What an update never writes, removes or backs up: the player's things and the updater's own. */
bool never_touched(const std::string &name)
{
    std::string n = lower(name);
    static const char *const names[] = { "settings.ini", "update-cache.txt", "update-cache.txt.tmp", "old", "update-tmp",
                                         "saves", "crashreports", "hd-pack", "portable.txt", "opentricky.exe.old", "hdd" };
    for (const char *x : names)
        if (n == x) return true;
    static const char *const ext[] = { ".ini", ".log", ".iso", ".xiso", ".sav", ".dmp" };
    for (const char *e : ext)
        if (ends_with(n, e)) return true;
    return false;
}

void set_fail(OtInstallState &st, const char *fail, const std::string &detail)
{
    std::lock_guard<std::mutex> lk(st.m);
    st.ok = false;
    st.fail = fail;
    st.detail = detail;
}

/* ── Putting top-level entries in place, with a way back ─────────────── */

struct Op {
    std::string name;
    std::string src;            /* "" = remove */
    bool copy = false;          /* copy from src (restore) instead of moving it (update) */
};

bool apply_ops(const std::string &base, const std::vector<Op> &ops, const std::string &backup,
               const std::string &version_now, const std::string &fail_at, std::string &detail)
{
    /* 1. The backup of what is there now (nothing changed yet). */
    remove_any(backup);
    if (!mkdirs(backup)) { detail = "cannot make " + backup; return false; }
    std::string manifest = "OpenTricky backup 1\nversion " + version_now + "\n";
    std::vector<bool> had(ops.size(), false);
    for (size_t i = 0; i < ops.size(); i++) {
        std::string cur = base + ops[i].name;
        if (!exists(cur)) { manifest += "new " + ops[i].name + "\n"; continue; }
        if (!copy_any(cur, backup + "\\" + ops[i].name)) { detail = "cannot back up " + ops[i].name; return false; }
        had[i] = true;
        manifest += "had " + ops[i].name + "\n";
    }
    SDL_IOStream *io = SDL_IOFromFile((backup + "\\manifest.txt").c_str(), "wb");
    if (!io || SDL_WriteIO(io, manifest.data(), manifest.size()) != manifest.size()) {
        if (io) SDL_CloseIO(io);
        detail = "cannot write the backup's manifest";
        return false;
    }
    SDL_CloseIO(io);

    /* 2. Each entry in place; the first failure puts back all those touched. */
    size_t touched = 0;
    bool ok = true;
    for (; touched < ops.size() && ok; touched++) {
        const Op &op = ops[touched];
        std::string cur = base + op.name;
        if (!fail_at.empty() && lower(op.name) == lower(fail_at)) { detail = "test failure at " + op.name; ok = false; continue; }
        if (lower(op.name) == lower(k_launcher)) {
            /* The running launcher cannot be overwritten, but it can be renamed. */
            remove_any(cur + ".old");
            if (exists(cur) && !move_any(cur, cur + ".old")) { detail = "cannot rename the running launcher"; ok = false; continue; }
        } else if (exists(cur) && !remove_any(cur)) {
            detail = "cannot replace " + op.name + " (in use?)";
            ok = false;
            continue;
        }
        if (op.src.empty()) continue;
        if (!(op.copy ? copy_any(op.src, cur) : move_any(op.src, cur))) { detail = "cannot put " + op.name + " in place"; ok = false; }
    }
    if (ok) return true;

    /* 3. Back: every touched entry as it was. */
    for (size_t i = touched; i-- > 0;) {
        const Op &op = ops[i];
        std::string cur = base + op.name;
        if (lower(op.name) == lower(k_launcher)) {
            if (exists(cur + ".old")) {
                remove_any(cur);
                move_any(cur + ".old", cur);
            }
            continue;
        }
        bool gone = remove_any(cur);      /* fails on a file still in use: then it is the old one */
        if (had[i] && (gone || is_dir(cur))) copy_any(backup + "\\" + op.name, cur);
    }
    return false;
}

std::vector<Op> stage_ops(const std::string &stage)
{
    std::vector<Op> ops;
    for (const std::string &n : list_dir(stage)) {
        if (never_touched(n)) continue;
        Op op;
        op.name = n;
        op.src = stage + "\\" + n;
        ops.push_back(op);
    }
    /* The launcher last: the rest is in place before it changes. */
    std::stable_partition(ops.begin(), ops.end(), [](const Op &o) { return lower(o.name) != lower(k_launcher); });
    return ops;
}

std::string hex_of_line(const std::string &sums, const std::string &name)
{
    size_t p = 0;
    while (p < sums.size()) {
        size_t q = sums.find('\n', p);
        if (q == std::string::npos) q = sums.size();
        std::string line = sums.substr(p, q - p);
        p = q + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 66) continue;
        std::string h = lower(line.substr(0, 64)), rest = line.substr(64);
        size_t a = rest.find_first_not_of(" \t*");
        if (a == std::string::npos || rest.substr(a) != name) continue;
        if (h.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
        return h;
    }
    return "";
}

} // namespace

/* ── Public ────────────────────────────────────────────────────────────── */

std::string ot_sha256_file(const std::string &path)
{
    SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return "";
    Sha256 s;
    std::vector<char> buf(1 << 20);
    size_t n;
    while ((n = SDL_ReadIO(io, buf.data(), buf.size())) > 0) s.add(buf.data(), n);
    SDL_CloseIO(io);
    return s.hex();
}

/* Signature of the release (Ed25519 over the zip's SHA-256, version and
 * channel), once the project has its key. Until then, the SHA-256 listed in
 * the release's SHA256SUMS.txt is the check. */
static bool ot_install_signature_ok(const std::string & /*zip*/, const std::string & /*sha256*/)
{
    return true;
}

bool ot_unzip(const std::string &zip, const std::string &dir, std::string &error)
{
    std::string z;
    if (!ot_read_file(zip, z, (size_t)1 << 31)) { error = "cannot read the zip"; return false; }
    const uint8_t *d = (const uint8_t *)z.data();
    size_t n = z.size();
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= n ? (uint32_t)(d[o] | d[o + 1] << 8) : 0; };
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= n ? (uint32_t)(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24) : 0; };
    /* End of central directory (the last 64 KB, comment included). */
    size_t e = std::string::npos;
    if (n >= 22) {
        size_t stop = n > 65557 ? n - 65557 : 0;
        for (size_t i = n - 22 + 1; i-- > stop;)
            if (u32(i) == 0x06054b50) { e = i; break; }
    }
    if (e == std::string::npos) { error = "not a zip"; return false; }
    uint32_t count = u16(e + 10), cd = u32(e + 16);
    struct Entry { std::string name; uint32_t method, crc, csize, usize, local; };
    std::vector<Entry> ents;
    size_t p = cd;
    for (uint32_t i = 0; i < count; i++) {
        if (u32(p) != 0x02014b50) { error = "damaged zip directory"; return false; }
        Entry en;
        en.method = u16(p + 10);
        en.crc = u32(p + 16);
        en.csize = u32(p + 20);
        en.usize = u32(p + 24);
        uint32_t nl = u16(p + 28), el = u16(p + 30), cl = u16(p + 32);
        en.local = u32(p + 42);
        if (p + 46 + nl > n) { error = "damaged zip directory"; return false; }
        en.name.assign((const char *)d + p + 46, nl);
        for (char &c : en.name) if (c == '\\') c = '/';
        ents.push_back(en);
        p += 46 + nl + el + cl;
    }
    /* One folder holding everything (OpenTricky-x.y.z-win64/): its content is the program. */
    std::string top;
    size_t slash = ents.empty() ? std::string::npos : ents[0].name.find('/');
    if (slash != std::string::npos) {
        top = ents[0].name.substr(0, slash + 1);
        for (const Entry &en : ents)
            if (en.name.compare(0, top.size(), top) != 0) { top.clear(); break; }
    }
    if (!mkdirs(dir)) { error = "cannot make " + dir; return false; }
    for (const Entry &en : ents) {
        std::string rel = en.name.substr(top.size());
        if (rel.empty()) continue;
        /* No way out of the folder: no absolute path, drive or "..". */
        if (rel[0] == '/' || rel.find(':') != std::string::npos || ("/" + rel + "/").find("/../") != std::string::npos) {
            error = "unsafe path in the zip: " + rel;
            return false;
        }
        std::string out = dir + "\\";
        for (char c : rel) out += c == '/' ? '\\' : c;
        if (rel.back() == '/') { if (!mkdirs(out.substr(0, out.size() - 1))) { error = "cannot make a folder"; return false; } continue; }
        size_t lp = en.local;
        if (u32(lp) != 0x04034b50) { error = "damaged zip entry " + rel; return false; }
        size_t data = lp + 30 + u16(lp + 26) + u16(lp + 28);
        if (data + en.csize > n) { error = "zip cut short at " + rel; return false; }
        std::vector<uint8_t> buf;
        if (en.method == 0) {
            buf.assign(d + data, d + data + en.csize);
        } else if (en.method == 8) {
            buf.reserve(en.usize);
            Inflate inf(d + data, en.csize, buf, en.usize);
            if (!inf.run()) { error = "damaged data in " + rel; return false; }
        } else {
            error = "unknown compression in " + rel;
            return false;
        }
        if (buf.size() != en.usize || crc32_of(buf.data(), buf.size()) != en.crc) { error = "damaged data in " + rel; return false; }
        size_t s = out.find_last_of('\\');
        if (s != std::string::npos) mkdirs(out.substr(0, s));
        SDL_IOStream *io = SDL_IOFromFile(out.c_str(), "wb");
        if (!io) { error = "cannot write " + rel; return false; }
        bool ok = buf.empty() || SDL_WriteIO(io, buf.data(), buf.size()) == buf.size();
        if (!SDL_CloseIO(io) || !ok) { error = "cannot write " + rel; return false; }
    }
    return true;
}

bool ot_install_writable(const std::string &base)
{
#ifdef _WIN32
    HANDLE h = CreateFileW(W(base + "update-write-test.tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
#else
    (void)base;
    return false;
#endif
}

bool ot_install_game_running(const std::string &base)
{
#ifdef _WIN32
    HANDLE h = CreateFileW(W(base + k_game).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return false; }
    return GetLastError() == ERROR_SHARING_VIOLATION;
#else
    (void)base;
    return false;
#endif
}

bool ot_install_has_backup(const std::string &base, std::string &version)
{
    std::string m;
    if (!ot_read_file(base + "old\\manifest.txt", m, 1 << 20)) return false;
    if (m.compare(0, 19, "OpenTricky backup 1") != 0) return false;
    size_t v = m.find("\nversion ");
    if (v == std::string::npos) return false;
    size_t e = m.find('\n', v + 9);
    version = m.substr(v + 9, e == std::string::npos ? std::string::npos : e - v - 9);
    return true;
}

void ot_install_cleanup(const std::string &base)
{
    if (exists(base + "update-tmp")) remove_any(base + "update-tmp");
    /* The launcher that installed us may still be closing: a few tries. */
    std::string old = base + k_launcher + std::string(".old");
    for (int i = 0; i < 30 && exists(old); i++) {
        if (remove_any(old)) break;
        SDL_Delay(100);
    }
}

void ot_install_update(const OtInstallRequest &req, OtInstallState &st)
{
    const std::string tmp = req.base + "update-tmp", stage = tmp + "\\stage", backup = tmp + "\\backup";
    auto finish = [&](bool ok) {
        if (ok) { std::lock_guard<std::mutex> lk(st.m); st.ok = true; st.fail.clear(); }
        else remove_any(tmp);           /* nothing of a stopped update stays */
        st.done = true;
    };
    st.step = 1;
    remove_any(tmp);
    if (!mkdirs(tmp)) { set_fail(st, "folder", "cannot make " + tmp); return finish(false); }

    /* 1. SHA256SUMS.txt, then the zip. */
    OtHttpRequest q;
    q.user_agent = req.user_agent;
    q.allow_loopback_http = req.allow_loopback_http;
    q.timeout_ms = 20000;
    q.cancel = &st.cancel;
    q.url = req.sums_url;
    q.max_body = 1 << 16;
    OtHttp sums = q.url.empty() ? OtHttp{} : ot_http_get(q);
    if (req.sums_url.empty() || sums.status != 200 || !sums.error.empty()) {
        set_fail(st, req.sums_url.empty() ? "verify" : "download",
                 req.sums_url.empty() ? "no SHA256SUMS.txt in the release" : "SHA256SUMS.txt: " + (sums.error.empty() ? "HTTP " + std::to_string(sums.status) : sums.error));
        return finish(false);
    }
    std::string want = hex_of_line(sums.body, req.zip_name);
    if (want.empty()) { set_fail(st, "verify", req.zip_name + " is not listed in SHA256SUMS.txt"); return finish(false); }
    std::string zip = tmp + "\\" + req.zip_name;
    q.url = req.zip_url;
    q.file = zip;
    st.total = req.zip_size;
    q.progress = [&st](long long got, long long total) {
        st.got = got;
        if (total > 0) st.total = total;
    };
    OtHttp z = ot_http_get(q);
    if (z.status != 200 || !z.error.empty() || (req.zip_size > 0 && z.received != req.zip_size)) {
        set_fail(st, "download", req.zip_name + ": " + (!z.error.empty() ? z.error : z.status != 200 ? "HTTP " + std::to_string(z.status) : "size differs"));
        return finish(false);
    }

    /* 2. The check. */
    st.step = 2;
    std::string got = ot_sha256_file(zip);
    if (got != want) { set_fail(st, "verify", "SHA-256 " + got + " instead of " + want); return finish(false); }
    if (!ot_install_signature_ok(zip, got)) { set_fail(st, "verify", "signature"); return finish(false); }
    std::string err;
    if (!ot_unzip(zip, stage, err)) { set_fail(st, "verify", err); return finish(false); }
    if (!exists(stage + "\\" + k_game) || !exists(stage + "\\" + k_launcher) || !is_dir(stage + "\\ui")) {
        set_fail(st, "verify", "the zip does not hold SSX Tricky.exe, OpenTricky.exe and ui\\");
        return finish(false);
    }

    /* 3. In place. */
    st.step = 3;
    if (ot_install_game_running(req.base)) { set_fail(st, "busy", "the game is running"); return finish(false); }
    std::string detail;
    if (!apply_ops(req.base, stage_ops(stage), backup, req.version_now, req.fail_at, detail)) {
        set_fail(st, "install", detail);
        return finish(false);
    }
    /* The backup becomes old\ (the previous version, for Restore). */
    remove_any(req.base + "old");
    if (!move_any(backup, req.base + "old") && !copy_any(backup, req.base + "old")) {
        std::lock_guard<std::mutex> lk(st.m);
        st.detail = "installed, but the previous version could not be kept in old\\";
    }
    remove_any(stage);
    remove_any(zip);
    st.step = 4;
    finish(true);
}

void ot_install_restore(const OtInstallRequest &req, OtInstallState &st)
{
    const std::string old = req.base + "old", tmp = req.base + "update-tmp", undo = tmp + "\\undo";
    auto finish = [&](bool ok) {
        if (ok) { std::lock_guard<std::mutex> lk(st.m); st.ok = true; st.fail.clear(); }
        st.done = true;
    };
    st.step = 3;
    std::string m;
    if (!ot_read_file(old + "\\manifest.txt", m, 1 << 20)) { set_fail(st, "install", "no previous version in old\\"); return finish(false); }
    std::vector<Op> ops;
    size_t p = 0;
    while (p < m.size()) {
        size_t q = m.find('\n', p);
        if (q == std::string::npos) q = m.size();
        std::string line = m.substr(p, q - p);
        p = q + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Op op;
        if (line.rfind("had ", 0) == 0) { op.name = line.substr(4); op.src = old + "\\" + op.name; op.copy = true; }
        else if (line.rfind("new ", 0) == 0) op.name = line.substr(4);
        else continue;
        if (op.name.empty() || never_touched(op.name) || op.name.find_first_of("\\/:") != std::string::npos) continue;
        ops.push_back(op);
    }
    std::stable_partition(ops.begin(), ops.end(), [](const Op &o) { return lower(o.name) != lower(k_launcher); });
    if (ot_install_game_running(req.base)) { set_fail(st, "busy", "the game is running"); return finish(false); }
    mkdirs(tmp);
    std::string detail;
    if (!apply_ops(req.base, ops, undo, req.version_now, req.fail_at, detail)) {
        set_fail(st, "install", detail);
        return finish(false);
    }
    remove_any(old);
    remove_any(tmp + "\\undo");
    st.step = 4;
    finish(true);
}
