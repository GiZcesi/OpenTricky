/*
 * ot_install.h -- installing a new OpenTricky from its GitHub release, and
 * going back to the previous one. Only after the player's click.
 *
 * Update (ot_install_update, in a thread of its own):
 *   1. downloads SHA256SUMS.txt, checks its Ed25519 signature
 *      (SHA256SUMS.txt.sig, ot_sign.h; once the launcher holds the key, a
 *      release without it is refused), then downloads the release's zip
 *      into update-tmp\ beside the program (HTTPS);
 *   2. checks the zip's SHA-256 against SHA256SUMS.txt -- any doubt and
 *      nothing is changed;
 *   3. unpacks it into update-tmp\stage\ (each file's CRC-32 checked) and
 *      checks it holds OpenTricky.exe and the game: SSX Tricky.exe and ui\,
 *      in bin\ (0.1.2 and later) or beside OpenTricky.exe (older);
 *   4. copies the program's current files into a backup, then puts each new
 *      top-level file or folder in place -- bin\, once there, one entry at a
 *      time (ot_install.cpp, stage_ops). The running OpenTricky.exe is
 *      renamed OpenTricky.exe.old first (removed at the next start). Any
 *      failure puts every replaced file back from the backup. The backup
 *      becomes old\ (the previous version, for Restore). A journal
 *      (update-tmp\installing.txt) covers this step: an install cut half-way
 *      is put back, or finished, at the next start (ot_install_recover).
 * Never touched: settings.ini, the update cache, the theme order, saves, disc images, the HD
 * pack, CrashReports, logs, old\ (besides being replaced by the new backup);
 * in bin\ too.
 *
 * Restore (ot_install_restore): the same step 4 from old\ (its manifest
 * says which files the update added, they are removed), then old\ goes, and
 * bin\ when nothing is left in it.
 *
 * The folders: the launcher's (`base`) is the install root, where settings.ini
 * and the player's files are; the game and the interface files are in its
 * bin\ subfolder (ot_program_dir), or beside it in an older or a developer's
 * install.
 */
#ifndef OT_INSTALL_H
#define OT_INSTALL_H

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

struct OtInstallState {
    std::atomic<int> step{0};               /* 1 download, 2 check, 3 install, 4 done */
    std::atomic<long long> got{0}, total{0};
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};        /* the player's Cancel: an update stops before step 3 (fail "cancel") */
    std::mutex m;
    bool ok = false;
    std::string fail;                       /* "download", "signature", "verify", "install", "busy", "folder",
                                               "partial" (stopped, not all put back yet: the next start does it) */
    std::string detail;                     /* for the log */
};

struct OtInstallRequest {
    std::string base;                       /* the program's folder, with a trailing separator */
    std::string zip_url, zip_name, sums_url;
    std::string sig_url;                    /* SHA256SUMS.txt.sig (ot_sign.h), "" when absent */
    long long zip_size = 0;
    std::string version_now, version_new;
    std::string user_agent;
    bool allow_loopback_http = false;       /* tests: a local fake GitHub */
    std::string fail_at;                    /* tests: fail when this top-level entry is installed */
    std::string kill_at;                    /* tests: the process ends there, as if killed: an entry (once the
                                               old one is gone), "@applied" (all in place) or "@old" (old\ gone) */
};

void ot_install_update(const OtInstallRequest &req, OtInstallState &st);
void ot_install_restore(const OtInstallRequest &req, OtInstallState &st);

/* old\ holds a previous version: its version (from its manifest). */
bool ot_install_has_backup(const std::string &base, std::string &version);
/* The program's folder can be written (a file is made there and removed). */
bool ot_install_writable(const std::string &base);
/* SSX Tricky.exe of this folder or of its bin\ is running (it cannot be replaced). */
bool ot_install_game_running(const std::string &base);
/* An update or a Restore cut half-way (the launcher killed, a power cut):
 * its journal (update-tmp\installing.txt, written before the first entry
 * changes) is still there. */
struct OtRecovery {
    bool found = false;         /* a journal was there */
    bool ok = false;            /* put right: back as before, or finished */
    bool finished = false;      /* every entry was already in place: finished rather than undone */
    bool gave_up = false;       /* not put right after a few starts: the journal is dropped */
    std::string kind;           /* "update" or "restore" */
    std::string version;        /* undone: the version that is back */
    std::string detail;         /* for the log */
};
/* At start, before anything else reads the program's files, and before
 * ot_install_cleanup: undoes a cut install from its backup, or finishes it
 * when every entry was in place. When it cannot (a file in use), the journal
 * and the backup stay for the next start, a few times at most. */
OtRecovery ot_install_recover(const std::string &base);
/* At start: removes OpenTricky.exe.old and update-tmp\ left by an update
 * (update-tmp\ stays while a cut install is not put right). */
void ot_install_cleanup(const std::string &base);
/* Where the game and the interface files are: base + "bin\" when it holds
 * ui\ (the game too, normally), else base (everything in one folder). */
std::string ot_program_dir(const std::string &base);
/* At start, once the game is in bin\: removes the copies of the program left
 * beside OpenTricky.exe (SSX Tricky.exe, ui\, THIRD-PARTY-LICENSES.txt) by the
 * update from 0.1.1 or by unzipping a new version over an old one. Nothing
 * else: never old\ nor a player's file. A file in use stays (`failed`) and is
 * tried again at the next start. */
void ot_install_tidy_root(const std::string &base, std::vector<std::string> &removed, std::vector<std::string> &failed);

/* Exposed for the tests. */
std::string ot_sha256_file(const std::string &path);
bool ot_unzip(const std::string &zip, const std::string &dir, std::string &error);

#endif /* OT_INSTALL_H */
