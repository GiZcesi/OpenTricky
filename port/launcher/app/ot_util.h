/*
 * ot_util.h -- the launcher's helpers that have nothing to do with drawing:
 *  - the "What's new" block of a release's notes (<!-- launcher ... -->);
 *  - a small zip writer (stored, no compression) for bug reports;
 *  - crash reports left by the game (CrashReports\);
 *  - removing the player's folders and user name from text put in a report.
 * Portable C++17 on SDL3 (file and time calls); nothing here is sent anywhere.
 */
#ifndef OT_UTIL_H
#define OT_UTIL_H

#include <SDL3/SDL.h>
#include <string>
#include <vector>

/* ── What's new ─────────────────────────────────────────────────────
 * Each GitHub release's notes start with a block hidden in an HTML comment
 * (invisible on the web page, read by the launcher):
 *
 *   <!-- launcher
 *   version: 0.1.1
 *   title: No more freezes
 *   card: snowdream            (optional: the track whose card heads the panel)
 *   fixes: The game no longer freezes after a video
 *   new: PlayStation-style button icons
 *   better: Smoother on big and ultrawide screens
 *   -->
 *
 * One line per item; sections show in the order FIXES, NEW, BETTER. The
 * installed version's block is built into the launcher (whatsnew.txt). */
struct OtNoteSection {
    std::string name;                   /* "FIXES", "NEW", "BETTER" */
    std::vector<std::string> items;
};
struct OtNotes {
    bool ok = false;
    std::string version, title, card;
    std::vector<OtNoteSection> sections;
};
OtNotes ot_notes_parse(const std::string &text);

/* ── Zip (stored) ─────────────────────────────────────────────────── */
struct OtZipEntry {
    std::string name;                   /* path inside the zip, '/' separated */
    std::string data;
};
bool ot_zip_write(const std::string &path, const std::vector<OtZipEntry> &entries);

/* ── Files ──────────────────────────────────────────────────────────── */
bool ot_read_file(const std::string &path, std::string &out, size_t max_bytes = 64u << 20);
/* The last `max_bytes` of a file (a long log). */
bool ot_read_tail(const std::string &path, std::string &out, size_t max_bytes);
std::string ot_dir_of(const std::string &path);          /* with the trailing separator */
std::string ot_file_name(const std::string &path);

/* Folders and the user name replaced: first each of `folders` (path, shown
 * name), then the profile folder (shown as %USERPROFILE%) and the user name
 * alone (shown as <user>). */
std::string ot_scrub(const std::string &text,
                     const std::vector<std::pair<std::string, std::string>> &folders = {});

/* ── Crash reports (the game's CrashReports folders) ─────────────────── */
struct OtCrashReport {
    bool found = false;
    std::string dir;                    /* with the trailing separator */
    std::string name;                   /* 2026-10-08_02-15-33_crash */
    std::string kind;                   /* "crash", "freeze" or "" */
    SDL_Time time = 0;
};
/* The newest report folder written at or after `since` (0 = any), under
 * <game folder>CrashReports\ and %LOCALAPPDATA%\SSX Tricky\CrashReports\. */
OtCrashReport ot_find_crash_report(const std::string &game_dir, SDL_Time since);

/* "today 21:42", "yesterday 08:10", "2026-10-06 21:42". */
std::string ot_when(SDL_Time t);

#endif /* OT_UTIL_H */
