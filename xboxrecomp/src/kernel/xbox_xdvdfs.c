/*
 * xbox_xdvdfs.c - read-only XDVDFS (Xbox ISO) reader. See xbox_xdvdfs.h.
 */

#include "xbox_xdvdfs.h"
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define XDVDFS_SECTOR       2048u
#define XDVDFS_DESC_SECTOR  32u
#define XDVDFS_MAGIC        "MICROSOFT*XBOX*MEDIA"
#define XDVDFS_MAGIC_LEN    20

/* Base offsets seen in the wild: a plain xiso starts at 0, while images
 * that keep the (unused here) video partition put the game partition at a
 * fixed offset. Probed in order; the first with a valid descriptor wins. */
static const uint64_t s_base_candidates[] = {
    0ull, 0x18300000ull, 0x0FD90000ull, 0x02080000ull, 0x1FB20000ull
};

static FILE    *s_iso        = NULL;
static uint64_t s_base       = 0;
static uint32_t s_root_sec   = 0;
static uint32_t s_root_size  = 0;
static CRITICAL_SECTION s_cs;   /* the title reads the disc from several threads */
static BOOL     s_cs_init    = FALSE;

/* ---------------------------------------------------------------- */

static BOOL raw_read(uint64_t offset, void *buf, uint32_t len)
{
    size_t got;
    if (!s_iso) return FALSE;
    if (_fseeki64(s_iso, (long long)(s_base + offset), SEEK_SET) != 0)
        return FALSE;
    got = fread(buf, 1, len, s_iso);
    return got == len;
}

/* XBOX_FIX_XDVDFS_CASE (default 1): walk directory trees with the uppercase
 * ordering the disc is mastered with. 0 = old lowercase walk, which misorders
 * '_' against letters and misses files such as data/textures/tbc_*.xsh. */
static int xdvdfs_fold_upper(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XBOX_FIX_XDVDFS_CASE");
        on = !(e && e[0] == '0');
        fprintf(stderr, "[XDVDFS] XBOX_FIX_XDVDFS_CASE=%d\n", on);
    }
    return on;
}

/* Case folding used for tree ordering; ASCII only, independent of locale. */
static int fold_char(int c, int up)
{
    if (up) return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
    return tolower(c);
}

/* Case-insensitive compare of a counted on-disc name against a NUL- or
 * separator-terminated path component. */
static int name_matches(const char *disc, uint8_t disc_len,
                        const char *comp, size_t comp_len)
{
    size_t i;
    if (disc_len != comp_len) return 0;
    for (i = 0; i < comp_len; i++) {
        if (tolower((unsigned char)disc[i]) != tolower((unsigned char)comp[i]))
            return 0;
    }
    return 1;
}

/*
 * Search one directory extent for a single path component.
 *
 * Entries form a binary tree whose left/right links are offsets from the
 * start of the directory in 4-byte units. Rather than recurse (a corrupt or
 * hostile image could otherwise blow the stack), walk the tree with an
 * explicit bound on the number of visited nodes.
 */
static BOOL dir_find_component(uint32_t sector, uint32_t size,
                               const char *comp, size_t comp_len,
                               uint32_t *out_sector, uint32_t *out_size,
                               uint8_t *out_attrs)
{
    uint8_t *dir;
    uint32_t off = 0;
    uint32_t visited = 0;
    uint32_t max_visits;
    BOOL     found = FALSE;

    if (size == 0 || size > 16u * 1024u * 1024u) return FALSE;
    dir = (uint8_t *)malloc(size);
    if (!dir) return FALSE;
    if (!raw_read((uint64_t)sector * XDVDFS_SECTOR, dir, size)) {
        free(dir);
        return FALSE;
    }

    max_visits = size / 4 + 1;      /* every node occupies >= 4 bytes */
    while (off * 4 + 14 <= size && visited++ < max_visits) {
        uint32_t p     = off * 4;
        uint16_t left  = (uint16_t)(dir[p]     | (dir[p + 1] << 8));
        uint16_t right = (uint16_t)(dir[p + 2] | (dir[p + 3] << 8));
        uint32_t start = (uint32_t)(dir[p + 4] | (dir[p + 5] << 8) |
                                    (dir[p + 6] << 16) | (dir[p + 7] << 24));
        uint32_t fsz   = (uint32_t)(dir[p + 8] | (dir[p + 9] << 8) |
                                    (dir[p + 10] << 16) | (dir[p + 11] << 24));
        uint8_t  attrs = dir[p + 12];
        uint8_t  nlen  = dir[p + 13];
        const char *nm = (const char *)(dir + p + 14);
        int cmp;

        if (p + 14 + nlen > size) break;   /* malformed entry */

        /* The tree is ordered by a case-insensitive byte comparison of the
         * names folded to uppercase ('_' sorts after the letters). */
        {
            size_t n = (nlen < comp_len) ? nlen : comp_len;
            size_t i;
            const int up = xdvdfs_fold_upper();
            cmp = 0;
            for (i = 0; i < n && cmp == 0; i++) {
                int a = fold_char((unsigned char)nm[i], up);
                int b = fold_char((unsigned char)comp[i], up);
                cmp = (a > b) - (a < b);
            }
            if (cmp == 0) cmp = (nlen > comp_len) - (nlen < comp_len);
        }

        if (cmp == 0 && name_matches(nm, nlen, comp, comp_len)) {
            if (out_sector) *out_sector = start;
            if (out_size)   *out_size   = fsz;
            if (out_attrs)  *out_attrs  = attrs;
            found = TRUE;
            break;
        }
        if (cmp > 0) {           /* on-disc name sorts after the target */
            if (!left) break;
            off = left;
        } else {
            if (!right) break;
            off = right;
        }
    }

    free(dir);
    return found;
}

/* Collect a whole directory in tree (sorted) order, iteratively. */
static uint32_t dir_collect(uint32_t sector, uint32_t size,
                            xdvdfs_entry *out, uint32_t max)
{
    uint8_t  *dir;
    uint32_t  count = 0;
    uint32_t *stack;
    uint32_t  sp = 0, cap;
    uint32_t  cur;
    BOOL      descending = TRUE;

    if (size == 0 || size > 16u * 1024u * 1024u || max == 0) return 0;
    dir = (uint8_t *)malloc(size);
    if (!dir) return 0;
    if (!raw_read((uint64_t)sector * XDVDFS_SECTOR, dir, size)) {
        free(dir);
        return 0;
    }

    cap = size / 4 + 2;
    stack = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!stack) { free(dir); return 0; }

    /* Standard iterative in-order traversal. */
    cur = 0;
    while ((descending || sp > 0) && count < max) {
        uint32_t p;
        uint16_t left, right;
        if (descending) {
            p = cur * 4;
            if (p + 14 > size) { descending = FALSE; continue; }
            left = (uint16_t)(dir[p] | (dir[p + 1] << 8));
            if (sp < cap) stack[sp++] = cur; else break;
            if (left) { cur = left; } else { descending = FALSE; }
            continue;
        }
        if (sp == 0) break;
        cur = stack[--sp];
        p = cur * 4;
        if (p + 14 > size) continue;
        right = (uint16_t)(dir[p + 2] | (dir[p + 3] << 8));
        {
            uint8_t nlen = dir[p + 13];
            if (p + 14 + nlen <= size) {
                xdvdfs_entry *e = &out[count];
                uint8_t n = (nlen < sizeof(e->name) - 1) ? nlen
                                                         : (uint8_t)(sizeof(e->name) - 1);
                memcpy(e->name, dir + p + 14, n);
                e->name[n] = '\0';
                e->sector = (uint32_t)(dir[p + 4] | (dir[p + 5] << 8) |
                                       (dir[p + 6] << 16) | (dir[p + 7] << 24));
                e->size   = (uint32_t)(dir[p + 8] | (dir[p + 9] << 8) |
                                       (dir[p + 10] << 16) | (dir[p + 11] << 24));
                e->attrs  = dir[p + 12];
                count++;
            }
        }
        if (right) { cur = right; descending = TRUE; }
    }

    free(stack);
    free(dir);
    return count;
}

/* ---------------------------------------------------------------- */

BOOL xdvdfs_mount(const char *iso_path)
{
    uint8_t desc[XDVDFS_SECTOR];
    size_t  i;

    if (!iso_path) return FALSE;
    xdvdfs_unmount();

    s_iso = fopen(iso_path, "rb");
    if (!s_iso) return FALSE;

    for (i = 0; i < sizeof(s_base_candidates) / sizeof(s_base_candidates[0]); i++) {
        s_base = s_base_candidates[i];
        if (!raw_read((uint64_t)XDVDFS_DESC_SECTOR * XDVDFS_SECTOR, desc, XDVDFS_SECTOR))
            continue;
        if (memcmp(desc, XDVDFS_MAGIC, XDVDFS_MAGIC_LEN) != 0)
            continue;
        if (memcmp(desc + XDVDFS_SECTOR - XDVDFS_MAGIC_LEN,
                   XDVDFS_MAGIC, XDVDFS_MAGIC_LEN) != 0)
            continue;   /* both copies must match -- guards a false positive */

        s_root_sec  = (uint32_t)(desc[20] | (desc[21] << 8) |
                                 (desc[22] << 16) | (desc[23] << 24));
        s_root_size = (uint32_t)(desc[24] | (desc[25] << 8) |
                                 (desc[26] << 16) | (desc[27] << 24));
        if (!s_cs_init) { InitializeCriticalSection(&s_cs); s_cs_init = TRUE; }
        fprintf(stderr, "  XDVDFS: mounted %s (base=0x%llX, root sector=%u, %u bytes)\n",
                iso_path, (unsigned long long)s_base, s_root_sec, s_root_size);
        fflush(stderr);
        return TRUE;
    }

    fclose(s_iso);
    s_iso = NULL;
    return FALSE;
}

BOOL xdvdfs_is_mounted(void) { return s_iso != NULL; }

void xdvdfs_unmount(void)
{
    if (s_iso) { fclose(s_iso); s_iso = NULL; }
    s_root_sec = s_root_size = 0;
    s_base = 0;
}

BOOL xdvdfs_find(const char *rel_path, uint32_t *out_sector,
                 uint32_t *out_size, uint8_t *out_attrs)
{
    uint32_t sec, size;
    uint8_t  attrs = XDVDFS_ATTR_DIRECTORY;
    const char *p = rel_path;
    BOOL ok = TRUE;

    if (!s_iso || !rel_path) return FALSE;

    sec  = s_root_sec;
    size = s_root_size;

    EnterCriticalSection(&s_cs);
    while (ok) {
        const char *start;
        size_t len;
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;                     /* trailing separator: done */
        start = p;
        while (*p && *p != '\\' && *p != '/') p++;
        len = (size_t)(p - start);
        /* Trailing blanks are tolerated by the real filesystem. */
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '.')) len--;
        if (len == 0) continue;

        if (!(attrs & XDVDFS_ATTR_DIRECTORY)) { ok = FALSE; break; }
        ok = dir_find_component(sec, size, start, len, &sec, &size, &attrs);
    }
    LeaveCriticalSection(&s_cs);

    if (!ok) return FALSE;
    if (out_sector) *out_sector = sec;
    if (out_size)   *out_size   = size;
    if (out_attrs)  *out_attrs  = attrs;
    return TRUE;
}

uint32_t xdvdfs_read(uint32_t sector, uint32_t size,
                     uint32_t offset, void *buf, uint32_t len)
{
    uint32_t avail;
    BOOL ok;

    if (!s_iso || !buf) return 0;
    if (offset >= size) return 0;
    avail = size - offset;
    if (len > avail) len = avail;
    if (len == 0) return 0;

    EnterCriticalSection(&s_cs);
    ok = raw_read((uint64_t)sector * XDVDFS_SECTOR + offset, buf, len);
    LeaveCriticalSection(&s_cs);
    return ok ? len : 0;
}

uint32_t xdvdfs_list(uint32_t sector, uint32_t size,
                     xdvdfs_entry *out, uint32_t max)
{
    uint32_t n;
    if (!s_iso || !out) return 0;
    EnterCriticalSection(&s_cs);
    n = dir_collect(sector, size, out, max);
    LeaveCriticalSection(&s_cs);
    return n;
}
