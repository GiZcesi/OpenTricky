/*
 * ot_update.h -- "is there a newer OpenTricky?": the launcher reads the list
 * of releases of the project on GitHub (HTTPS, the system's WinHTTP), at
 * most once a day, in a thread of its own, and never installs anything.
 *
 *  - ot_update_check() runs in a worker thread: it reads the cache file
 *    (beside settings.ini), asks api.github.com only when the cache is older
 *    than a day (If-None-Match with the cached ETag), and gives back the
 *    releases. Nothing about the player is sent: one GET, a User-Agent with
 *    the launcher's version, no cookie.
 *  - ot_update_pick() chooses the release a channel follows:
 *      Stable   = the newest release that is not a pre-release; when there is
 *                 none, the newest pre-release;
 *      Unstable = the newest release, pre-releases included.
 *    Drafts are never taken. Versions compare as major.minor.patch, the
 *    channel suffix ("-unstable", "-dev") ignored.
 *  - ot_update_notes() gives a release's "What's new": its <!-- launcher -->
 *    block (ot_util.h), else the first three bullets of its "Highlights".
 */
#ifndef OT_UPDATE_H
#define OT_UPDATE_H

#include "ot_util.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct OtAsset {
    std::string name, url;      /* url: browser_download_url */
    long long size = 0;
};
struct OtRelease {
    std::string tag, name, url, body;
    bool prerelease = false, draft = false;
    std::vector<OtAsset> assets;
};

/* One GET (HTTPS; plain HTTP only to 127.0.0.1 / localhost when allowed, for
 * tests). To a file when `file` is set (progress called as it comes), else
 * into `body`. Blocking: run it in a thread. */
struct OtHttpRequest {
    std::string url, etag, user_agent, headers, file;
    int timeout_ms = 3000;
    bool allow_loopback_http = false;
    size_t max_body = 8u << 20;
    std::function<void(long long got, long long total)> progress;
    const std::atomic<bool> *cancel = nullptr;
};
struct OtHttp {
    int status = 0;             /* 0: no answer */
    std::string body, etag, error;
    long long length = 0, received = 0;
};
OtHttp ot_http_get(const OtHttpRequest &q);

/* major.minor.patch of "v0.1.2-unstable" -> 0, 1, 2. false if it is not a version. */
bool ot_version_parse(const std::string &s, int out[3]);
/* <0, 0, >0 like strcmp; a string that is not a version is older than any. */
int ot_version_cmp(const std::string &a, const std::string &b);

/* The GitHub API's JSON list of releases. false if it is not one. */
bool ot_releases_parse(const std::string &json, std::vector<OtRelease> &out);
/* Index of the release the channel follows, or -1. */
int ot_update_pick(const std::vector<OtRelease> &rel, bool unstable);
OtNotes ot_update_notes(const OtRelease &r);

struct OtUpdateRequest {
    std::string cache_file;     /* "" = no cache (tests) */
    std::string url;            /* https only */
    std::string json_file;      /* tests: read this file instead of the network */
    std::string user_agent;
    int timeout_ms = 3000;
    bool allow_loopback_http = false;   /* tests: a local fake GitHub */
};
struct OtUpdateResult {
    bool ok = false;            /* a list of releases was read */
    std::string source;         /* "network", "not modified", "cache", "file", or why it failed */
    std::vector<OtRelease> releases;
    std::string skip;           /* the version the player chose to skip (cache file) */
    double ms = 0;
};
/* Blocking: run it in a thread. */
OtUpdateResult ot_update_check(const OtUpdateRequest &req);
/* The skipped version, kept in the cache file (the rest of it unchanged). */
std::string ot_update_read_skip(const std::string &cache_file);
bool ot_update_write_skip(const std::string &cache_file, const std::string &version);

#endif /* OT_UPDATE_H */
