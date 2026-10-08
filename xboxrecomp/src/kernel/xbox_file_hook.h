/*
 * xbox_file_hook.h - host-side rewrite of a game file as it is opened
 *
 * Lets the host serve a variant of a data file without touching the disc or
 * the extracted files: when the title opens a path the hook claims, the
 * kernel reads the original file in full, hands it to the hook, and gives the
 * title a read-only in-memory handle on what the hook returned. A hook that
 * declines (returns NULL) leaves the original file served as usual.
 *
 * Win32 backend only. With no hook installed nothing changes.
 */
#ifndef XBOX_FILE_HOOK_H
#define XBOX_FILE_HOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TRUE when the hook wants to see this file (`xbox_path` is the title's own
 * ANSI path, e.g. "D:\data\config\btnmap0.dat"). Called on every open:
 * keep it cheap. */
typedef int (*xbox_file_hook_match_fn)(const char *xbox_path);

/* Return a malloc'd replacement and its size, or NULL to serve the original.
 * The kernel frees the buffer when the title closes the handle. */
typedef void *(*xbox_file_hook_rewrite_fn)(const char *xbox_path,
                                           const void *data, uint32_t size,
                                           uint32_t *out_size);

/* Up to four hooks; the first whose match claims a path serves it. */
void xbox_file_add_hook(xbox_file_hook_match_fn match,
                        xbox_file_hook_rewrite_fn rewrite);
/* The same as xbox_file_add_hook (kept for the callers of one hook). */
void xbox_file_set_hook(xbox_file_hook_match_fn match,
                        xbox_file_hook_rewrite_fn rewrite);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_FILE_HOOK_H */
