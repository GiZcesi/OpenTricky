/*
 * ot_disc.c -- see ot_disc.h.
 */
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif
#include "ot_disc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ── Small helpers ───────────────────────────────────────────────── */

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | (p[1] << 8) | p[2]; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static const char *const s_track_id[OT_TRACK_COUNT] = {
    "alas", "aloh", "elys", "gari", "merq", "mesa", "pipe", "snow", "toky", "untr" };
static const char *const s_track_name[OT_TRACK_COUNT] = {
    "ALASKA", "ALOHA ICE JAM", "ELYSIUM ALPS", "GARIBALDI", "MERQURY CITY",
    "MESABLANCA", "PIPEDREAM", "SNOWDREAM", "TOKYO MEGAPLEX", "UNTRACKED" };

const char *ot_track_id(int t) { return t >= 0 && t < OT_TRACK_COUNT ? s_track_id[t] : ""; }
const char *ot_track_name(int t) { return t >= 0 && t < OT_TRACK_COUNT ? s_track_name[t] : ""; }

/* ── The disc image (XDVDFS) ─────────────────────────────────────────
 * Sector 32 of the game partition holds "MICROSOFT*XBOX*MEDIA", the root
 * directory's sector and size, and the same magic at its end. A plain xiso
 * starts at 0; full images keep the video partition first and put the game
 * partition at one of a few fixed offsets. Directories are binary trees of
 * entries (left, right: offsets in 4-byte units; start sector, size,
 * attributes, name length, name); every node is visited, so the tree's
 * ordering does not matter. */

#define SECTOR 2048u
#define ATTR_DIR 0x10

typedef struct {
    FILE    *f;
    uint64_t base;
    uint32_t root_sec, root_size;
} Iso;

static FILE *open_utf8(const char *path)
{
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) return NULL;
    return _wfopen(w, L"rb");
#else
    return fopen(path, "rb");
#endif
}

static bool iso_read(Iso *iso, uint64_t off, void *buf, uint32_t len)
{
#ifdef _WIN32
    if (_fseeki64(iso->f, (long long)(iso->base + off), SEEK_SET) != 0) return false;
#else
    if (fseeko(iso->f, (off_t)(iso->base + off), SEEK_SET) != 0) return false;
#endif
    return fread(buf, 1, len, iso->f) == len;
}

static bool iso_open(Iso *iso, const char *path)
{
    static const uint64_t bases[] = { 0ull, 0x18300000ull, 0x0FD90000ull, 0x02080000ull, 0x1FB20000ull };
    uint8_t d[SECTOR];
    size_t i;
    memset(iso, 0, sizeof *iso);
    if (!path || !path[0] || !(iso->f = open_utf8(path))) return false;
    for (i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        iso->base = bases[i];
        if (!iso_read(iso, 32ull * SECTOR, d, SECTOR)) continue;
        if (memcmp(d, "MICROSOFT*XBOX*MEDIA", 20) || memcmp(d + SECTOR - 20, "MICROSOFT*XBOX*MEDIA", 20)) continue;
        iso->root_sec = le32(d + 20);
        iso->root_size = le32(d + 24);
        if (iso->root_size) return true;
    }
    fclose(iso->f);
    iso->f = NULL;
    return false;
}

static void iso_close(Iso *iso)
{
    if (iso->f) fclose(iso->f);
    iso->f = NULL;
}

/* One name in one directory. */
static bool dir_find(Iso *iso, uint32_t sec, uint32_t size, const char *name, size_t nlen,
                     uint32_t *out_sec, uint32_t *out_size, uint8_t *out_attr)
{
    uint8_t *dir;
    uint32_t *stack, sp = 0, visits = 0, cap = size / 14 + 2;     /* at most one push per node */
    bool found = false;
    if (size == 0 || size > (16u << 20)) return false;
    dir = (uint8_t *)malloc(size);
    stack = (uint32_t *)malloc(sizeof(uint32_t) * cap);
    if (!dir || !stack || !iso_read(iso, (uint64_t)sec * SECTOR, dir, size)) { free(dir); free(stack); return false; }
    stack[sp++] = 0;
    while (sp && visits++ < size / 4 + 1) {
        uint32_t p = stack[--sp] * 4;
        uint16_t left, right;
        uint8_t nl;
        if (p + 14 > size) continue;
        left = le16(dir + p);
        right = le16(dir + p + 2);
        if (left == 0xFFFF) continue;                      /* sector padding */
        nl = dir[p + 13];
        if (p + 14 + nl > size) continue;
        if (nl == nlen) {
            size_t i;
            for (i = 0; i < nlen; i++)
                if (tolower((unsigned char)dir[p + 14 + i]) != tolower((unsigned char)name[i])) break;
            if (i == nlen) {
                *out_sec = le32(dir + p + 4);
                *out_size = le32(dir + p + 8);
                *out_attr = dir[p + 12];
                found = true;
                break;
            }
        }
        if (left && sp < cap) stack[sp++] = left;
        if (right && sp < cap) stack[sp++] = right;
    }
    free(dir);
    free(stack);
    return found;
}

/* A file's place on the disc, by path ('/' or '\\'). */
static bool iso_find(Iso *iso, const char *path, uint32_t *sec, uint32_t *size)
{
    uint32_t s = iso->root_sec, z = iso->root_size;
    uint8_t attr = ATTR_DIR;
    while (*path) {
        const char *e = path;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e > path) {
            if (!(attr & ATTR_DIR) || !dir_find(iso, s, z, path, (size_t)(e - path), &s, &z, &attr)) return false;
        }
        path = *e ? e + 1 : e;
    }
    if (attr & ATTR_DIR) return false;
    *sec = s;
    *size = z;
    return true;
}

/* `len` bytes from `off` of a file (len 0: to the end; capped at 64 MB). */
static uint8_t *iso_file(Iso *iso, const char *path, uint32_t off, uint32_t len, uint32_t *out_len)
{
    uint32_t sec, size;
    uint8_t *buf;
    if (!iso_find(iso, path, &sec, &size) || off > size) return NULL;
    if (len == 0 || len > size - off) len = size - off;
    if (len == 0 || len > (64u << 20)) return NULL;
    buf = (uint8_t *)malloc(len);
    if (!buf) return NULL;
    if (!iso_read(iso, (uint64_t)sec * SECTOR + off, buf, len)) { free(buf); return NULL; }
    *out_len = len;
    return buf;
}

/* ── Disc check ──────────────────────────────────────────────────── */



OtDiscCheck ot_disc_check(const char *path)
{
    return ot_disc_check_entry(path, 0);
}

OtDiscCheck ot_disc_check_entry(const char *path, uint32_t expected_entry)
{
    OtDiscCheck r;
    Iso iso;
    FILE *f;
    uint8_t *x;
    uint32_t n = 0;
    memset(&r, 0, sizeof r);
    if (!path || !path[0]) { r.status = OT_DISC_NO_PATH; return r; }
    if (!(f = open_utf8(path))) { r.status = OT_DISC_NOT_FOUND; return r; }
    fclose(f);
    r.status = OT_DISC_WRONG;
    if (!iso_open(&iso, path)) return r;
    r.xbox = true;
    x = iso_file(&iso, "default.xbe", 0, 0x1000, &n);
    iso_close(&iso);
    if (x && n >= 0x200 && !memcmp(x, "XBEH", 4)) {
        uint32_t base = le32(x + 0x104), cert = le32(x + 0x118) - base, entry = le32(x + 0x128);
        if (cert + 0xA4 <= n) {
            int i;
            r.title_id = le32(x + cert + 8);
            for (i = 0; i < 40 && i < (int)sizeof r.title - 1; i++) {
                unsigned c = le16(x + cert + 0x0C + i * 2);
                if (!c) break;
                r.title[i] = (c >= 32 && c < 127) ? (char)c : '?';
            }
            r.region = le32(x + cert + 0xA0);
        }
        if (expected_entry) {
            /* Retail images XOR the entry point with 0xA8FC57AB, debug ones with 0x94859D4B. */
            if ((entry ^ 0xA8FC57ABu) == expected_entry || (entry ^ 0x94859D4Bu) == expected_entry)
                r.status = OT_DISC_OK;
        } else if (r.title_id == OT_SSX_TRICKY_USA_TITLE_ID && (r.region & 1)) {
            r.status = OT_DISC_OK;
        }
    }
    free(x);
    return r;
}

/* ── RefPack (EA's LZ77) ─────────────────────────────────────────── */

static uint8_t *refpack(const uint8_t *src, uint32_t n, uint32_t *out_len)
{
    uint32_t p = 2, size, o = 0;
    uint8_t *out;
    if (n < 5 || src[1] != 0xFB) return NULL;
    if (src[0] & 0x01) p += (src[0] & 0x80) ? 4 : 3;          /* compressed size */
    if (src[0] & 0x80) { if (n < p + 4) return NULL; size = (be24(src + p) << 8) | src[p + 3]; p += 4; }
    else               { if (n < p + 3) return NULL; size = be24(src + p); p += 3; }
    if (size == 0 || size > (64u << 20)) return NULL;
    out = (uint8_t *)malloc(size);
    if (!out) return NULL;
    while (p < n) {
        uint32_t c = src[p], lit, len = 0, off = 0, i;
        if (c < 0x80) {
            if (p + 2 > n) break;
            lit = c & 3; len = ((c >> 2) & 7) + 3; off = ((c & 0x60) << 3) + src[p + 1] + 1; p += 2;
        } else if (c < 0xC0) {
            if (p + 3 > n) break;
            lit = src[p + 1] >> 6; len = (c & 0x3F) + 4;
            off = ((src[p + 1] & 0x3F) << 8) + src[p + 2] + 1; p += 3;
        } else if (c < 0xE0) {
            if (p + 4 > n) break;
            lit = c & 3; len = ((c & 0x0C) << 6) + src[p + 3] + 5;
            off = ((c & 0x10) << 12) + (src[p + 1] << 8) + src[p + 2] + 1; p += 4;
        } else if (c < 0xFC) {
            lit = ((c & 0x1F) << 2) + 4; p++;
            if (p + lit > n || o + lit > size) break;
            memcpy(out + o, src + p, lit); o += lit; p += lit;
            continue;
        } else {
            lit = c & 3; p++;
            if (p + lit > n || o + lit > size) break;
            memcpy(out + o, src + p, lit); o += lit;
            *out_len = o;
            if (o == size) return out;
            break;
        }
        if (p + lit > n || o + lit + len > size || off > o + lit) break;
        memcpy(out + o, src + p, lit); o += lit; p += lit;
        for (i = 0; i < len; i++, o++) out[o] = out[o - off];
    }
    free(out);
    return NULL;
}

/* One entry of a c0fb archive (24-bit big-endian offset and size per entry)
 * whose name ends with `suffix`, unpacked. */
static uint8_t *c0fb_entry(const uint8_t *d, uint32_t n, const char *suffix, uint32_t *out_len)
{
    uint32_t count, p = 6, i, sl = (uint32_t)strlen(suffix);
    if (n < 6 || d[0] != 0xC0 || d[1] != 0xFB) return NULL;
    count = (d[4] << 8) | d[5];
    for (i = 0; i < count; i++) {
        uint32_t off, size, nl = 0;
        const char *name;
        if (p + 7 > n) return NULL;
        off = be24(d + p); size = be24(d + p + 3); p += 6;
        name = (const char *)d + p;
        while (p + nl < n && name[nl]) nl++;
        p += nl + 1;
        if (nl >= sl && ieq(name + nl - sl, suffix)) {
            if (off > n || size > n - off) return NULL;
            if (size > 2 && d[off + 1] == 0xFB) return refpack(d + off, size, out_len);
            {
                uint8_t *copy = (uint8_t *)malloc(size);
                if (copy) { memcpy(copy, d + off, size); *out_len = size; }
                return copy;
            }
        }
    }
    return NULL;
}

/* The bytes of one entry of a BIGF archive on the disc (big-endian count,
 * then offset, size and name per entry), at most `cap` from its start
 * (0 = all). Only the table and the entry are read. */
static uint8_t *bigf_entry(Iso *iso, const char *archive, const char *name, uint32_t cap, uint32_t *len)
{
    uint8_t *head, *toc, *out = NULL;
    uint32_t count, toc_len, p, i, got = 0;
    head = iso_file(iso, archive, 0, 16, &got);
    if (!head) return NULL;
    if (got < 16 || memcmp(head, "BIGF", 4) != 0) { free(head); return NULL; }
    count = be32(head + 8);
    toc_len = be32(head + 12);
    free(head);
    if (count == 0 || count > 4096 || toc_len < 16 || toc_len > (1u << 20)) return NULL;
    toc = iso_file(iso, archive, 0, toc_len, &got);
    if (!toc || got < toc_len) { free(toc); return NULL; }
    for (p = 16, i = 0; i < count && p + 9 <= toc_len; i++) {
        uint32_t off = be32(toc + p), size = be32(toc + p + 4), nl = 0;
        const char *nm = (const char *)toc + p + 8;
        while (p + 8 + nl < toc_len && nm[nl]) nl++;
        if (p + 8 + nl >= toc_len) break;
        if (ieq(nm, name)) {
            if (cap && size > cap) size = cap;
            out = size ? iso_file(iso, archive, off, size, len) : NULL;
            if (out && *len != size) { free(out); out = NULL; }
            break;
        }
        p += 8 + nl + 1;
    }
    free(toc);
    return out;
}

/* ── SHPX texture banks ──────────────────────────────────────────────
 * The first entry, or the one named `name` (4 characters). Formats seen on
 * the disc: 0x7D 32-bit BGRA, linear; 0x61 DXT3. */
static bool shpx_image(const uint8_t *d, uint32_t n, const char *name, OtImage *out)
{
    uint32_t count, i, off = 0;
    int w, h, fmt, x, y;
    const uint8_t *px;
    memset(out, 0, sizeof *out);
    if (n < 0x18 || memcmp(d, "SHPX", 4) != 0) return false;
    count = le32(d + 8);
    for (i = 0; i < count && 0x18 + i * 8 <= n; i++)
        if (!name || memcmp(d + 0x10 + i * 8, name, 4) == 0) { off = le32(d + 0x14 + i * 8); break; }
    if (!off || off + 16 > n) return false;
    fmt = d[off];
    w = le16(d + off + 4);
    h = le16(d + off + 6);
    px = d + off + 16;
    if (w <= 0 || h <= 0 || !ot_image_alloc(out, w, h)) return false;
    if (fmt == 0x7D) {
        if (off + 16 + (uint64_t)w * h * 4 > n) { ot_image_free(out); return false; }
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                const uint8_t *s = px + ((size_t)y * w + x) * 4;      /* B G R A */
                out->px[(size_t)y * w + x] = ((uint32_t)s[3] << 24) | (s[2] << 16) | (s[1] << 8) | s[0];
            }
        return true;
    }
    if (fmt == 0x61) {
        int bx, by, k;
        if (off + 16 + (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * 16 > n) { ot_image_free(out); return false; }
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 4) {
                uint32_t blk[16];
                ot_decode_bc2(px + ((size_t)(by / 4) * ((w + 3) / 4) + bx / 4) * 16, blk);
                for (k = 0; k < 16; k++) {
                    x = bx + (k & 3); y = by + (k >> 2);
                    if (x < w && y < h) out->px[(size_t)y * w + x] = blk[k];
                }
            }
        return true;
    }
    ot_image_free(out);
    return false;
}

/* The loading cards are stored square (256 x 256) but the game shows them
 * 3.2 times wider than tall: 520 x 160 px on its 1440 x 1080 loading screen
 * (measured on the game's own screen). Bring them back to that shape (256 x
 * 80, each row the alpha-weighted average of the 3.2 texture rows it
 * covers); every later scale is uniform. */
static bool card_shape(const OtImage *im, OtImage *out)
{
    int x, y, w = im->w, h = im->w * 5 / 16;
    float k = (float)im->h / h;
    if (!ot_image_alloc(out, w, h)) return false;
    for (y = 0; y < h; y++) {
        float y0 = y * k, y1 = y0 + k;
        for (x = 0; x < w; x++) {
            float acc[4] = { 0, 0, 0, 0 }, wsum = 0;
            int sy, c;
            for (sy = (int)y0; sy < im->h && (float)sy < y1; sy++) {
                float a = ((float)(sy + 1) < y1 ? (float)(sy + 1) : y1) - ((float)sy > y0 ? (float)sy : y0);
                uint32_t p = im->px[(size_t)sy * w + x];
                float al = (float)(p >> 24);
                if (a <= 0) continue;
                acc[3] += a * al;
                for (c = 0; c < 3; c++) acc[c] += a * al * (float)((p >> (c * 8)) & 255);
                wsum += a;
            }
            {
                uint32_t A = wsum > 0 ? (uint32_t)(acc[3] / wsum + 0.5f) : 0, px = A << 24;
                for (c = 0; c < 3 && acc[3] > 0; c++) px |= (uint32_t)(acc[c] / acc[3] + 0.5f) << (c * 8);
                out->px[(size_t)y * w + x] = px;
            }
        }
    }
    return true;
}

/* The round emblem at the left of every track card: the same square on the
 * ten cards, x 22..82.5 and y 9..69.5 of the 256 x 80 card (y 28.8..222.4 of
 * the stored texture), drawn 240 x 240 inside a circle (inset 6 px). */
static bool card_emblem(const OtImage *tex, OtImage *out)
{
    uint8_t *m;
    float k = (float)tex->h / (tex->w * 5.0f / 16.0f), s = tex->w / 256.0f;
    if (!ot_image_resample(tex, 22.0f * s, 9.0f * s * k, 82.5f * s, 69.5f * s * k, 240, 240, out)) return false;
    m = ot_mask_ellipse(240, 240, 6, 6, 234, 234);
    if (!m) { ot_image_free(out); return false; }
    ot_image_apply_mask(out, m);
    free(m);
    return true;
}

bool ot_card_emblem(const OtImage *card, OtImage *out)
{
    if (!card || !card->px || card->w < 64) return false;
    return card_emblem(card, out);
}

/* ── FNTF fonts ──────────────────────────────────────────────────── */

static bool ffn_font(const uint8_t *d, uint32_t n, OtFont *f)
{
    uint32_t count, gt, at, i;
    memset(f, 0, sizeof *f);
    if (n < 0x20 || memcmp(d, "FNTF", 4) != 0) return false;
    count = le16(d + 10);
    gt = le32(d + 0x14);
    at = le32(d + 0x1C);
    if (at + 16 > n || gt + count * 12 > n) return false;
    f->w = le16(d + at + 4);
    f->h = le16(d + at + 6);
    if (f->w <= 0 || f->h <= 0 || at + 16 + (uint32_t)f->w * f->h / 2 > n) return false;
    f->cov = (uint8_t *)malloc((size_t)f->w * f->h);
    if (!f->cov) return false;
    for (i = 0; i < (uint32_t)f->w * f->h / 2; i++) {        /* 4-bit, high nibble first */
        uint8_t b = d[at + 16 + i];
        f->cov[2 * i]     = (uint8_t)((b >> 4) * 17);
        f->cov[2 * i + 1] = (uint8_t)((b & 15) * 17);
    }
    for (i = 0; i < count; i++) {
        const uint8_t *r = d + gt + i * 12;
        uint16_t code = le16(r);
        OtGlyph *g;
        if (code >= 128) continue;
        g = &f->g[code];
        g->w = r[2]; g->h = r[3]; g->x = le16(r + 4); g->y = le16(r + 6);
        g->adv = r[8]; g->ox = (int8_t)r[9]; g->oy = (int8_t)r[10];
        if (g->x + g->w > f->w || g->y + g->h > f->h) memset(g, 0, sizeof *g);
        else if (g->h > f->line) f->line = g->h;
    }
    return true;
}

int ot_font_text_width(const OtFont *f, const char *s)
{
    int w = 0;
    for (; *s; s++)
        if ((unsigned char)*s < 128) w += f->g[(unsigned char)*s].adv;
    return w;
}

/* ── Front-end sounds ────────────────────────────────────────────────
 * data\audio\zbxfe.bnk (inside audio.big) is the front end's EA sound bank:
 * "BNKl" v5, a table of offsets to "PT" headers (tag, length, big-endian
 * value: 0x82 channels, 0x84 rate, 0x85 samples, 0x88 data offset, 0x89
 * second channel's data offset, 0xA0 codec, 0x0A = EA-XA), and EA-XA R2
 * data: per channel, 15-byte frames of 28 samples, or 0xEE + 4 history
 * bytes + 28 raw big-endian samples. Which sound the game plays for what was
 * matched against recordings of the game's own menus: a move in a list #18,
 * a value changed #17, a choice made #2, back #34. */

static const int s_eaxa_coef[8] = { 0, 240, 460, 392, 0, 0, -208, -220 };

static int16_t eaxa_sample(int nib, int sh, int c1, int c2, int h1, int h2)
{
    int s = ((int32_t)((uint32_t)nib << 28) >> sh);
    s = (s + c1 * h1 + c2 * h2 + 128) >> 8;
    return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

static bool eaxa_r2(const uint8_t *d, uint32_t n, uint32_t o, int16_t *out, int frames, int stride)
{
    int h1 = 0, h2 = 0, done = 0, i;
    while (done < frames) {
        if (o >= n) return false;
        if (d[o] == 0xEE) {
            if (o + 61 > n) return false;
            for (i = 0; i < 28 && done < frames; i++, done++)
                out[done * stride] = (int16_t)((d[o + 5 + 2 * i] << 8) | d[o + 6 + 2 * i]);
            h1 = (int16_t)((d[o + 59] << 8) | d[o + 60]);
            h2 = (int16_t)((d[o + 57] << 8) | d[o + 58]);
            o += 61;
        } else {
            int c1 = s_eaxa_coef[d[o] >> 4 & 3], c2 = s_eaxa_coef[(d[o] >> 4 & 3) + 4], sh = (d[o] & 15) + 8;
            if (o + 15 > n) return false;
            for (i = 0; i < 28 && done < frames; i++, done++) {
                int nib = (i & 1) ? (d[o + 1 + i / 2] & 15) : (d[o + 1 + i / 2] >> 4);
                int16_t s = eaxa_sample(nib, sh, c1, c2, h1, h2);
                out[done * stride] = s;
                h2 = h1; h1 = s;
            }
            o += 15;
        }
    }
    return true;
}

static bool bnk_sound(const uint8_t *d, uint32_t n, int index, OtSound *out)
{
    uint32_t rel, o, tags[256];
    int ch;
    memset(out, 0, sizeof *out);
    memset(tags, 0, sizeof tags);
    if (n < 16 || memcmp(d, "BNKl", 4) != 0 || index >= le16(d + 6) || 0x10 + 4u * index + 4 > n) return false;
    rel = le32(d + 0x10 + 4 * index);
    if (!rel) return false;
    o = 0x10 + 4 * index + rel;
    if (o + 4 > n || d[o] != 'P' || d[o + 1] != 'T') return false;
    o += 4;
    while (o < n && d[o] != 0xFF) {
        uint32_t t = d[o++], len, v = 0;
        if (t == 0xFD || t == 0xFE) continue;
        if (o >= n) return false;
        len = d[o++];
        if (len > 4 || o + len > n) return false;
        while (len--) v = (v << 8) | d[o++];
        tags[t] = v;
    }
    ch = tags[0x82] ? (int)tags[0x82] : 1;
    if (tags[0xA0] != 0x0A || ch < 1 || ch > 2 || !tags[0x85] || tags[0x85] > 30 * 48000 || !tags[0x84]) return false;
    out->rate = (int)tags[0x84];
    out->channels = ch;
    out->frames = (int)tags[0x85];
    out->pcm = (int16_t *)malloc((size_t)out->frames * ch * 2);
    if (!out->pcm ||
        !eaxa_r2(d, n, tags[0x88], out->pcm, out->frames, ch) ||
        (ch == 2 && !eaxa_r2(d, n, tags[0x89], out->pcm + 1, out->frames, ch))) {
        free(out->pcm);
        memset(out, 0, sizeof *out);
        return false;
    }
    return true;
}

static bool load_sounds(Iso *iso, OtDiscAssets *a)
{
    static const int index[OT_SND_COUNT] = { 18, 17, 2, 34 };   /* move, change, select, back */
    uint32_t n = 0;
    uint8_t *bank = bigf_entry(iso, "data/audio/audio.big", "data\\audio\\zbxfe.bnk", 0, &n);
    bool ok = bank != NULL;
    int i;
    for (i = 0; ok && i < OT_SND_COUNT; i++) ok = bnk_sound(bank, n, index[i], &a->sound[i]);
    free(bank);
    if (!ok)
        for (i = 0; i < OT_SND_COUNT; i++) { free(a->sound[i].pcm); memset(&a->sound[i], 0, sizeof a->sound[i]); }
    return a->sounds_ok = ok;
}

/* ── The title screen's music ─────────────────────────────────────────
 * data\audio\ssxmenu.mus (inside music.big) is the front end's interactive
 * music: a run of EA "SCHl" streams (one bar each, 1.69 s, stereo 44.1 kHz,
 * EA-XA), 128-byte aligned. Each stream: "SCHl" (PT header), "SCCl" (block
 * count), "SCDl" blocks (u32 samples, u32 offset per channel, then per
 * channel two little-endian history samples and EA-XA R1 frames of 15 bytes
 * / 28 samples), "SCEl". On the title screen the game plays bars 0 to 7 over
 * and over (matched in a recording of the game); the bars join without a
 * click, bar 7 back into bar 0 as well. Only the first 640 KB are read. */

#define MUSIC_BARS 8

static bool eaxa_r1(const uint8_t *d, uint32_t n, uint32_t o, int16_t *out, int frames, int stride)
{
    int h1, h2, done = 0, i;
    if (o + 4 > n) return false;
    h2 = (int16_t)le16(d + o);
    h1 = (int16_t)le16(d + o + 2);
    o += 4;
    while (done < frames) {
        int c1, c2, sh;
        if (o + 15 > n) return false;
        c1 = s_eaxa_coef[d[o] >> 4 & 3]; c2 = s_eaxa_coef[(d[o] >> 4 & 3) + 4]; sh = (d[o] & 15) + 8;
        for (i = 0; i < 28 && done < frames; i++, done++) {
            int nib = (i & 1) ? (d[o + 1 + i / 2] & 15) : (d[o + 1 + i / 2] >> 4);
            int16_t s = eaxa_sample(nib, sh, c1, c2, h1, h2);
            out[done * stride] = s;
            h2 = h1; h1 = s;
        }
        o += 15;
    }
    return true;
}

static bool load_music(Iso *iso, OtDiscAssets *a)
{
    uint32_t n = 0, p = 0, have = 0, cap_frames = MUSIC_BARS * 80000;
    uint8_t *d = bigf_entry(iso, "data/audio/music.big", "data\\audio\\ssxmenu.mus", 0xA0000, &n);
    int16_t *pcm;
    int bars = 0;
    bool ok = d != NULL;
    OtSound *m = &a->music;
    memset(m, 0, sizeof *m);
    pcm = ok ? (int16_t *)malloc((size_t)cap_frames * 2 * 2) : NULL;
    ok = ok && pcm;
    while (ok && bars < MUSIC_BARS) {
        uint32_t size;
        while (p + 8 <= n && memcmp(d + p, "SCHl", 4) != 0) p += 4;
        if (p + 8 > n) { ok = false; break; }
        size = le32(d + p + 4);
        if (size < 8 || size > n - p) { ok = false; break; }
        p += size;
        while (ok && p + 8 <= n) {
            uint32_t sz = le32(d + p + 4);
            if (sz < 8 || sz > n - p) { ok = false; break; }
            if (memcmp(d + p, "SCDl", 4) == 0) {
                uint32_t frames = le32(d + p + 8), base = p + 20;
                if (sz < 20 || have + frames > cap_frames ||
                    !eaxa_r1(d, p + sz, base + le32(d + p + 12), pcm + have * 2, (int)frames, 2) ||
                    !eaxa_r1(d, p + sz, base + le32(d + p + 16), pcm + have * 2 + 1, (int)frames, 2))
                    ok = false;
                else
                    have += frames;
            } else if (memcmp(d + p, "SCEl", 4) == 0) {
                p += sz;
                break;
            } else if (memcmp(d + p, "SCCl", 4) != 0) {
                ok = false;
            }
            p += sz;
        }
        bars++;
    }
    free(d);
    if (!ok || have < 44100) { free(pcm); return a->music_ok = false; }
    m->rate = 44100;
    m->channels = 2;
    m->frames = (int)have;
    m->pcm = pcm;
    return a->music_ok = true;
}

/* ── Everything ──────────────────────────────────────────────────── */

void ot_disc_free(OtDiscAssets *a)
{
    int i;
    for (i = 0; i < OT_TRACK_COUNT; i++) {
        ot_image_free(&a->card_tex[i]);
        ot_image_free(&a->card[i]);
        ot_image_free(&a->emblem[i]);
    }
    for (i = 0; i < OT_SND_COUNT; i++) free(a->sound[i].pcm);
    free(a->music.pcm);
    free(a->title.cov);
    memset(a, 0, sizeof *a);
}

bool ot_disc_load(const char *path, OtDiscAssets *a)
{
    Iso iso;
    uint8_t *load = NULL, *font = NULL;
    uint32_t nb = 0, nt = 0;
    bool ok;
    int i;

    memset(a, 0, sizeof *a);
    if (!iso_open(&iso, path)) return false;
    load = iso_file(&iso, "data/textures/xboxload.big", 0, 0, &nb);
    font = iso_file(&iso, "data/fonts/title.ffn", 0, 0, &nt);
    load_sounds(&iso, a);
    load_music(&iso, a);
    iso_close(&iso);

    ok = load && font && ffn_font(font, nt, &a->title);
    for (i = 0; ok && i < OT_TRACK_COUNT; i++) {
        char suffix[32];
        uint32_t n = 0;
        uint8_t *x;
        snprintf(suffix, sizeof suffix, "ldtrack%s.xsh", s_track_id[i]);
        x = c0fb_entry(load, nb, suffix, &n);
        ok = x && shpx_image(x, n, NULL, &a->card_tex[i]) &&
             card_shape(&a->card_tex[i], &a->card[i]) &&
             card_emblem(&a->card_tex[i], &a->emblem[i]);
        free(x);
    }
    free(load);
    free(font);
    if (!ok) { ot_disc_free(a); return false; }
    a->ok = true;
    return true;
}
