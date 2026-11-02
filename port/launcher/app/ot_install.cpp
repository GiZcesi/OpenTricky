/*
 * ot_install.cpp -- see ot_install.h.
 */
#include "ot_install.h"
#include "ot_sign.h"
#include "ot_update.h"
#include "ot_util.h"

#include "miniz.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
/* A junction or a symbolic link (to a folder or a file), `a` its attributes.
 * The helpers below never go through one: removed, only the link goes, never
 * what it points to; it is never copied. Other reparse points (OneDrive's
 * files on demand, deduplicated files) are plain files and folders here. */
bool is_link(const std::string &p, DWORD a)
{
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(p).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;         /* cannot tell: never gone through */
    FindClose(h);
    return fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
}
/* What `p` is: 0 nothing, 1 a file, 2 a folder, 3 a junction or a link. */
int kind_of(const std::string &p)
{
    DWORD a = GetFileAttributesW(W(p).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    if (is_link(p, a)) return 3;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
}
/* The size of a file (kind 1), -1 for anything else. */
long long file_size(const std::string &p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(W(p).c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        is_link(p, d.dwFileAttributes))
        return -1;
    return (long long)d.nFileSizeHigh << 32 | d.nFileSizeLow;
}
/* `p` (a file, or a folder: `folder`; never gone through) could be removed
 * now: the right to (DELETE, or the right to remove what is in its folder:
 * the system weighs both) and no one holding it open without sharing its
 * removal. Opened for that only: nothing of it is read. */
bool deletable(const std::string &p, bool folder)
{
    HANDLE h = CreateFileW(W(p).c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | (folder ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
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
    DWORD a = GetFileAttributesW(W(p).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (is_link(p, a))                                  /* the link alone */
        return (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(W(p).c_str()) != 0 : DeleteFileW(W(p).c_str()) != 0;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(W(p).c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(W(p).c_str()) != 0;
    }
    bool ok = true;
    for (const std::string &n : list_dir(p)) ok = remove_any(p + "\\" + n) && ok;
    return RemoveDirectoryW(W(p).c_str()) && ok;
}
bool copy_any(const std::string &src, const std::string &dst)
{
    DWORD a = GetFileAttributesW(W(src).c_str());
    if (is_link(src, a)) return false;
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return CopyFileW(W(src).c_str(), W(dst).c_str(), FALSE) != 0;
    bool ok = mkdirs(dst);
    for (const std::string &n : list_dir(src)) ok = copy_any(src + "\\" + n, dst + "\\" + n) && ok;
    return ok;
}
bool move_any(const std::string &src, const std::string &dst)
{
    return MoveFileExW(W(src).c_str(), W(dst).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
/* A file's data on the disk now (a power cut after this keeps it); best effort. */
void flush_file(const std::string &p)
{
    HANDLE h = CreateFileW(W(p).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FlushFileBuffers(h);
    CloseHandle(h);
}
void flush_tree(const std::string &p)
{
    DWORD a = GetFileAttributesW(W(p).c_str());
    if (is_link(p, a)) return;
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { flush_file(p); return; }
    for (const std::string &n : list_dir(p)) flush_tree(p + "\\" + n);
}
/* A small text file written whole or not at all, on the disk when this returns. */
bool write_durable(const std::string &path, const std::string &text)
{
    std::string tmp = path + ".tmp";
    HANDLE h = CreateFileW(W(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr) && put == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    ok = ok && move_any(tmp, path);
    if (!ok) DeleteFileW(W(tmp).c_str());
    return ok;
}
#else
bool exists(const std::string &p) { return SDL_GetPathInfo(p.c_str(), nullptr); }
bool is_dir(const std::string &p) { SDL_PathInfo i; return SDL_GetPathInfo(p.c_str(), &i) && i.type == SDL_PATHTYPE_DIRECTORY; }
int kind_of(const std::string &p) { return !exists(p) ? 0 : is_dir(p) ? 2 : 1; }
long long file_size(const std::string &) { return -1; }
bool deletable(const std::string &, bool) { return false; }
bool mkdirs(const std::string &p) { return SDL_CreateDirectory(p.c_str()); }
std::vector<std::string> list_dir(const std::string &) { return {}; }
bool remove_any(const std::string &) { return false; }
bool copy_any(const std::string &, const std::string &) { return false; }
bool move_any(const std::string &, const std::string &) { return false; }
void flush_file(const std::string &) {}
void flush_tree(const std::string &) {}
bool write_durable(const std::string &, const std::string &) { return false; }
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
const char k_bin[] = "bin";
/* What an install of 0.1.1 had beside OpenTricky.exe and is now in bin\.
 * The release zips have had no copies of them there since 0.1.2 (the updater
 * of 0.1.1 cannot install a newer version); a zip that has them does not get
 * them installed. A new version unzipped over 0.1.1 leaves them: once bin\
 * holds the game, the files that are exactly 0.1.1's go (k_files_011,
 * ot_install_tidy_root, or the update itself); anything else of those names
 * is the player's and stays. */
const char *const k_program_copies[] = { "SSX Tricky.exe", "ui", "THIRD-PARTY-LICENSES.txt" };

/* Those copies as the published 0.1.1 zip has them: each file's path, size
 * and SHA-256, then their folders, deepest first. */
struct ShippedFile {
    const char *path;
    long long size;
    const char *sha256;
};
#include "ot_install_011.inc"

/* The game in `dir`: SSX Tricky.exe and ui\. */
bool has_game(const std::string &dir)
{
    return exists(dir + "\\" + k_game) && !is_dir(dir + "\\" + k_game) && is_dir(dir + "\\ui");
}

/* `rel`, below the program's folder `base`, against the file 0.1.1 had
 * there: 1 the same bytes, 0 another file, -1 cannot tell now -- of 0.1.1's
 * name and size, but it could not be removed anyway (in use, or protected),
 * so it is not read: a copy that cannot go costs nothing at each start (the
 * game is 165 MB). */
int shipped_011(const std::string &base, const std::string &rel)
{
    const long long size = file_size(base + rel);
    for (const ShippedFile &f : k_files_011)
        if (f.size == size && lower(rel) == lower(f.path))
            return !deletable(base + rel, false) ? -1 : ot_sha256_file(base + rel) == f.sha256 ? 1 : 0;
    return 0;
}

bool shipped_dir_011(const std::string &rel)
{
    for (const char *d : k_dirs_011)
        if (lower(rel) == lower(d)) return true;
    return false;
}

/* The two pages that make a copy of ui\ 0.1.1's (copy_011): looked at
 * first, removed last (remove_011). */
const char *const k_pages_011[] = { "home.rml", "launcher.rcss" };

/* `rel` is one of those pages, directly in a copy ("ui\home.rml"). */
bool page_011(const std::string &rel)
{
    const size_t s = rel.find('\\');
    if (s == std::string::npos || rel.find('\\', s + 1) != std::string::npos) return false;
    for (const char *p : k_pages_011)
        if (lower(rel.substr(s + 1)) == p) return true;
    return false;
}

/* `rel` and every folder above it, below `base`, are folders: none is a
 * junction or a link. */
bool real_dir(const std::string &base, const std::string &rel)
{
    bool real = kind_of(base + rel) == 2;
    for (size_t s = rel.find('\\'); real && s != std::string::npos; s = rel.find('\\', s + 1))
        real = kind_of(base + rel.substr(0, s)) == 2;
    return real;
}

/* A copy of the program beside OpenTricky.exe (`rel`, one of
 * k_program_copies, then what is inside it): its files that are 0.1.1's
 * (`ours`), how many other entries it holds (`others`: the player's files
 * and folders, junctions and links, never gone through), and how many of
 * its entries cannot be removed now (`stuck`: files left unread, folders in
 * use). A copy of ui\ is looked into only when its two pages are 0.1.1's:
 * otherwise nothing in it is the program's. */
void copy_011(const std::string &base, const std::string &rel, std::vector<std::string> &ours, int &others, int &stuck)
{
    const int k = kind_of(base + rel);
    if (k == 0) return;
    if (k == 1) {
        const int s = shipped_011(base, rel);
        if (s > 0) ours.push_back(rel);
        else if (s < 0) stuck++;
        else others++;
        return;
    }
    if (k != 2 || !shipped_dir_011(rel)) {
        others++;
        return;
    }
    if (rel.find('\\') == std::string::npos) {
        const int p0 = shipped_011(base, rel + "\\" + k_pages_011[0]), p1 = shipped_011(base, rel + "\\" + k_pages_011[1]);
        if (p0 < 0 || p1 < 0) { stuck++; return; }
        if (!p0 || !p1) { others++; return; }
    }
    if (!deletable(base + rel, true)) stuck++;          /* in use: what is in it may go, not the folder */
    for (const std::string &n : list_dir(base + rel)) copy_011(base, rel + "\\" + n, ours, others, stuck);
}

/* Removes what copy_011 found to be the program's in the copy `name`: its
 * files, then its folders left empty, deepest first (each a folder, as are
 * all those above it: never one seen through a junction), and only then the
 * two pages of ui\ and ui\ itself, once nothing else of 0.1.1's is left in
 * it (`stuck`: copy_011's). The pages are what makes a start look into ui\:
 * a pass that a file or a folder in use cut short is taken up again at the
 * next start. False when something of 0.1.1's stays. */
bool remove_011(const std::string &base, const std::string &name, const std::vector<std::string> &ours, int stuck)
{
    bool ok = true;
    std::vector<std::string> pages;
    for (const std::string &r : ours)
        if (page_011(r)) pages.push_back(r);
        else ok = remove_any(base + r) && ok;
#ifdef _WIN32
    const std::string below = lower(name) + "\\";
    for (const char *d : k_dirs_011) {
        const std::string rel = d;
        if (lower(rel).compare(0, below.size(), below) != 0 || !real_dir(base, rel)) continue;
        /* Only when empty: one with the player's files stays; one left empty is in use. */
        if (!RemoveDirectoryW(W(base + rel).c_str()) && list_dir(base + rel).empty()) ok = false;
    }
#endif
    if (!ok || stuck) return false;
    for (const std::string &r : pages) ok = remove_any(base + r) && ok;
#ifdef _WIN32
    if (shipped_dir_011(name) && real_dir(base, name)) RemoveDirectoryW(W(base + name).c_str());   /* only when empty */
#endif
    return ok;
}

/* What an update never writes, removes or backs up: the player's things and the updater's own. */
bool never_touched(const std::string &name)
{
    std::string n = lower(name);
    static const char *const names[] = { "settings.ini", "update-cache.txt", "update-cache.txt.tmp", "theme-order.txt", "old", "update-tmp",
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

/* ── Putting entries in place, with a way back ─────────────────────────── */

/* An entry is a name of the program's folder, or "bin\<name>": bin\ is
 * updated one entry at a time, so what the player put there stays. */
struct Op {
    std::string name;
    std::string src;            /* "" = remove */
    bool copy = false;          /* copy from src (restore) instead of moving it (update) */
};

/* "bin\x" -> "bin"; "x" -> "". */
std::string parent_of(const std::string &name)
{
    size_t s = name.find_last_of("\\/");
    return s == std::string::npos ? std::string() : name.substr(0, s);
}

/* A name read from a manifest: one entry of the program's folder, or one of
 * bin\ (one level); never a player's file, never a way out of the folder. */
bool manifest_name_ok(const std::string &name)
{
    std::string leaf = name;
    size_t s = name.find_first_of("\\/");
    if (s != std::string::npos) {
        if (lower(name.substr(0, s)) != k_bin) return false;
        leaf = name.substr(s + 1);
    }
    if (leaf.empty() || leaf == "." || leaf == ".." || leaf.find_first_of("\\/:") != std::string::npos) return false;
    return !never_touched(leaf);
}

/* The journal of an install in progress, update-tmp\installing.txt: on the
 * disk before the first entry changes, gone once the install is over (done,
 * or put back). A start that finds it (the launcher killed, a power cut)
 * undoes the install from its backup, or finishes it when every entry was
 * already in place (ot_install_recover).
 *   kind    update | restore
 *   backup  backup | undo      (the folder of update-tmp\ holding the backup)
 *   state   applying           (entries are being replaced)
 *           applied            (all in place; old\ not made yet)
 *   tries   n                  (starts that could not put it right) */
const char k_journal_magic[] = "OpenTricky install 1";
const int k_recover_tries = 3;

std::string journal_path(const std::string &base) { return base + "update-tmp\\installing.txt"; }

bool journal_write(const std::string &base, const std::string &kind, const std::string &backup_leaf, const char *state, int tries = 0)
{
    return write_durable(journal_path(base), std::string(k_journal_magic) + "\nkind " + kind + "\nbackup " + backup_leaf +
                                                 "\nstate " + state + "\ntries " + std::to_string(tries) + "\n");
}

void journal_remove(const std::string &base)
{
#ifdef _WIN32
    DeleteFileW(W(journal_path(base)).c_str());
#endif
}

/* The value of "key value" in a journal or a manifest, "" if absent. */
std::string text_value(const std::string &t, const char *key)
{
    const std::string k = std::string(key) + " ";
    size_t p = 0;
    while (p < t.size()) {
        size_t q = t.find('\n', p);
        if (q == std::string::npos) q = t.size();
        std::string line = t.substr(p, q - p);
        p = q + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, k.size(), k) == 0) return line.substr(k.size());
    }
    return "";
}

/* old\ and the update's other leftovers once a Restore is over: bin\ goes
 * when nothing is left in it; with a file of the player's it stays (an older
 * version does not look at it). A restored version in bin\ keeps it. */
void restore_tidy_bin(const std::string &base, std::string &detail)
{
    if (is_dir(base + k_bin) && !has_game(base + k_bin)) {
        if (list_dir(base + k_bin).empty()) remove_any(base + k_bin);
        else detail = "bin\\ kept: it holds files that are not the program's";
    }
}

/* Tests: the process ends here, at once, as if killed (a cut install). */
void test_kill(const std::string &kill_at, const std::string &where)
{
    if (kill_at.empty() || lower(kill_at) != lower(where)) return;
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0xC0DE);
#endif
}

bool apply_ops(const std::string &base, const std::vector<Op> &ops, const std::string &backup,
               const std::string &version_now, const std::string &fail_at, const std::string &kill_at,
               const char *kind, std::string &detail)
{
    /* 1. The backup of what is there now (nothing changed yet), on the disk
     * before anything changes: it is the way back after a power cut too. */
    remove_any(backup);
    if (!mkdirs(backup)) { detail = "cannot make " + backup; return false; }
    std::string manifest = "OpenTricky backup 1\nversion " + version_now + "\n";
    std::vector<bool> had(ops.size(), false);
    for (size_t i = 0; i < ops.size(); i++) {
        std::string cur = base + ops[i].name;
        if (!exists(cur)) { manifest += "new " + ops[i].name + "\n"; continue; }
        if (!parent_of(ops[i].name).empty()) mkdirs(backup + "\\" + parent_of(ops[i].name));
        if (!copy_any(cur, backup + "\\" + ops[i].name)) { detail = "cannot back up " + ops[i].name; return false; }
        flush_tree(backup + "\\" + ops[i].name);
        had[i] = true;
        manifest += "had " + ops[i].name + "\n";
    }
    if (!write_durable(backup + "\\manifest.txt", manifest)) { detail = "cannot write the backup's manifest"; return false; }
    const std::string leaf = backup.substr(backup.find_last_of("\\/") + 1);
    if (!journal_write(base, kind, leaf, "applying")) { detail = "cannot write the install journal"; return false; }

    /* 2. Each entry in place; the first failure puts back all those touched. */
    size_t touched = 0;
    bool ok = true;
    for (; touched < ops.size() && ok; touched++) {
        const Op &op = ops[touched];
        std::string cur = base + op.name;
        if (!fail_at.empty() && (lower(op.name) == lower(fail_at) || lower(parent_of(op.name)) == lower(fail_at))) {
            detail = "test failure at " + op.name;
            ok = false;
            continue;
        }
        if (lower(op.name) == lower(k_launcher)) {
            /* The running launcher cannot be overwritten, but it can be renamed. */
            remove_any(cur + ".old");
            if (exists(cur) && !move_any(cur, cur + ".old")) { detail = "cannot rename the running launcher"; ok = false; continue; }
        } else if (exists(cur) && !remove_any(cur)) {
            detail = "cannot replace " + op.name + " (in use?)";
            ok = false;
            continue;
        }
        test_kill(kill_at, op.name);         /* the old entry gone, the new one not there yet */
        if (op.src.empty()) continue;
        if (!parent_of(op.name).empty()) mkdirs(base + parent_of(op.name));
        if (!(op.copy ? copy_any(op.src, cur) : move_any(op.src, cur))) { detail = "cannot put " + op.name + " in place"; ok = false; }
    }
    if (ok) {
        /* Not written: the journal still says "applying", and the launcher
         * (the last entry) is the new one, which says the same (ot_install_recover). */
        journal_write(base, kind, leaf, "applied");
        test_kill(kill_at, "@applied");
        return true;
    }

    /* 3. Back: every touched entry as it was. The journal stays when one
     * could not be put back: the next start tries again. */
    bool back = true;
    for (size_t i = touched; i-- > 0;) {
        const Op &op = ops[i];
        std::string cur = base + op.name;
        if (lower(op.name) == lower(k_launcher)) {
            if (exists(cur + ".old")) {
                remove_any(cur);
                back = move_any(cur + ".old", cur) && back;
            }
            continue;
        }
        bool gone = remove_any(cur);      /* fails on a file still in use: then it is the old one */
        if (had[i] && (gone || is_dir(cur))) back = copy_any(backup + "\\" + op.name, cur) && back;
        else if (!had[i] && !gone) back = false;
    }
#ifdef _WIN32
    /* Only when empty: a bin\ this attempt made. Never a junction or a link,
     * which RemoveDirectoryW removes whatever it points to holds. */
    if (kind_of(base + k_bin) == 2) RemoveDirectoryW(W(base + k_bin).c_str());
#endif
    if (back) journal_remove(base);
    else detail += "; not all of it could be put back (next start)";
    return false;
}

/* What an update does with the unpacked zip. Its game is in bin\ (0.1.2 and
 * later) or beside OpenTricky.exe (older). With the game in bin\:
 *  - copies of bin\'s entries beside it are not installed, and the old copies
 *    in the program's folder are removed by the update itself (backed up:
 *    Restore puts them back) when they are 0.1.1's through and through
 *    (copy_011); anything else of those names is the player's, and one that
 *    cannot be removed now is left to the tidy of a next start;
 *  - bin\, when the program's folder has one, is updated one entry at a time. */
std::vector<Op> stage_ops(const std::string &stage, const std::string &base)
{
    std::vector<Op> ops;
    const std::string sbin = stage + "\\" + k_bin;
    const bool game_in_bin = has_game(sbin);
    for (const std::string &n : list_dir(stage)) {
        if (never_touched(n)) continue;
        bool copy_of_bin = false;
        for (const char *c : k_program_copies)
            if (lower(n) == lower(c)) copy_of_bin = true;
        if (game_in_bin && copy_of_bin) continue;
        if (lower(n) == k_bin && is_dir(stage + "\\" + n) && is_dir(base + k_bin)) {
            for (const std::string &c : list_dir(stage + "\\" + n)) {
                if (never_touched(c)) continue;
                Op op;
                op.name = std::string(k_bin) + "\\" + c;
                op.src = stage + "\\" + n + "\\" + c;
                ops.push_back(op);
            }
            continue;
        }
        Op op;
        op.name = n;
        op.src = stage + "\\" + n;
        ops.push_back(op);
    }
    if (game_in_bin)
        for (const char *c : k_program_copies) {
            std::vector<std::string> ours;
            int others = 0, stuck = 0;
            copy_011(base, c, ours, others, stuck);
            if (ours.empty() || others || stuck) continue;
            Op op;
            op.name = c;                       /* src "": removed */
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

/* The largest file a release may hold once unpacked, and the whole release
 * (the game is ~170 MB): a zip that announces more is refused before
 * anything is written. */
static const mz_uint64 k_unzip_file_max = (mz_uint64)1 << 30;
static const mz_uint64 k_unzip_total_max = (mz_uint64)4 << 30;

bool ot_unzip(const std::string &zip, const std::string &dir, std::string &error)
{
    std::string z;
    if (!ot_read_file(zip, z, (size_t)1 << 31)) { error = "cannot read the zip"; return false; }
    mz_zip_archive za;
    mz_zip_zero_struct(&za);
    if (!mz_zip_reader_init_mem(&za, z.data(), z.size(), 0)) {
        error = std::string("not a zip (") + mz_zip_get_error_string(mz_zip_get_last_error(&za)) + ")";
        return false;
    }
    struct Close { mz_zip_archive *a; ~Close() { mz_zip_reader_end(a); } } close_zip{ &za };
    auto why = [&za]() {
        mz_zip_error e = mz_zip_get_last_error(&za);
        return e == MZ_ZIP_NO_ERROR ? std::string() : std::string(" (") + mz_zip_get_error_string(e) + ")";
    };

    struct Entry { mz_uint index; std::string name; mz_zip_archive_file_stat st; };
    std::vector<Entry> ents;
    mz_uint count = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < count; i++) {
        Entry en;
        en.index = i;
        if (!mz_zip_reader_file_stat(&za, i, &en.st)) { error = "damaged zip directory" + why(); return false; }
        /* The full name (the stat's copy is cut at 512 bytes). */
        mz_uint nl = mz_zip_reader_get_filename(&za, i, nullptr, 0);
        if (nl == 0) { error = "damaged zip directory"; return false; }
        std::vector<char> nb(nl);
        mz_zip_reader_get_filename(&za, i, nb.data(), nl);
        en.name.assign(nb.data(), nl - 1);
        for (char &c : en.name) if (c == '\\') c = '/';
        ents.push_back(en);
    }
    /* One folder holding everything (OpenTricky-x.y.z-win64/): its content is the program. */
    std::string top;
    size_t slash = ents.empty() ? std::string::npos : ents[0].name.find('/');
    if (slash != std::string::npos) {
        top = ents[0].name.substr(0, slash + 1);
        for (const Entry &en : ents)
            if (en.name.compare(0, top.size(), top) != 0) { top.clear(); break; }
    }
    /* Every name and size is checked before anything is written. */
    mz_uint64 total = 0;
    for (const Entry &en : ents) {
        std::string rel = en.name.substr(top.size());
        if (rel.empty()) continue;
        /* No way out of the folder: no absolute path, drive or "..". */
        if (rel[0] == '/' || rel.find(':') != std::string::npos || ("/" + rel + "/").find("/../") != std::string::npos) {
            error = "unsafe path in the zip: " + rel;
            return false;
        }
        if (rel.back() == '/') continue;
        if (en.st.m_is_encrypted || !en.st.m_is_supported || (en.st.m_method != 0 && en.st.m_method != MZ_DEFLATED)) {
            error = "unknown compression in " + rel;
            return false;
        }
        if (en.st.m_method == 0 && en.st.m_comp_size != en.st.m_uncomp_size) { error = "damaged data in " + rel; return false; }
        total += en.st.m_uncomp_size;
        if (en.st.m_uncomp_size > k_unzip_file_max || total > k_unzip_total_max) { error = "too large: " + rel; return false; }
    }
    if (!mkdirs(dir)) { error = "cannot make " + dir; return false; }
    for (const Entry &en : ents) {
        std::string rel = en.name.substr(top.size());
        if (rel.empty()) continue;
        std::string out = dir + "\\";
        for (char c : rel) out += c == '/' ? '\\' : c;
        if (rel.back() == '/') { if (!mkdirs(out.substr(0, out.size() - 1))) { error = "cannot make a folder"; return false; } continue; }
        /* miniz inflates into exactly the announced size and fails on a
         * cut, longer or damaged stream; the CRC-32 is checked again here. */
        size_t size = (size_t)en.st.m_uncomp_size;
        std::vector<uint8_t> buf(size ? size : 1);
        if (!mz_zip_reader_extract_to_mem(&za, en.index, buf.data(), size, 0)) { error = "damaged data in " + rel + why(); return false; }
        if ((mz_uint32)mz_crc32(MZ_CRC32_INIT, buf.data(), size) != en.st.m_crc32) { error = "damaged data in " + rel; return false; }
        size_t s = out.find_last_of('\\');
        if (s != std::string::npos) mkdirs(out.substr(0, s));
        SDL_IOStream *io = SDL_IOFromFile(out.c_str(), "wb");
        if (!io) { error = "cannot write " + rel; return false; }
        bool ok = size == 0 || SDL_WriteIO(io, buf.data(), size) == size;
        if (!SDL_CloseIO(io) || !ok) { error = "cannot write " + rel; return false; }
    }
    return true;
}

std::string ot_program_dir(const std::string &base)
{
    /* The interface decides, not the game: with SSX Tricky.exe gone from bin\ (an
     * antivirus took it, a partial unzip) the pages load from bin\ui\ all the same, and
     * PLAY says the game is missing, instead of a window with nothing in it. */
    return is_dir(base + k_bin + "\\ui") ? base + k_bin + "\\" : base;
}

bool ot_install_writable(const std::string &base)
{
#ifdef _WIN32
    for (const std::string &dir : { base, base + k_bin + "\\" }) {
        if (dir != base && !is_dir(base + k_bin)) continue;
        HANDLE h = CreateFileW(W(dir + "update-write-test.tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        CloseHandle(h);
    }
    return true;
#else
    (void)base;
    return false;
#endif
}

bool ot_install_game_running(const std::string &base)
{
#ifdef _WIN32
    for (const std::string &exe : { base + k_game, base + k_bin + "\\" + k_game }) {
        HANDLE h = CreateFileW(W(exe).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); continue; }
        if (GetLastError() == ERROR_SHARING_VIOLATION) return true;
    }
    return false;
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

void ot_install_tidy_root(const std::string &base, std::vector<std::string> &removed, std::vector<std::string> &kept,
                          std::vector<std::string> &failed)
{
    if (!has_game(base + k_bin)) return;               /* the game is not in bin\: nothing to tidy */
    for (const char *c : k_program_copies) {
        std::vector<std::string> ours;
        int others = 0, stuck = 0;
        copy_011(base, c, ours, others, stuck);
        if (ours.empty()) {
            if (stuck) failed.push_back(c);                 /* cannot tell now: the next start looks again */
            else if (others) kept.push_back(std::string(c) + ": not the old OpenTricky's");
            continue;
        }
        if (!remove_011(base, c, ours, stuck)) failed.push_back(c);
        else if (others) kept.push_back(std::string(c) + ": " + std::to_string(others) + (others == 1 ? " entry in it is" : " entries in it are") +
                                        " not the old OpenTricky's (" + std::to_string(ours.size()) + " of its files removed)");
        else if (kind_of(base + c) != 0) failed.push_back(c);
        else removed.push_back(c);
    }
}

std::vector<std::string> ot_install_plan(const std::string &stage, const std::string &base)
{
    std::vector<std::string> out;
    for (const Op &op : stage_ops(stage, base)) out.push_back((op.src.empty() ? "- " : "+ ") + op.name);
    return out;
}

OtRecovery ot_install_recover(const std::string &base)
{
    OtRecovery r;
    std::string j;
    if (!exists(journal_path(base))) return r;
    r.found = true;
    if (!ot_read_file(journal_path(base), j, 1 << 16) || j.compare(0, sizeof k_journal_magic - 1, k_journal_magic) != 0) {
        r.detail = "the install journal cannot be read: removed";
        journal_remove(base);
        return r;
    }
    const std::string kind = text_value(j, "kind"), leaf = text_value(j, "backup"), state = text_value(j, "state");
    const int tries = atoi(text_value(j, "tries").c_str());
    r.kind = kind;
    if ((kind != "update" && kind != "restore") || (leaf != "backup" && leaf != "undo") || (state != "applying" && state != "applied")) {
        r.detail = "the install journal is not one of this version's: removed";
        journal_remove(base);
        return r;
    }
    const std::string bdir = base + "update-tmp\\" + leaf;
    struct Ent { std::string name; bool had; };
    std::vector<Ent> ents;
    std::string m;
    bool have_manifest = ot_read_file(bdir + "\\manifest.txt", m, 1 << 20);
    r.version = have_manifest ? text_value(m, "version") : "";
    for (size_t p = 0; p < m.size();) {
        size_t q = m.find('\n', p);
        if (q == std::string::npos) q = m.size();
        std::string line = m.substr(p, q - p);
        p = q + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        bool had = line.rfind("had ", 0) == 0;
        if (!had && line.rfind("new ", 0) != 0) continue;
        Ent e{ line.substr(4), had };
        if (manifest_name_ok(e.name)) ents.push_back(e);
    }

    /* Applying, and the launcher (always the last entry) already the new
     * one: every entry is in place, so the install is finished instead. */
    bool finish = state == "applied";
    for (const Ent &e : ents)
        if (!finish && e.had && lower(e.name) == lower(k_launcher)) {
            std::string now = ot_sha256_file(base + k_launcher), was = ot_sha256_file(bdir + "\\" + k_launcher);
            finish = !now.empty() && !was.empty() && now != was;
        }

    bool ok = true;
    if (finish) {
        if (kind == "update") {
            /* The backup becomes old\ (done already when it is not there any more). */
            if (is_dir(bdir)) {
                remove_any(base + "old");
                if (!move_any(bdir, base + "old") && !copy_any(bdir, base + "old")) r.detail = "finished, but the previous version could not be kept in old\\";
            }
        } else {
            remove_any(base + "old");
            remove_any(bdir);
            restore_tidy_bin(base, r.detail);
        }
    } else if (!have_manifest) {
        ok = false;                     /* written before the journal: nothing to go back with */
        r.detail = "no backup manifest";
    } else {
        /* Back: each entry as the backup has it, the last first. The running
         * launcher is the one from before (its entry was not reached). */
        for (size_t i = ents.size(); i-- > 0;) {
            const Ent &e = ents[i];
            if (lower(e.name) == lower(k_launcher)) continue;
            std::string cur = base + e.name;
            bool gone = remove_any(cur);
            if (e.had) {
                if (!parent_of(e.name).empty()) mkdirs(base + parent_of(e.name));
                if ((gone || is_dir(cur)) && copy_any(bdir + "\\" + e.name, cur)) continue;
                ok = false;
                r.detail = "cannot put back " + e.name + " (in use?)";
            } else if (!gone) {
                ok = false;
                r.detail = "cannot remove " + e.name + " (in use?)";
            }
        }
#ifdef _WIN32
        /* Only when empty: a bin\ the cut install made; never a junction or a link (apply_ops). */
        if (kind_of(base + k_bin) == 2) RemoveDirectoryW(W(base + k_bin).c_str());
#endif
    }
    r.finished = finish;
    r.ok = ok;
    if (ok) {
        journal_remove(base);
    } else if (tries + 1 >= k_recover_tries) {
        r.gave_up = true;               /* never a loop: the player reinstalls (the notice says how) */
        journal_remove(base);
    } else {
        journal_write(base, kind, leaf, state.c_str(), tries + 1);
    }
    return r;
}

void ot_install_cleanup(const std::string &base)
{
    /* An install that could not be put right yet keeps its backup (ot_install_recover). */
    if (exists(base + "update-tmp") && !exists(journal_path(base))) remove_any(base + "update-tmp");
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
        else if (st.cancel && st.step < 3) set_fail(st, "cancel", "cancelled by the player");   /* whatever stopped the download */
        if (!ok && !exists(journal_path(req.base))) remove_any(tmp);   /* nothing of a stopped update stays, but a way back */
        st.done = true;
    };
    st.step = 1;
    if (exists(journal_path(req.base))) {
        set_fail(st, "install", "an interrupted install is not put right yet (next start)");
        return finish(false);
    }
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
    /* The signature of SHA256SUMS.txt (ot_sign.h), before a line of it is
     * read: refused when absent or wrong, once the launcher holds the key. */
    if (ot_sign_key_set(ot_release_key)) {
        if (req.sig_url.empty()) { set_fail(st, "signature", ot_sign_text(OT_SIG_ABSENT)); return finish(false); }
        q.url = req.sig_url;
        q.max_body = 1024;
        OtHttp sig = ot_http_get(q);
        if (sig.status != 200 || !sig.error.empty()) {
            set_fail(st, "download", "SHA256SUMS.txt.sig: " + (sig.error.empty() ? "HTTP " + std::to_string(sig.status) : sig.error));
            return finish(false);
        }
        OtSigResult r = ot_sign_check(sums.body.data(), sums.body.size(), sig.body.data(), sig.body.size(), ot_release_key);
        if (r != OT_SIG_OK) { set_fail(st, "signature", ot_sign_text(r)); return finish(false); }
        /* The signed list names the zip, and the zip's name its version: an
         * older signed release under a newer tag is refused. The tag's
         * suffix ("-unstable") is not part of the version (ot_sign.h). */
        if (!ot_sign_zip_is_version(req.zip_name.c_str(), req.version_new.c_str())) {
            set_fail(st, "signature", req.zip_name + " is not version " + req.version_new);
            return finish(false);
        }
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
    if (st.cancel) { set_fail(st, "cancel", "cancelled by the player"); return finish(false); }
    if (z.status != 200 || !z.error.empty() || (req.zip_size > 0 && z.received != req.zip_size)) {
        set_fail(st, "download", req.zip_name + ": " + (!z.error.empty() ? z.error : z.status != 200 ? "HTTP " + std::to_string(z.status) : "size differs"));
        return finish(false);
    }

    /* 2. The check. */
    st.step = 2;
    std::string got = ot_sha256_file(zip);
    if (got != want) { set_fail(st, "verify", "SHA-256 " + got + " instead of " + want); return finish(false); }
    std::string err;
    if (!ot_unzip(zip, stage, err)) { set_fail(st, "verify", err); return finish(false); }
    if (!exists(stage + "\\" + k_launcher) || !(has_game(stage + "\\" + k_bin) || has_game(stage))) {
        set_fail(st, "verify", "the zip does not hold OpenTricky.exe and the game (SSX Tricky.exe and ui\\, in bin\\ or beside it)");
        return finish(false);
    }

    /* 3. In place. The player's Cancel counts until here: nothing changed yet. */
    st.step = 3;
    if (st.cancel) { set_fail(st, "cancel", "cancelled by the player"); return finish(false); }
    if (ot_install_game_running(req.base)) { set_fail(st, "busy", "the game is running"); return finish(false); }
    flush_tree(stage);                  /* the new files on the disk before the old ones go */
    std::string detail;
    if (!apply_ops(req.base, stage_ops(stage, req.base), backup, req.version_now, req.fail_at, req.kill_at, "update", detail)) {
        set_fail(st, exists(journal_path(req.base)) ? "partial" : "install", detail);
        return finish(false);
    }
    /* The backup becomes old\ (the previous version, for Restore); then the
     * journal goes: the install is over. */
    remove_any(req.base + "old");
    test_kill(req.kill_at, "@old");
    if (!move_any(backup, req.base + "old") && !copy_any(backup, req.base + "old")) {
        std::lock_guard<std::mutex> lk(st.m);
        st.detail = "installed, but the previous version could not be kept in old\\";
    }
    journal_remove(req.base);
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
    /* bin\ as one entry (the update from 0.1.1 added it whole): restored one
     * entry at a time too, so a file the player put in it stays. */
    auto bin_ops = [&](bool had) {
        const std::string cur = req.base + k_bin, back = old + "\\" + k_bin;
        std::vector<std::string> was = had ? list_dir(back) : std::vector<std::string>();
        for (const std::string &c : was) {
            if (never_touched(c)) continue;
            Op op;
            op.name = std::string(k_bin) + "\\" + c;
            op.src = back + "\\" + c;
            op.copy = true;
            ops.push_back(op);
        }
        for (const std::string &c : list_dir(cur)) {
            if (never_touched(c)) continue;
            bool kept = false;
            for (const std::string &w : was)
                if (lower(w) == lower(c)) kept = true;
            if (kept) continue;
            Op op;
            op.name = std::string(k_bin) + "\\" + c;      /* src "": removed */
            ops.push_back(op);
        }
    };
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
        if (!manifest_name_ok(op.name)) continue;
        if (lower(op.name) == k_bin) { bin_ops(op.copy); continue; }
        ops.push_back(op);
    }
    std::stable_partition(ops.begin(), ops.end(), [](const Op &o) { return lower(o.name) != lower(k_launcher); });
    if (ot_install_game_running(req.base)) { set_fail(st, "busy", "the game is running"); return finish(false); }
    if (exists(journal_path(req.base))) {
        set_fail(st, "install", "an interrupted install is not put right yet (next start)");
        return finish(false);
    }
    mkdirs(tmp);
    std::string detail;
    if (!apply_ops(req.base, ops, undo, req.version_now, req.fail_at, req.kill_at, "restore", detail)) {
        set_fail(st, exists(journal_path(req.base)) ? "partial" : "install", detail);
        return finish(false);
    }
    remove_any(old);
    remove_any(tmp + "\\undo");
    {
        std::string tidy;
        restore_tidy_bin(req.base, tidy);
        if (!tidy.empty()) {
            std::lock_guard<std::mutex> lk(st.m);
            st.detail = tidy;
        }
    }
    journal_remove(req.base);
    st.step = 4;
    finish(true);
}
