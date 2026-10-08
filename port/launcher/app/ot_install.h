/*
 * ot_install.h -- installing a new OpenTricky from its GitHub release, and
 * going back to the previous one. Only after the player's click.
 *
 * Update (ot_install_update, in a thread of its own):
 *   1. downloads SHA256SUMS.txt and the release's zip into update-tmp\
 *      beside the program (HTTPS);
 *   2. checks the zip's SHA-256 against SHA256SUMS.txt (and, later, an
 *      Ed25519 signature: ot_install_signature_ok) -- any doubt and nothing
 *      is changed;
 *   3. unpacks it into update-tmp\stage\ (each file's CRC-32 checked) and
 *      checks it holds SSX Tricky.exe, OpenTricky.exe and ui\;
 *   4. copies the program's current files into a backup, then puts each new
 *      top-level file or folder in place. The running OpenTricky.exe is
 *      renamed OpenTricky.exe.old first (removed at the next start). Any
 *      failure puts every replaced file back from the backup. The backup
 *      becomes old\ (the previous version, for Restore).
 * Never touched: settings.ini, the update cache, saves, disc images, the HD
 * pack, CrashReports, logs, old\ (besides being replaced by the new backup).
 *
 * Restore (ot_install_restore): the same step 4 from old\ (its manifest
 * says which files the update added, they are removed), then old\ goes.
 */
#ifndef OT_INSTALL_H
#define OT_INSTALL_H

#include <atomic>
#include <mutex>
#include <string>

struct OtInstallState {
    std::atomic<int> step{0};               /* 1 download, 2 check, 3 install, 4 done */
    std::atomic<long long> got{0}, total{0};
    std::atomic<bool> done{false}, cancel{false};
    std::mutex m;
    bool ok = false;
    std::string fail;                       /* "download", "verify", "install", "busy", "folder" */
    std::string detail;                     /* for the log */
};

struct OtInstallRequest {
    std::string base;                       /* the program's folder, with a trailing separator */
    std::string zip_url, zip_name, sums_url;
    long long zip_size = 0;
    std::string version_now, version_new;
    std::string user_agent;
    bool allow_loopback_http = false;       /* tests: a local fake GitHub */
    std::string fail_at;                    /* tests: fail when this top-level entry is installed */
};

void ot_install_update(const OtInstallRequest &req, OtInstallState &st);
void ot_install_restore(const OtInstallRequest &req, OtInstallState &st);

/* old\ holds a previous version: its version (from its manifest). */
bool ot_install_has_backup(const std::string &base, std::string &version);
/* The program's folder can be written (a file is made there and removed). */
bool ot_install_writable(const std::string &base);
/* SSX Tricky.exe of this folder is running (it cannot be replaced). */
bool ot_install_game_running(const std::string &base);
/* At start: removes OpenTricky.exe.old and update-tmp\ left by an update. */
void ot_install_cleanup(const std::string &base);

/* Exposed for the tests. */
std::string ot_sha256_file(const std::string &path);
bool ot_unzip(const std::string &zip, const std::string &dir, std::string &error);

#endif /* OT_INSTALL_H */
