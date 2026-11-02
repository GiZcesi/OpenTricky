/*
 * tidy_test.cpp -- a host test of what the launcher removes beside
 * OpenTricky.exe: at start (ot_install_tidy_root) and before an update
 * (ot_install_plan), only the copies of the program that an install of 0.1.1
 * left there, each file exactly as the published 0.1.1 zip has it -- never a
 * player's file, never through a junction; and neither a Restore nor the
 * clean-up at start empties what a junction points to. A pass that a file or
 * a folder in use cuts short is finished at the next start, and a copy that
 * cannot be removed is never read.
 *
 *   cmake -S port/launcher/app -B <build dir> -G "MinGW Makefiles" -DOT_BUILD_TESTS=ON
 *   cmake --build <build dir> --target ot_tidy_test
 *   ot_tidy_test <OpenTricky-0.1.1-win64.zip> <work folder, new or empty>
 *
 * One line per check, then "PASS n/n" (exit 0) or "FAIL k of n" (exit 1);
 * 2 = bad arguments, or a step of the set-up that failed. The work folder is
 * left as it is, to be looked at.
 */
#include "ot_install.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

typedef std::map<std::string, std::string> Snap;

int g_checks = 0, g_failed = 0;

void check(bool ok, const std::string &what)
{
    g_checks++;
    if (!ok) g_failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    fflush(stdout);
}

[[noreturn]] void setup_failed(const std::string &what)
{
    printf("SET-UP FAILED: %s\n", what.c_str());
    exit(2);
}

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

DWORD attrs(const std::string &p) { return GetFileAttributesW(W(p).c_str()); }
bool exists(const std::string &p) { return attrs(p) != INVALID_FILE_ATTRIBUTES; }

/* A junction or a symbolic link. */
bool link_at(const std::string &p)
{
    DWORD a = attrs(p);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(p).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
}

std::vector<std::string> names(const std::string &dir)
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

void make_dirs(const std::string &p)
{
    for (size_t i = 3; i < p.size(); i++)
        if (p[i] == '\\') CreateDirectoryW(W(p.substr(0, i)).c_str(), nullptr);
    CreateDirectoryW(W(p).c_str(), nullptr);
}

void put(const std::string &p, const std::string &bytes)
{
    size_t s = p.find_last_of('\\');
    if (s != std::string::npos) make_dirs(p.substr(0, s));
    HANDLE h = CreateFileW(W(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD n = 0;
    bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, bytes.data(), (DWORD)bytes.size(), &n, nullptr) && n == bytes.size();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) setup_failed("cannot write " + p);
}

std::string get(const std::string &p)
{
    std::string out;
    HANDLE h = CreateFileW(W(p).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) setup_failed("cannot read " + p);
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(h);
    return out;
}

/* A file or a folder, copied (the sources hold no link). */
void copy_tree(const std::string &src, const std::string &dst)
{
    DWORD a = attrs(src);
    if (a == INVALID_FILE_ATTRIBUTES) setup_failed("missing " + src);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        size_t s = dst.find_last_of('\\');
        if (s != std::string::npos) make_dirs(dst.substr(0, s));
        if (!CopyFileW(W(src).c_str(), W(dst).c_str(), FALSE)) setup_failed("cannot copy " + src);
        return;
    }
    make_dirs(dst);
    for (const std::string &n : names(src)) copy_tree(src + "\\" + n, dst + "\\" + n);
}

void junction(const std::string &link, const std::string &target)
{
    std::wstring cmd = L"mklink /J \"" + W(link) + L"\" \"" + W(target) + L"\" >nul";
    if (_wsystem(cmd.c_str()) != 0 || !link_at(link)) setup_failed("cannot make the junction " + link);
}

/* icacls on `p` for everyone (S-1-1-0), e.g. "/deny *S-1-1-0:(DE)" or "/remove:d *S-1-1-0". */
void acl(const std::string &p, const char *args)
{
    std::wstring cmd = L"icacls \"" + W(p) + L"\" " + W(args) + L" >nul";
    if (_wsystem(cmd.c_str()) != 0) setup_failed("icacls " + p + " " + args);
}

/* Every entry below `dir`: "f <sha256>" a file, "d" a folder, "l" a junction
 * or a link (not gone through). */
void snap_into(const std::string &dir, const std::string &rel, Snap &out)
{
    for (const std::string &n : names(rel.empty() ? dir : dir + "\\" + rel)) {
        const std::string r = rel.empty() ? n : rel + "\\" + n, p = dir + "\\" + r;
        if (link_at(p)) out[r] = "l";
        else if (attrs(p) & FILE_ATTRIBUTE_DIRECTORY) {
            out[r] = "d";
            snap_into(dir, r, out);
        } else {
            out[r] = "f " + ot_sha256_file(p);
        }
    }
}

Snap snap(const std::string &dir)
{
    Snap s;
    snap_into(dir, "", s);
    return s;
}

/* `s` without `name` and what is below it. */
Snap without(Snap s, const std::string &name)
{
    for (auto it = s.begin(); it != s.end();) {
        if (it->first == name || it->first.compare(0, name.size() + 1, name + "\\") == 0) it = s.erase(it);
        else ++it;
    }
    return s;
}

bool has(const std::vector<std::string> &v, const std::string &x)
{
    for (const std::string &e : v)
        if (e == x) return true;
    return false;
}

bool starts(const std::vector<std::string> &v, const std::string &x)
{
    for (const std::string &e : v)
        if (e.compare(0, x.size(), x) == 0) return true;
    return false;
}

std::string join(const std::vector<std::string> &v)
{
    std::string s;
    for (const std::string &e : v) s += (s.empty() ? "" : " | ") + e;
    return "[" + s + "]";
}

struct Tidy {
    std::vector<std::string> removed, kept, failed;
};

Tidy tidy(const std::string &dir)
{
    Tidy t;
    ot_install_tidy_root(dir + "\\", t.removed, t.kept, t.failed);
    printf("     tidy %s: removed %s kept %s failed %s\n", dir.c_str(), join(t.removed).c_str(), join(t.kept).c_str(),
           join(t.failed).c_str());
    return t;
}

/* A folder where the game is in bin\ (0.1.2 and later), with the player's things. */
void program_folder(const std::string &d)
{
    put(d + "\\OpenTricky.exe", "launcher");
    put(d + "\\README.txt", "readme");
    put(d + "\\bin\\SSX Tricky.exe", "game");
    put(d + "\\bin\\ui\\home.rml", "<rml/>");
    put(d + "\\bin\\THIRD-PARTY-LICENSES.txt", "licences");
    put(d + "\\settings.ini", "[Meta]\r\nVersion=1\r\n");
    put(d + "\\portable.txt", "");
    put(d + "\\Saves\\UDATA\\save.bin", "save");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: ot_tidy_test <OpenTricky-0.1.1-win64.zip> <work folder, new or empty>\n");
        return 2;
    }
    std::string work = argv[2];
    for (char &c : work)
        if (c == '/') c = '\\';
    while (work.size() > 3 && work.back() == '\\') work.pop_back();
    if (exists(work) && !names(work).empty()) setup_failed(work + " is not empty");
    make_dirs(work);
    const std::string v011 = work + "\\v011";
    std::string err;
    if (!ot_unzip(argv[1], v011, err)) setup_failed("the 0.1.1 zip: " + err);
    if (!exists(v011 + "\\SSX Tricky.exe") || !exists(v011 + "\\ui\\home.rml") || !exists(v011 + "\\THIRD-PARTY-LICENSES.txt"))
        setup_failed(std::string(argv[1]) + " is not the 0.1.1 zip");
    const char *const copies[] = { "SSX Tricky.exe", "ui", "THIRD-PARTY-LICENSES.txt" };

    /* 1. 0.1.1 unzipped over by a newer version: its three copies go, nothing else. */
    const std::string a = work + "\\a-011";
    program_folder(a);
    for (const char *c : copies) copy_tree(v011 + "\\" + c, a + "\\" + c);
    Snap before = snap(a);
    auto t0 = std::chrono::steady_clock::now();
    Tidy t = tidy(a);
    long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    printf("     (%lld ms, the hashes of 0.1.1's files included)\n", ms);
    check(t.removed.size() == 3 && has(t.removed, "SSX Tricky.exe") && has(t.removed, "ui") && has(t.removed, "THIRD-PARTY-LICENSES.txt") &&
              t.kept.empty() && t.failed.empty(),
          "0.1.1's SSX Tricky.exe, ui\\ and THIRD-PARTY-LICENSES.txt are removed");
    Snap want = before;
    for (const char *c : copies) want = without(want, c);
    check(snap(a) == want, "nothing else changed: bin\\, settings.ini, portable.txt, Saves\\ as they were");
    t = tidy(a);
    check(t.removed.empty() && t.kept.empty() && t.failed.empty() && snap(a) == want, "a second start does nothing");

    /* 2. Files of those names that are not 0.1.1's: all kept, nothing touched. */
    const std::string b = work + "\\b-other";
    program_folder(b);
    put(b + "\\SSX Tricky.exe", "MZ another program");
    put(b + "\\ui\\index.html", "<html></html>");
    put(b + "\\ui\\img\\a.png", "png");
    put(b + "\\THIRD-PARTY-LICENSES.txt", "Another program -- licences\r\n");
    before = snap(b);
    t = tidy(b);
    check(t.removed.empty() && t.failed.empty() && t.kept.size() == 3, "another program's SSX Tricky.exe, ui\\ and licences are kept");
    check(snap(b) == before, "... untouched");

    const std::string b2 = work + "\\b2-changed";
    program_folder(b2);
    copy_tree(v011 + "\\ui", b2 + "\\ui");
    put(b2 + "\\ui\\launcher.rcss", get(b2 + "\\ui\\launcher.rcss") + "\r\n/* the player's theme */\r\n");
    std::string lic = get(v011 + "\\THIRD-PARTY-LICENSES.txt");
    lic[lic.size() / 2] ^= 1;                                   /* same size, one bit changed */
    put(b2 + "\\THIRD-PARTY-LICENSES.txt", lic);
    before = snap(b2);
    t = tidy(b2);
    check(t.removed.empty() && t.failed.empty() && t.kept.size() == 2 && starts(t.kept, "ui: not") &&
              starts(t.kept, "THIRD-PARTY-LICENSES.txt: not"),
          "0.1.1's ui\\ with a changed page, and a licence file one bit off, are kept");
    check(snap(b2) == before, "... untouched, every file of that ui\\ included");

    /* 3. The player's files in 0.1.1's ui\: only 0.1.1's go. */
    const std::string c = work + "\\c-merged";
    program_folder(c);
    copy_tree(v011 + "\\ui", c + "\\ui");
    put(c + "\\ui\\mine.txt", "mine");
    put(c + "\\ui\\fonts\\Other.ttf", "font");
    put(c + "\\ui\\extra\\notes.txt", "notes");
    make_dirs(c + "\\ui\\empty");
    Snap ours = snap(v011 + "\\ui");
    t = tidy(c);
    check(t.removed.empty() && t.failed.empty() && starts(t.kept, "ui: 4 entries in it are not"), "a ui\\ that also holds the player's files is kept");
    Snap after = snap(c + "\\ui");
    Snap want_c = { { "empty", "d" }, { "extra", "d" }, { "extra\\notes.txt", "f " + ot_sha256_file(c + "\\ui\\extra\\notes.txt") },
                    { "fonts", "d" }, { "fonts\\Other.ttf", "f " + ot_sha256_file(c + "\\ui\\fonts\\Other.ttf") },
                    { "mine.txt", "f " + ot_sha256_file(c + "\\ui\\mine.txt") } };
    check(after == want_c && ours.size() > 50,
          "... holding the player's 6 entries only, as they were: 0.1.1's files and the folders they left empty are gone");

    /* 4. Junctions: never gone through, what they point to untouched. */
    const std::string d1 = work + "\\d1-ui-junction", d1t = work + "\\d1-target";
    program_folder(d1);
    copy_tree(v011 + "\\ui", d1t);
    junction(d1 + "\\ui", d1t);
    before = snap(d1t);
    t = tidy(d1);
    check(t.removed.empty() && t.failed.empty() && starts(t.kept, "ui: not") && link_at(d1 + "\\ui") && snap(d1t) == before,
          "ui\\ a junction (to a copy of 0.1.1's ui\\): kept, what it points to untouched");

    const std::string d2 = work + "\\d2-inner-junction", d2t = work + "\\d2-target";
    program_folder(d2);
    copy_tree(v011 + "\\ui", d2 + "\\ui");
    if (!MoveFileW(W(d2 + "\\ui\\icons").c_str(), W(d2t).c_str())) setup_failed("cannot move ui\\icons");
    junction(d2 + "\\ui\\icons", d2t);
    before = snap(d2t);
    t = tidy(d2);
    check(t.removed.empty() && t.failed.empty() && starts(t.kept, "ui: 1 entry in it is not") && link_at(d2 + "\\ui\\icons") &&
              !exists(d2 + "\\ui\\home.rml") && snap(d2t) == before,
          "a junction inside 0.1.1's ui\\: its other files go, the junction stays, what it points to untouched");

    const std::string d3 = work + "\\d3-restore", d3t = work + "\\d3-target";
    put(d3 + "\\OpenTricky.exe", "launcher");
    put(d3 + "\\bin\\SSX Tricky.exe", "game");
    put(d3t + "\\home.rml", "<the player's/>");
    put(d3t + "\\fonts\\mine.ttf", "font");
    junction(d3 + "\\bin\\ui", d3t);
    put(d3 + "\\old\\manifest.txt", "OpenTricky backup 1\nversion 0.1.3\nhad bin\n");
    put(d3 + "\\old\\bin\\SSX Tricky.exe", "old game");
    put(d3 + "\\old\\bin\\ui\\home.rml", "<old/>");
    before = snap(d3t);
    {
        OtInstallRequest req;
        req.base = d3 + "\\";
        req.version_now = "0.1.4";
        OtInstallState st;
        ot_install_restore(req, st);
        printf("     restore: ok %d, fail \"%s\", %s\n", st.ok ? 1 : 0, st.fail.c_str(), st.detail.c_str());
        check(!st.ok && st.detail.find("cannot back up bin\\ui") != std::string::npos && link_at(d3 + "\\bin\\ui") &&
                  snap(d3t) == before,
              "Restore with bin\\ui a junction: refused, what it points to untouched");
    }

    const std::string d4 = work + "\\d4-cleanup", d4t = work + "\\d4-target";
    program_folder(d4);
    put(d4t + "\\keep.txt", "the player's");
    make_dirs(d4 + "\\update-tmp\\stage");
    junction(d4 + "\\update-tmp\\stage\\j", d4t);
    before = snap(d4t);
    ot_install_cleanup(d4 + "\\");
    check(!exists(d4 + "\\update-tmp") && snap(d4t) == before, "clean-up of update-tmp\\ holding a junction: the link goes, what it points to stays");

    /* bin\ a junction (the program moved to another drive): putting an
     * install back never removes it -- a failed Restore, and an update cut
     * half-way then undone at start. */
    const std::string j1 = work + "\\j1-restore-fails", j1t = work + "\\j1-target";
    put(j1 + "\\OpenTricky.exe", "launcher");
    put(j1t + "\\SSX Tricky.exe", "game");
    put(j1t + "\\ui\\home.rml", "<rml/>");
    junction(j1 + "\\bin", j1t);
    put(j1 + "\\old\\manifest.txt", "OpenTricky backup 1\nversion 0.1.3\nhad bin\n");
    put(j1 + "\\old\\bin\\SSX Tricky.exe", "old game");
    put(j1 + "\\old\\bin\\ui\\home.rml", "<old/>");
    before = snap(j1t);
    {
        OtInstallRequest req;
        req.base = j1 + "\\";
        req.version_now = "0.1.4";
        req.fail_at = "bin";
        OtInstallState st;
        ot_install_restore(req, st);
        printf("     restore: ok %d, fail \"%s\", %s\n", st.ok ? 1 : 0, st.fail.c_str(), st.detail.c_str());
        check(!st.ok && st.fail == "install" && link_at(j1 + "\\bin") && snap(j1t) == before,
              "a Restore that fails in bin\\, a junction: put back, bin\\ is the junction as before, what it points to as it was");
    }
    const std::string j2 = work + "\\j2-recover", j2t = work + "\\j2-target";
    put(j2 + "\\OpenTricky.exe", "launcher");
    put(j2t + "\\SSX Tricky.exe", "new game");
    put(j2t + "\\ui\\home.rml", "<rml/>");
    junction(j2 + "\\bin", j2t);
    put(j2 + "\\update-tmp\\backup\\manifest.txt", "OpenTricky backup 1\nversion 0.1.4\nhad bin\\SSX Tricky.exe\n");
    put(j2 + "\\update-tmp\\backup\\bin\\SSX Tricky.exe", "game");
    put(j2 + "\\update-tmp\\installing.txt", "OpenTricky install 1\nkind update\nbackup backup\nstate applying\ntries 0\n");
    {
        OtRecovery r = ot_install_recover(j2 + "\\");
        printf("     recover: found %d, ok %d, finished %d, %s\n", r.found ? 1 : 0, r.ok ? 1 : 0, r.finished ? 1 : 0, r.detail.c_str());
        check(r.found && r.ok && !r.finished && link_at(j2 + "\\bin") && get(j2t + "\\SSX Tricky.exe") == "game" &&
                  exists(j2t + "\\ui\\home.rml") && !exists(j2 + "\\update-tmp\\installing.txt"),
              "an update cut half-way, undone at start with bin\\ a junction: bin\\ is the junction as before, the game put back in it");
    }

    /* 5. A file in use: kept until the next start, then removed. */
    const std::string e = work + "\\e-in-use";
    program_folder(e);
    copy_tree(v011 + "\\THIRD-PARTY-LICENSES.txt", e + "\\THIRD-PARTY-LICENSES.txt");
    HANDLE h = CreateFileW(W(e + "\\THIRD-PARTY-LICENSES.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) setup_failed("cannot open the licence file");
    t = tidy(e);
    check(t.removed.empty() && t.kept.empty() && t.failed.size() == 1 && has(t.failed, "THIRD-PARTY-LICENSES.txt") &&
              exists(e + "\\THIRD-PARTY-LICENSES.txt"),
          "0.1.1's licence file in use: not removed (failed)");
    CloseHandle(h);
    t = tidy(e);
    check(t.removed.size() == 1 && has(t.removed, "THIRD-PARTY-LICENSES.txt") && t.failed.empty() && !exists(e + "\\THIRD-PARTY-LICENSES.txt"),
          "... removed at the next start");

    /* A file or a folder of 0.1.1's ui\ in use: the rest goes, but the two
     * pages stay, so that the next start knows that ui\ and finishes it. */
    const std::string e2 = work + "\\e2-ui-file-in-use";
    program_folder(e2);
    copy_tree(v011 + "\\ui", e2 + "\\ui");
    h = CreateFileW(W(e2 + "\\ui\\fonts\\Inter-Variable.ttf").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) setup_failed("cannot open ui\\fonts\\Inter-Variable.ttf");
    t = tidy(e2);
    check(t.removed.empty() && t.kept.empty() && t.failed.size() == 1 && has(t.failed, "ui") && exists(e2 + "\\ui\\home.rml") &&
              exists(e2 + "\\ui\\launcher.rcss") && exists(e2 + "\\ui\\fonts\\Inter-Variable.ttf") && !exists(e2 + "\\ui\\dialog.rml"),
          "a font of 0.1.1's ui\\ in use: the other files go; that font and the two pages stay (failed)");
    CloseHandle(h);
    t = tidy(e2);
    check(t.removed.size() == 1 && has(t.removed, "ui") && t.kept.empty() && t.failed.empty() && !exists(e2 + "\\ui"),
          "... the next start removes the rest of that ui\\");

    const std::string e3 = work + "\\e3-ui-folder-in-use";
    program_folder(e3);
    copy_tree(v011 + "\\ui", e3 + "\\ui");
    h = CreateFileW(W(e3 + "\\ui\\icons\\ps2").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);              /* as a console whose folder it is */
    if (h == INVALID_HANDLE_VALUE) setup_failed("cannot open ui\\icons\\ps2");
    t = tidy(e3);
    check(t.removed.empty() && t.kept.empty() && t.failed.size() == 1 && has(t.failed, "ui") && exists(e3 + "\\ui\\home.rml") &&
              exists(e3 + "\\ui\\launcher.rcss") && exists(e3 + "\\ui\\icons\\ps2") && names(e3 + "\\ui\\icons\\ps2").empty(),
          "a folder of 0.1.1's ui\\ in use: its files go, the folder and the two pages stay (failed)");
    CloseHandle(h);
    t = tidy(e3);
    check(t.removed.size() == 1 && has(t.removed, "ui") && t.kept.empty() && t.failed.empty() && !exists(e3 + "\\ui"),
          "... the next start removes the rest of that ui\\");

    const std::string e4 = work + "\\e4-ui-itself-in-use";
    program_folder(e4);
    copy_tree(v011 + "\\ui", e4 + "\\ui");
    h = CreateFileW(W(e4 + "\\ui").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) setup_failed("cannot open ui\\");
    t = tidy(e4);
    check(t.removed.empty() && t.kept.empty() && t.failed.size() == 1 && has(t.failed, "ui") && exists(e4 + "\\ui\\home.rml") &&
              exists(e4 + "\\ui\\launcher.rcss") && !exists(e4 + "\\ui\\dialog.rml") && !exists(e4 + "\\ui\\fonts"),
          "0.1.1's ui\\ itself in use: what is in it goes but the two pages (failed)");
    CloseHandle(h);
    t = tidy(e4);
    check(t.removed.size() == 1 && has(t.removed, "ui") && t.kept.empty() && t.failed.empty() && !exists(e4 + "\\ui"),
          "... the next start removes the pages and ui\\");

    /* 6. An update: the same rule (ot_install_plan, the entries it would put in place or remove). */
    const std::string stage = work + "\\stage";
    put(stage + "\\OpenTricky.exe", "new launcher");
    put(stage + "\\README.txt", "new readme");
    put(stage + "\\bin\\SSX Tricky.exe", "new game");
    put(stage + "\\bin\\ui\\home.rml", "<new/>");
    put(stage + "\\bin\\THIRD-PARTY-LICENSES.txt", "new licences");
    const std::string f1 = work + "\\f1-update-011";
    program_folder(f1);
    for (const char *k : copies) copy_tree(v011 + "\\" + k, f1 + "\\" + k);
    std::vector<std::string> plan = ot_install_plan(stage, f1 + "\\");
    printf("     plan %s: %s\n", f1.c_str(), join(plan).c_str());
    check(has(plan, "- SSX Tricky.exe") && has(plan, "- ui") && has(plan, "- THIRD-PARTY-LICENSES.txt") && plan.back() == "+ OpenTricky.exe",
          "update over 0.1.1's copies: they are removed (and backed up), the launcher last");
    int other_removals = 0;
    for (const std::string &s : plan)
        if (s[0] == '-' && s != "- SSX Tricky.exe" && s != "- ui" && s != "- THIRD-PARTY-LICENSES.txt") other_removals++;
    check(other_removals == 0, "... and nothing else is removed");
    struct { std::string dir, what; } kept_cases[] = {
        { b, "another program's files of those names" },
        { b2, "0.1.1's ui\\ with a changed page, a licence one bit off" },
        { c, "a ui\\ that held the player's files" },
        { d1, "ui\\ a junction" },
    };
    for (const auto &k : kept_cases) {
        plan = ot_install_plan(stage, k.dir + "\\");
        bool none = true;
        for (const char *x : copies)
            if (has(plan, std::string("- ") + x)) none = false;
        check(none, "update with " + k.what + ": not removed");
    }
    const std::string f2 = work + "\\f2-update-merged";
    program_folder(f2);
    copy_tree(v011 + "\\ui", f2 + "\\ui");
    put(f2 + "\\ui\\mine.txt", "mine");
    plan = ot_install_plan(stage, f2 + "\\");
    check(!has(plan, "- ui"), "update with 0.1.1's ui\\ plus a file of the player's: ui\\ is not removed");

    /* 7. A copy of 0.1.1's that cannot be removed (no right to: an install in
     * Program Files started without the rights, a protected folder): reported
     * as not removed, and never read -- reading it is denied too here, so a
     * read would have made it "not 0.1.1's". Once allowed, removed. */
    const std::string g = work + "\\g-protected";
    program_folder(g);
    copy_tree(v011 + "\\SSX Tricky.exe", g + "\\SSX Tricky.exe");
    acl(g + "\\SSX Tricky.exe", "/deny *S-1-1-0:(DE,RD)");
    acl(g, "/deny *S-1-1-0:(DC)");
    t0 = std::chrono::steady_clock::now();
    t = tidy(g);
    ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    printf("     (%lld ms)\n", ms);
    check(t.removed.empty() && t.kept.empty() && t.failed.size() == 1 && has(t.failed, "SSX Tricky.exe") && exists(g + "\\SSX Tricky.exe"),
          "0.1.1's SSX Tricky.exe that cannot be removed: not removed (failed), and not read");
    plan = ot_install_plan(stage, g + "\\");
    check(!has(plan, "- SSX Tricky.exe"), "... an update leaves it to the next start's tidy");
    acl(g, "/remove:d *S-1-1-0");
    acl(g + "\\SSX Tricky.exe", "/remove:d *S-1-1-0");
    t = tidy(g);
    check(t.removed.size() == 1 && has(t.removed, "SSX Tricky.exe") && t.failed.empty() && !exists(g + "\\SSX Tricky.exe"),
          "... removed once it can be");

    if (g_failed) {
        printf("FAIL %d of %d\n", g_failed, g_checks);
        return 1;
    }
    printf("PASS %d/%d\n", g_checks, g_checks);
    return 0;
}
