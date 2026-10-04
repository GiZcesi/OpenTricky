/*
 * discart.c -- see discart.h.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discart.h"
#include "kernel/xbox_xdvdfs.h"

/* ── Reading files from the mounted disc ─────────────────────────── */

static uint8_t *disc_file(const char *path, uint32_t *len)
{
    uint32_t sector, size;
    uint8_t attrs, *buf;
    if (!xdvdfs_find(path, &sector, &size, &attrs) || (attrs & XDVDFS_ATTR_DIRECTORY) ||
        size == 0 || size > (64u << 20))
        return NULL;
    buf = (uint8_t *)malloc(size);
    if (!buf) return NULL;
    if (xdvdfs_read(sector, size, 0, buf, size) != size) { free(buf); return NULL; }
    *len = size;
    return buf;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | (p[1] << 8) | p[2]; }

/* ── RefPack (EA's LZ77) ─────────────────────────────────────────── */

static uint8_t *refpack(const uint8_t *src, uint32_t n, uint32_t *out_len)
{
    uint32_t p = 2, size, o = 0;
    uint8_t *out;
    if (n < 5 || src[1] != 0xFB) return NULL;
    if (src[0] & 0x01) p += (src[0] & 0x80) ? 4 : 3;          /* compressed size */
    if (src[0] & 0x80) { size = (be24(src + p) << 8) | src[p + 3]; p += 4; }
    else               { size = be24(src + p); p += 3; }
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

/* One entry of a c0fb archive whose name ends with `suffix`, unpacked. */
static uint8_t *big_entry(const uint8_t *d, uint32_t n, const char *suffix, uint32_t *out_len)
{
    uint32_t count, p = 6, i, sl = (uint32_t)strlen(suffix);
    if (n < 6 || d[0] != 0xC0 || d[1] != 0xFB) return NULL;
    count = (d[4] << 8) | d[5];
    for (i = 0; i < count; i++) {
        uint32_t off, size, nl;
        const char *name;
        if (p + 7 > n) return NULL;
        off = be24(d + p); size = be24(d + p + 3); p += 6;
        name = (const char *)d + p;
        nl = (uint32_t)strnlen(name, n - p);
        p += nl + 1;
        if (nl >= sl && _stricmp(name + nl - sl, suffix) == 0) {
            if (off + size > n) return NULL;
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

/* ── SHPX texture banks ──────────────────────────────────────────── */

void artimage_free(ArtImage *im)
{
    free(im->px);
    memset(im, 0, sizeof *im);
}

static BOOL image_alloc(ArtImage *im, int w, int h)
{
    im->px = (uint32_t *)calloc((size_t)w * h, 4);
    im->w = im->px ? w : 0;
    im->h = im->px ? h : 0;
    return im->px != NULL;
}

static uint32_t rgb565(uint16_t c, uint32_t a)
{
    uint32_t r = (c >> 11) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* Decode the entry named `name` (4 characters) of a bank, cropped to
 * [x0,x1) x [y0,y1) (x1 = 0: whole width, likewise y1). Formats seen on
 * the disc: 0x7D 32-bit BGRA, linear; 0x61 DXT3. */
static BOOL shpx_image(const uint8_t *d, uint32_t n, const char *name, int x0, int y0, int x1, int y1,
                       ArtImage *out)
{
    uint32_t count, i, off = 0;
    int w, h, fmt, x, y;
    const uint8_t *px;
    memset(out, 0, sizeof *out);
    if (n < 16 || memcmp(d, "SHPX", 4) != 0) return FALSE;
    count = le32(d + 8);
    for (i = 0; i < count && 0x18 + i * 8 <= n; i++)
        if (memcmp(d + 0x10 + i * 8, name, 4) == 0) { off = le32(d + 0x14 + i * 8); break; }
    if (!off || off + 16 > n) return FALSE;
    fmt = d[off];
    w = le16(d + off + 4);
    h = le16(d + off + 6);
    px = d + off + 16;
    if (x1 <= 0) x1 = w;
    if (y1 <= 0) y1 = h;
    if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0 || x1 > w || y1 > h || x0 >= x1 || y0 >= y1) return FALSE;
    if (!image_alloc(out, x1 - x0, y1 - y0)) return FALSE;

    if (fmt == 0x7D) {
        if (off + 16 + (uint32_t)w * h * 4 > n) { artimage_free(out); return FALSE; }
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++) {
                const uint8_t *s = px + ((size_t)y * w + x) * 4;      /* B G R A */
                out->px[(size_t)(y - y0) * out->w + (x - x0)] =
                    ((uint32_t)s[3] << 24) | (s[2] << 16) | (s[1] << 8) | s[0];
            }
        return TRUE;
    }
    if (fmt == 0x61) {
        int bx, by, k;
        if (off + 16 + (uint32_t)((w + 3) / 4) * ((h + 3) / 4) * 16 > n) { artimage_free(out); return FALSE; }
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 4) {
                const uint8_t *b = px + ((size_t)(by / 4) * ((w + 3) / 4) + bx / 4) * 16;
                uint16_t c0 = le16(b + 8), c1 = le16(b + 10);
                uint32_t bits = le32(b + 12), pal[4];
                pal[0] = rgb565(c0, 0); pal[1] = rgb565(c1, 0); pal[2] = pal[3] = 0;
                for (k = 0; k < 3; k++) {
                    uint32_t s0 = (pal[0] >> (k * 8)) & 255, s1 = (pal[1] >> (k * 8)) & 255;
                    pal[2] |= ((2 * s0 + s1) / 3) << (k * 8);
                    pal[3] |= ((s0 + 2 * s1) / 3) << (k * 8);
                }
                for (k = 0; k < 16; k++) {
                    x = bx + (k & 3); y = by + (k >> 2);
                    if (x < x0 || x >= x1 || y < y0 || y >= y1) continue;
                    {
                        uint32_t a = ((b[k / 2] >> ((k & 1) * 4)) & 15) * 17;
                        out->px[(size_t)(y - y0) * out->w + (x - x0)] = (pal[(bits >> (2 * k)) & 3] & 0xFFFFFF) | (a << 24);
                    }
                }
            }
        return TRUE;
    }
    artimage_free(out);
    return FALSE;
}

/* ── FNTF fonts ──────────────────────────────────────────────────── */

static BOOL ffn_font(const uint8_t *d, uint32_t n, ArtFont *f)
{
    uint32_t count, gt, at, i;
    memset(f, 0, sizeof *f);
    if (n < 0x20 || memcmp(d, "FNTF", 4) != 0) return FALSE;
    count = le16(d + 10);
    gt = le32(d + 0x14);
    at = le32(d + 0x1C);
    if (at + 16 > n || gt + count * 12 > n) return FALSE;
    f->w = le16(d + at + 4);
    f->h = le16(d + at + 6);
    if (f->w <= 0 || f->h <= 0 || at + 16 + (uint32_t)f->w * f->h / 2 > n) return FALSE;
    f->cov = (uint8_t *)malloc((size_t)f->w * f->h);
    if (!f->cov) return FALSE;
    for (i = 0; i < (uint32_t)f->w * f->h / 2; i++) {        /* 4-bit, high nibble first */
        uint8_t b = d[at + 16 + i];
        f->cov[2 * i]     = (uint8_t)((b >> 4) * 17);
        f->cov[2 * i + 1] = (uint8_t)((b & 15) * 17);
    }
    for (i = 0; i < count; i++) {
        const uint8_t *r = d + gt + i * 12;
        uint16_t code = le16(r);
        ArtGlyph *g;
        if (code >= 128) continue;
        g = &f->g[code];
        g->w = r[2]; g->h = r[3]; g->x = le16(r + 4); g->y = le16(r + 6);
        g->adv = r[8]; g->ox = (int8_t)r[9]; g->oy = (int8_t)r[10];
        if (g->x + g->w > f->w || g->y + g->h > f->h) memset(g, 0, sizeof *g);
        else if (g->h > f->line) f->line = g->h;
    }
    return TRUE;
}

/* The loading cards are stored square (256 x 256) but the game shows them
 * 3.2 times wider than tall: 520 x 160 px on its 1440 x 1080 loading screen
 * (measured). Bring them back to that shape once (256 x 80, each row
 * the average of the 3.2 texture rows it covers); every later scale is
 * uniform. */
static BOOL card_shape(ArtImage *im)
{
    ArtImage out;
    int x, y, w = im->w, h = im->w * 5 / 16;
    float k = (float)im->h / h;
    if (!image_alloc(&out, w, h)) return FALSE;
    for (y = 0; y < h; y++) {
        float y0 = y * k, y1 = y0 + k;
        for (x = 0; x < w; x++) {
            float acc[4] = { 0, 0, 0, 0 }, wsum = 0;
            int sy, c;
            for (sy = (int)y0; sy < (int)ceilf(y1) && sy < im->h; sy++) {
                float a = (sy + 1 < y1 ? sy + 1 : y1) - (sy > y0 ? sy : y0);
                uint32_t p = im->px[(size_t)sy * w + x];
                float al = (float)(p >> 24);
                if (a <= 0) continue;
                /* weight colour by alpha so the transparent corners do not darken the rim */
                acc[3] += a * al;
                for (c = 0; c < 3; c++) acc[c] += a * al * ((p >> (c * 8)) & 255);
                wsum += a;
            }
            {
                uint32_t A = wsum > 0 ? (uint32_t)(acc[3] / wsum + 0.5f) : 0, px = A << 24;
                for (c = 0; c < 3 && acc[3] > 0; c++) px |= (uint32_t)(acc[c] / acc[3] + 0.5f) << (c * 8);
                out.px[(size_t)y * w + x] = px;
            }
        }
    }
    artimage_free(im);
    *im = out;
    return TRUE;
}

/* ── Front-end sounds ──────────────────────────────────
 *
 * data/audio/audio.big is a "BIGF" archive (big-endian count, offset and
 * size per entry); data\audio\zbxfe.bnk inside is the front end's EA sound
 * bank: "BNKl" v5, a table of offsets to "PT" headers (tag, length,
 * big-endian value: 0x82 channels, 0x84 rate, 0x85 samples, 0x88 data
 * offset, 0x89 second channel's data offset, 0xA0 codec, 0x0A = EA-XA),
 * and EA-XA R2 data: per channel, 15-byte frames of 28 samples, or 0xEE + 4
 * history bytes + 28 raw big-endian samples. Which sound the game plays for
 * what was measured by matching recordings of the game's own menus:
 * a move in a list #18, a value changed #17, a choice made #2, back #34.
 * Only the bank's bytes are read from the 14 MB archive. */

static uint8_t *disc_part(const char *path, uint32_t off, uint32_t len)
{
    uint32_t sector, size;
    uint8_t attrs, *buf;
    if (!xdvdfs_find(path, &sector, &size, &attrs) || (attrs & XDVDFS_ATTR_DIRECTORY) ||
        off > size || len > size - off || len == 0)
        return NULL;
    buf = (uint8_t *)malloc(len);
    if (buf && xdvdfs_read(sector, size, off, buf, len) != len) { free(buf); buf = NULL; }
    return buf;
}

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

/* The bytes of one entry of a BIGF archive on the disc. */
static uint8_t *bigf_entry(const char *archive, const char *name, uint32_t *len)
{
    uint8_t head[16], *toc, *out = NULL;
    uint32_t count, toc_len, p = 0, i;
    uint8_t *h = disc_part(archive, 0, 16);
    if (!h) return NULL;
    memcpy(head, h, 16);
    free(h);
    if (memcmp(head, "BIGF", 4) != 0) return NULL;
    count = be32(head + 8);
    toc_len = be32(head + 12);
    if (count == 0 || count > 4096 || toc_len < 16 || toc_len > (1u << 20)) return NULL;
    toc = disc_part(archive, 0, toc_len);
    if (!toc) return NULL;
    p = 16;
    for (i = 0; i < count && p + 9 <= toc_len; i++) {
        uint32_t off = be32(toc + p), size = be32(toc + p + 4);
        const char *nm = (const char *)toc + p + 8;
        size_t nl = strnlen(nm, toc_len - p - 8);
        if (_stricmp(nm, name) == 0) {
            out = disc_part(archive, off, size);
            if (out) *len = size;
            break;
        }
        p += 8 + (uint32_t)nl + 1;
    }
    free(toc);
    return out;
}

/* One channel of EA-XA R2 into every `stride`-th sample of out. */
static BOOL eaxa_r2(const uint8_t *d, uint32_t n, uint32_t o, int16_t *out, int frames, int stride)
{
    static const int coef[8] = { 0, 240, 460, 392, 0, 0, -208, -220 };
    int h1 = 0, h2 = 0, done = 0, i;
    while (done < frames) {
        if (o >= n) return FALSE;
        if (d[o] == 0xEE) {
            if (o + 61 > n) return FALSE;
            for (i = 0; i < 28 && done < frames; i++, done++)
                out[done * stride] = (int16_t)((d[o + 5 + 2 * i] << 8) | d[o + 6 + 2 * i]);
            h1 = (int16_t)((d[o + 59] << 8) | d[o + 60]);
            h2 = (int16_t)((d[o + 57] << 8) | d[o + 58]);
            o += 61;
        } else {
            int c1 = coef[d[o] >> 4 & 3], c2 = coef[(d[o] >> 4 & 3) + 4], sh = (d[o] & 15) + 8;
            if (o + 15 > n) return FALSE;
            for (i = 0; i < 28 && done < frames; i++, done++) {
                int nib = (i & 1) ? (d[o + 1 + i / 2] & 15) : (d[o + 1 + i / 2] >> 4);
                int s = ((int32_t)((uint32_t)nib << 28) >> sh);
                s = (s + c1 * h1 + c2 * h2 + 128) >> 8;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                out[done * stride] = (int16_t)s;
                h2 = h1; h1 = s;
            }
            o += 15;
        }
    }
    return TRUE;
}

static BOOL bnk_sound(const uint8_t *d, uint32_t n, int index, ArtSound *out)
{
    uint32_t rel, o, tags[256] = { 0 };
    int ch;
    memset(out, 0, sizeof *out);
    if (n < 16 || memcmp(d, "BNKl", 4) != 0 || index >= le16(d + 6) || 0x10 + 4u * index + 4 > n) return FALSE;
    rel = le32(d + 0x10 + 4 * index);
    if (!rel) return FALSE;
    o = 0x10 + 4 * index + rel;
    if (o + 4 > n || d[o] != 'P' || d[o + 1] != 'T') return FALSE;
    o += 4;
    while (o < n && d[o] != 0xFF) {            /* the sound's tags */
        uint32_t t = d[o++], len, v = 0;
        if (t == 0xFD || t == 0xFE) continue;
        if (o >= n) return FALSE;
        len = d[o++];
        if (len > 4 || o + len > n) return FALSE;
        while (len--) v = (v << 8) | d[o++];
        tags[t] = v;
    }
    ch = tags[0x82] ? (int)tags[0x82] : 1;
    if (tags[0xA0] != 0x0A || ch < 1 || ch > 2 || !tags[0x85] || tags[0x85] > 30 * 48000 || !tags[0x84]) return FALSE;
    out->rate = (int)tags[0x84];
    out->channels = ch;
    out->frames = (int)tags[0x85];
    out->pcm = (int16_t *)malloc((size_t)out->frames * ch * 2);
    if (!out->pcm ||
        !eaxa_r2(d, n, tags[0x88], out->pcm, out->frames, ch) ||
        (ch == 2 && !eaxa_r2(d, n, tags[0x89], out->pcm + 1, out->frames, ch))) {
        free(out->pcm);
        memset(out, 0, sizeof *out);
        return FALSE;
    }
    return TRUE;
}

/* While the disc is mounted. FALSE (and nothing kept) if any sound is missing. */
static BOOL load_sounds(DiscArt *a)
{
    static const int index[ART_SND_COUNT] = { [ART_SND_MOVE] = 18, [ART_SND_CHANGE] = 17,
                                              [ART_SND_SELECT] = 2, [ART_SND_BACK] = 34 };
    uint32_t n = 0;
    uint8_t *bank = bigf_entry("data/audio/audio.big", "data\\audio\\zbxfe.bnk", &n);
    BOOL ok = bank != NULL;
    int i;
    for (i = 0; ok && i < ART_SND_COUNT; i++) ok = bnk_sound(bank, n, index[i], &a->sound[i]);
    free(bank);
    if (!ok) for (i = 0; i < ART_SND_COUNT; i++) { free(a->sound[i].pcm); memset(&a->sound[i], 0, sizeof a->sound[i]); }
    a->sounds_ok = ok;
    return ok;
}

/* ── Everything ──────────────────────────────────────────────────── */

void discart_free(DiscArt *a)
{
    int i;
    artimage_free(&a->logo);
    artimage_free(&a->backdrop);
    artimage_free(&a->selbar);
    for (i = 0; i < ART_RIDERS; i++) artimage_free(&a->rider[i]);
    for (i = 0; i < ART_TRACKS; i++) artimage_free(&a->track[i]);
    for (i = 0; i < ART_SND_COUNT; i++) free(a->sound[i].pcm);
    free(a->title.cov);
    memset(a, 0, sizeof *a);
}

BOOL discart_load(const char *iso, DiscArt *a)
{
    static const char *const riders[ART_RIDERS] = {
        "brodi", "eddie", "elise", "jp", "kaori", "luther", "mac", "marisol", "moby", "psymon", "seeiah", "zoe" };
    static const char *const tracks[ART_TRACKS] = {
        "alas", "aloh", "elys", "gari", "merq", "mesa", "pipe", "snow", "toky", "untr" };
    uint8_t *loading = NULL, *fe = NULL, *load = NULL, *font = NULL;
    uint32_t nl = 0, nf = 0, nb = 0, nt = 0;
    BOOL ok;
    int i;

    memset(a, 0, sizeof *a);
    if (!iso || !iso[0]) return FALSE;
    if (xdvdfs_is_mounted()) xdvdfs_unmount();
    if (!xdvdfs_mount(iso)) return FALSE;
    loading = disc_file("data/textures/loading.xsh", &nl);
    fe      = disc_file("data/textures/fe_1.xsh", &nf);
    load    = disc_file("data/textures/xboxload.big", &nb);
    font    = disc_file("data/fonts/title.ffn", &nt);
    load_sounds(a);                         /* optional, silent without */
    xdvdfs_unmount();

    ok = loading && fe && load && font &&
         shpx_image(loading, nl, "loa2", 2, 2, 254, 148, &a->logo) &&
         shpx_image(fe, nf, "dvd1", 0, 36, 256, 220, &a->backdrop) &&
         shpx_image(fe, nf, "fe_1", 0, 222, 256, 251, &a->selbar) &&
         ffn_font(font, nt, &a->title);
    for (i = 0; ok && i < ART_RIDERS + ART_TRACKS; i++) {
        char suffix[40];
        uint32_t n = 0;
        uint8_t *x;
        BOOL rider = i < ART_RIDERS;
        snprintf(suffix, sizeof suffix, rider ? "ldrider%s.xsh" : "ldtrack%s.xsh",
                 rider ? riders[i] : tracks[i - ART_RIDERS]);
        x = big_entry(load, nb, suffix, &n);
        ok = x && n >= 0x18 &&
             shpx_image(x, n, (const char *)x + 0x10, 0, 0, 0, 0,
                        rider ? &a->rider[i] : &a->track[i - ART_RIDERS]) &&
             card_shape(rider ? &a->rider[i] : &a->track[i - ART_RIDERS]);
        free(x);
    }
    free(loading); free(fe); free(load); free(font);
    if (!ok) { discart_free(a); return FALSE; }
    a->ok = TRUE;
    return TRUE;
}
