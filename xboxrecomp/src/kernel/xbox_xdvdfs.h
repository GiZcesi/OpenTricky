/*
 * xbox_xdvdfs.h - read-only XDVDFS (Xbox ISO / "xiso") reader
 *
 * Lets the game disc be served straight out of a .iso instead of a tree of
 * extracted files. Read-only by design: on real hardware the disc is
 * read-only too, and this port already routes writes elsewhere (TDATA/UDATA
 * live on the emulated hard disk, see kernel_path.c), so nothing needs to
 * write here.
 *
 * Format (validated against a real SSX Tricky USA image before this was
 * written): 2048-byte sectors; a volume descriptor at sector 32 beginning
 * and ending with "MICROSOFT*XBOX*MEDIA" and carrying the root directory's
 * sector and byte size; directory entries stored as a binary tree, each
 * entry being {u16 left, u16 right, u32 start_sector, u32 size, u8 attrs,
 * u8 name_len, char name[]} where left/right are offsets from the start of
 * the directory in 4-byte units. Some images place the filesystem at a
 * fixed base offset within the file, so the descriptor is probed at the
 * known offsets.
 */

#ifndef XBOX_XDVDFS_H
#define XBOX_XDVDFS_H

#include "platform/xbox_winnt.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XDVDFS_ATTR_DIRECTORY 0x10

/* One resolved directory entry. */
typedef struct {
    char     name[256];
    uint32_t sector;
    uint32_t size;
    uint8_t  attrs;
} xdvdfs_entry;

/**
 * Mount an Xbox ISO. Returns FALSE if the file can't be opened or has no
 * recognisable XDVDFS volume descriptor (in which case nothing is mounted
 * and the caller should fall back to a normal directory).
 */
BOOL xdvdfs_mount(const char *iso_path);

/** TRUE once a disc image is mounted. */
BOOL xdvdfs_is_mounted(void);

/** Release the mounted image. Safe to call when nothing is mounted. */
void xdvdfs_unmount(void);

/**
 * Look up a path *relative to the disc root* ("default.xbe",
 * "data\\config\\x.ini"; forward slashes are accepted too, and matching is
 * case-insensitive, as on the real filesystem). Any of the out params may
 * be NULL.
 */
BOOL xdvdfs_find(const char *rel_path, uint32_t *out_sector,
                 uint32_t *out_size, uint8_t *out_attrs);

/**
 * Read from a file's extent. `sector` is the entry's start sector and
 * `offset` is a byte offset within the file; reads are clamped to `size`.
 * Returns the number of bytes actually read.
 */
uint32_t xdvdfs_read(uint32_t sector, uint32_t size,
                     uint32_t offset, void *buf, uint32_t len);

/**
 * Enumerate a directory given its extent. Fills up to `max` entries and
 * returns how many were written. Order follows the on-disc tree (in-order,
 * i.e. sorted), which is what a title walking the directory expects.
 */
uint32_t xdvdfs_list(uint32_t sector, uint32_t size,
                     xdvdfs_entry *out, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_XDVDFS_H */
