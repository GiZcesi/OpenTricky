/*
 * ot_image.c -- see ot_image.h.
 */
#include "ot_image.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Basics ──────────────────────────────────────────────────────── */

bool ot_image_alloc(OtImage *im, int w, int h)
{
    im->w = im->h = 0;
    im->px = NULL;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    im->px = (uint32_t *)calloc((size_t)w * h, 4);
    if (!im->px) return false;
    im->w = w;
    im->h = h;
    return true;
}

void ot_image_free(OtImage *im)
{
    if (!im) return;
    free(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}

bool ot_image_copy(const OtImage *src, OtImage *dst)
{
    if (!src->px || !ot_image_alloc(dst, src->w, src->h)) return false;
    memcpy(dst->px, src->px, (size_t)src->w * src->h * 4);
    return true;
}

/* Premultiplied float RGBA <-> packed straight alpha. */
static float *to_premul(const OtImage *im)
{
    size_t i, n = (size_t)im->w * im->h;
    float *f = (float *)malloc(n * 4 * sizeof(float));
    if (!f) return NULL;
    for (i = 0; i < n; i++) {
        uint32_t p = im->px[i];
        float a = (float)(p >> 24) / 255.0f;
        f[i * 4 + 0] = (float)((p >> 16) & 255) * a;
        f[i * 4 + 1] = (float)((p >> 8) & 255) * a;
        f[i * 4 + 2] = (float)(p & 255) * a;
        f[i * 4 + 3] = a;
    }
    return f;
}

static uint32_t pack_premul(const float *v)
{
    float a = v[3], r = 0, g = 0, b = 0;
    if (a > 1.0f) a = 1.0f;
    if (a > 0.0f) {
        r = v[0] / a; g = v[1] / a; b = v[2] / a;
    } else {
        a = 0.0f;
    }
#define CL(x) ((x) < 0.0f ? 0u : (x) > 255.0f ? 255u : (uint32_t)((x) + 0.5f))
    return (CL(a * 255.0f) << 24) | (CL(r) << 16) | (CL(g) << 8) | CL(b);
#undef CL
}

/* ── Separable filtering ─────────────────────────────────────────── */

typedef struct {
    int    n;          /* taps */
    int   *idx;        /* source index of each tap, per output sample */
    float *w;
} Taps;

static float lanczos3(float x)
{
    if (x < 0) x = -x;
    if (x < 1e-6f) return 1.0f;
    if (x >= 3.0f) return 0.0f;
    {
        double px = M_PI * x;
        return (float)(3.0 * sin(px) * sin(px / 3.0) / (px * px));
    }
}

/* Taps for `dn` outputs covering source [s0, s1) of a line of `sn` samples. */
static bool taps_resample(Taps *t, int dn, float s0, float s1, int sn)
{
    float scale = (s1 - s0) / (float)dn, fs = scale > 1.0f ? scale : 1.0f, support = 3.0f * fs;
    int d, k;
    t->n = (int)ceilf(support) * 2 + 2;
    t->idx = (int *)malloc(sizeof(int) * (size_t)t->n * dn);
    t->w = (float *)malloc(sizeof(float) * (size_t)t->n * dn);
    if (!t->idx || !t->w) return false;
    for (d = 0; d < dn; d++) {
        float center = s0 + ((float)d + 0.5f) * scale, sum = 0;
        int lo = (int)floorf(center - support);
        int *ix = t->idx + (size_t)d * t->n;
        float *w = t->w + (size_t)d * t->n;
        for (k = 0; k < t->n; k++) {
            int i = lo + k;
            float x = ((float)i + 0.5f - center) / fs;
            w[k] = lanczos3(x);
            ix[k] = i < 0 ? 0 : i >= sn ? sn - 1 : i;
            sum += w[k];
        }
        if (sum != 0.0f)
            for (k = 0; k < t->n; k++) w[k] /= sum;
    }
    return true;
}

static bool taps_gauss(Taps *t, int n, float sigma)
{
    int r = (int)ceilf(sigma * 3.0f), d, k;
    t->n = 2 * r + 1;
    t->idx = (int *)malloc(sizeof(int) * (size_t)t->n * n);
    t->w = (float *)malloc(sizeof(float) * (size_t)t->n * n);
    if (!t->idx || !t->w) return false;
    for (d = 0; d < n; d++) {
        float sum = 0;
        for (k = 0; k < t->n; k++) {
            int i = d - r + k;
            float x = (float)(k - r);
            t->w[(size_t)d * t->n + k] = expf(-x * x / (2.0f * sigma * sigma));
            t->idx[(size_t)d * t->n + k] = i < 0 ? 0 : i >= n ? n - 1 : i;
            sum += t->w[(size_t)d * t->n + k];
        }
        for (k = 0; k < t->n; k++) t->w[(size_t)d * t->n + k] /= sum;
    }
    return true;
}

static void taps_free(Taps *t)
{
    free(t->idx);
    free(t->w);
    t->idx = NULL;
    t->w = NULL;
}

/* src: sw x sh float4 -> dst: dw x sh (horizontal taps) */
static void pass_h(const float *src, int sw, int sh, float *dst, int dw, const Taps *t)
{
    int y, x, k, c;
    (void)sw;
    for (y = 0; y < sh; y++)
        for (x = 0; x < dw; x++) {
            float acc[4] = { 0, 0, 0, 0 };
            const int *ix = t->idx + (size_t)x * t->n;
            const float *w = t->w + (size_t)x * t->n;
            for (k = 0; k < t->n; k++) {
                const float *s = src + ((size_t)y * sw + ix[k]) * 4;
                for (c = 0; c < 4; c++) acc[c] += s[c] * w[k];
            }
            memcpy(dst + ((size_t)y * dw + x) * 4, acc, sizeof acc);
        }
}

/* src: w x sh -> dst: w x dh (vertical taps) */
static void pass_v(const float *src, int w, float *dst, int dh, const Taps *t)
{
    int y, x, k, c;
    for (y = 0; y < dh; y++) {
        const int *ix = t->idx + (size_t)y * t->n;
        const float *wt = t->w + (size_t)y * t->n;
        for (x = 0; x < w; x++) {
            float acc[4] = { 0, 0, 0, 0 };
            for (k = 0; k < t->n; k++) {
                const float *s = src + ((size_t)ix[k] * w + x) * 4;
                for (c = 0; c < 4; c++) acc[c] += s[c] * wt[k];
            }
            memcpy(dst + ((size_t)y * w + x) * 4, acc, sizeof acc);
        }
    }
}

bool ot_image_resample(const OtImage *src, float sx0, float sy0, float sx1, float sy1,
                       int dw, int dh, OtImage *dst)
{
    Taps th = { 0 }, tv = { 0 };
    float *f = NULL, *tmp = NULL, *out = NULL;
    bool ok = false;
    size_t i;
    if (!src->px || dw <= 0 || dh <= 0 || sx1 <= sx0 || sy1 <= sy0) return false;
    if (!ot_image_alloc(dst, dw, dh)) return false;
    f = to_premul(src);
    tmp = (float *)malloc(sizeof(float) * 4 * (size_t)dw * src->h);
    out = (float *)malloc(sizeof(float) * 4 * (size_t)dw * dh);
    if (f && tmp && out && taps_resample(&th, dw, sx0, sx1, src->w) && taps_resample(&tv, dh, sy0, sy1, src->h)) {
        pass_h(f, src->w, src->h, tmp, dw, &th);
        pass_v(tmp, dw, out, dh, &tv);
        for (i = 0; i < (size_t)dw * dh; i++) dst->px[i] = pack_premul(out + i * 4);
        ok = true;
    }
    taps_free(&th);
    taps_free(&tv);
    free(f);
    free(tmp);
    free(out);
    if (!ok) ot_image_free(dst);
    return ok;
}

bool ot_image_resize(const OtImage *src, int dw, int dh, OtImage *dst)
{
    return ot_image_resample(src, 0, 0, (float)src->w, (float)src->h, dw, dh, dst);
}

bool ot_image_blur(OtImage *im, float sigma)
{
    Taps th = { 0 }, tv = { 0 };
    float *f, *tmp;
    bool ok = false;
    size_t i;
    if (sigma <= 0.0f) return true;
    if (!im->px) return false;
    f = to_premul(im);
    tmp = (float *)malloc(sizeof(float) * 4 * (size_t)im->w * im->h);
    if (f && tmp && taps_gauss(&th, im->w, sigma) && taps_gauss(&tv, im->h, sigma)) {
        pass_h(f, im->w, im->h, tmp, im->w, &th);
        pass_v(tmp, im->w, f, im->h, &tv);
        for (i = 0; i < (size_t)im->w * im->h; i++) im->px[i] = pack_premul(f + i * 4);
        ok = true;
    }
    taps_free(&th);
    taps_free(&tv);
    free(f);
    free(tmp);
    return ok;
}

/* ── Masks and compositing ───────────────────────────────────────── */

static bool in_round_rect(float px, float py, float x0, float y0, float x1, float y1, float r)
{
    float dx, dy;
    if (px < x0 || px > x1 || py < y0 || py > y1) return false;
    dx = px < x0 + r ? x0 + r - px : px > x1 - r ? px - (x1 - r) : 0.0f;
    dy = py < y0 + r ? y0 + r - py : py > y1 - r ? py - (y1 - r) : 0.0f;
    return dx * dx + dy * dy <= r * r;
}

uint8_t *ot_mask_round_rect(int w, int h, float x0, float y0, float x1, float y1, float radius)
{
    uint8_t *m = (uint8_t *)malloc((size_t)w * h);
    int x, y, i, j;
    if (!m) return NULL;
    if (radius > (x1 - x0) / 2) radius = (x1 - x0) / 2;
    if (radius > (y1 - y0) / 2) radius = (y1 - y0) / 2;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int c = 0;
            for (j = 0; j < 4; j++)
                for (i = 0; i < 4; i++)
                    c += in_round_rect((float)x + (i + 0.5f) / 4, (float)y + (j + 0.5f) / 4, x0, y0, x1, y1, radius);
            m[(size_t)y * w + x] = (uint8_t)((c * 255 + 8) / 16);
        }
    return m;
}

uint8_t *ot_mask_ellipse(int w, int h, float x0, float y0, float x1, float y1)
{
    uint8_t *m = (uint8_t *)malloc((size_t)w * h);
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    int x, y, i, j;
    if (!m) return NULL;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int c = 0;
            for (j = 0; j < 4; j++)
                for (i = 0; i < 4; i++) {
                    float dx = ((float)x + (i + 0.5f) / 4 - cx) / rx, dy = ((float)y + (j + 0.5f) / 4 - cy) / ry;
                    c += dx * dx + dy * dy <= 1.0f;
                }
            m[(size_t)y * w + x] = (uint8_t)((c * 255 + 8) / 16);
        }
    return m;
}

void ot_image_apply_mask(OtImage *im, const uint8_t *mask)
{
    size_t i;
    for (i = 0; i < (size_t)im->w * im->h; i++) {
        uint32_t a = (im->px[i] >> 24) * mask[i];
        im->px[i] = (im->px[i] & 0xFFFFFFu) | (((a + 127) / 255) << 24);
    }
}

void ot_image_blend(OtImage *dst, const OtImage *src, int ox, int oy)
{
    int x, y;
    for (y = 0; y < src->h; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= dst->h) continue;
        for (x = 0; x < src->w; x++) {
            int dx = ox + x;
            uint32_t s, d;
            float sa, da, oa;
            int c;
            uint32_t out = 0;
            if (dx < 0 || dx >= dst->w) continue;
            s = src->px[(size_t)y * src->w + x];
            d = dst->px[(size_t)dy * dst->w + dx];
            sa = (float)(s >> 24) / 255.0f;
            if (sa <= 0.0f) continue;
            da = (float)(d >> 24) / 255.0f;
            oa = sa + da * (1.0f - sa);
            for (c = 0; c < 3; c++) {
                float sc = (float)((s >> (c * 8)) & 255), dc = (float)((d >> (c * 8)) & 255);
                float v = (sc * sa + dc * da * (1.0f - sa)) / oa;
                out |= (uint32_t)(v + 0.5f) << (c * 8);
            }
            dst->px[(size_t)dy * dst->w + dx] = out | ((uint32_t)(oa * 255.0f + 0.5f) << 24);
        }
    }
}

/* ── Block compression ───────────────────────────────────────────── */

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void rgb565(uint16_t c, int *r, int *g, int *b)
{
    int r5 = c >> 11, g6 = (c >> 5) & 63, b5 = c & 31;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

/* Colour part of BC1/2/3 (alpha left at 255). four_colour: BC2 / BC3 always. */
static void colour_block(const uint8_t *b, uint32_t out[16], bool four_colour, bool punch_alpha)
{
    uint16_t c0 = rd16(b), c1 = rd16(b + 2);
    uint32_t bits = rd32(b + 4), pal[4];
    int r[4], g[4], bl[4], k;
    rgb565(c0, &r[0], &g[0], &bl[0]);
    rgb565(c1, &r[1], &g[1], &bl[1]);
    if (four_colour || c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; bl[2] = (2 * bl[0] + bl[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; bl[3] = (bl[0] + 2 * bl[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; bl[2] = (bl[0] + bl[1]) / 2;
        r[3] = g[3] = bl[3] = 0;
    }
    for (k = 0; k < 4; k++) pal[k] = 0xFF000000u | ((uint32_t)r[k] << 16) | ((uint32_t)g[k] << 8) | (uint32_t)bl[k];
    if (!four_colour && c0 <= c1 && punch_alpha) pal[3] = 0;
    for (k = 0; k < 16; k++) out[k] = pal[(bits >> (2 * k)) & 3];
}

void ot_decode_bc1(const uint8_t *b, uint32_t out[16], bool punch_alpha)
{
    colour_block(b, out, false, punch_alpha);
}

void ot_decode_bc2(const uint8_t *b, uint32_t out[16])
{
    int k;
    colour_block(b + 8, out, true, false);
    for (k = 0; k < 16; k++) {
        uint32_t a = ((b[k / 2] >> ((k & 1) * 4)) & 15) * 17;
        out[k] = (out[k] & 0xFFFFFFu) | (a << 24);
    }
}

void ot_decode_bc3(const uint8_t *b, uint32_t out[16])
{
    uint32_t a[8], k;
    uint64_t bits = 0;
    colour_block(b + 8, out, true, false);
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
    } else {
        for (k = 1; k < 5; k++) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5;
        a[6] = 0;
        a[7] = 255;
    }
    for (k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
    for (k = 0; k < 16; k++) out[k] = (out[k] & 0xFFFFFFu) | (a[(bits >> (3 * k)) & 7] << 24);
}

/* BC7. Partition and anchor tables: the format's fixed tables (checked
 * entry by entry against an independent decoder). */
static const uint8_t bc7_part2[64][16] = {
    {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1},
    {0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1},
    {0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1},
    {0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1},
    {0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1},
    {0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1},
    {0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
    {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1},
    {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
    {0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1},
    {0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0},
    {0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0},
    {0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0},
    {0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0},
    {0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
    {0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0},
    {0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0},
    {0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0},
    {0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0},
    {0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0},
    {0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0},
    {0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0},
    {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},
    {0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1},
    {0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0},
    {0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
    {0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0},
    {0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0},
    {0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1},
    {0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1},
    {0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0},
    {0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0},
    {0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0},
    {0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0},
    {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0},
    {0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1},
    {0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1},
    {0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
    {0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0},
    {0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0},
    {0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0},
    {0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0},
    {0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1},
    {0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1},
    {0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0},
    {0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0},
    {0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1},
    {0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1},
    {0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1},
    {0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
    {0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1},
    {0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0},
    {0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0},
    {0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1},
};
static const uint8_t bc7_part3[64][16] = {
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2},
    {0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},
    {0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},
    {0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2},
    {0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},
    {0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1},
    {0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},
    {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},
    {0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2},
    {0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2},
    {0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},
    {0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},
    {0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
    {0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2},
    {0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},
    {0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2},
    {0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
    {0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},
    {0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},
    {0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2},
    {0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0},
    {0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},
    {0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},
    {0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
    {0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2},
    {0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},
    {0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1},
    {0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
    {0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},
    {0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},
    {0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2},
    {0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0},
    {0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},
    {0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},
    {0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
    {0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1},
    {0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},
    {0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1},
    {0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
    {0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},
    {0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},
    {0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1},
    {0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2},
    {0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},
    {0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},
    {0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
    {0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2},
    {0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},
    {0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2},
    {0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
    {0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},
    {0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},
    {0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2},
    {0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1},
    {0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},
    {0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},
    {0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0},
};
static const uint8_t bc7_anchor2[64] = {15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,2,8,2,2,8,8,15,2,8,2,2,8,8,2,2,15,15,6,8,2,8,15,15,2,8,2,2,2,15,15,6,6,2,6,8,15,15,2,2,15,15,15,15,15,2,2,15};
static const uint8_t bc7_anchor3a[64] = {3,3,15,15,8,3,15,15,8,8,6,6,6,5,3,3,3,3,8,15,3,3,6,10,5,8,8,6,8,5,15,15,8,15,3,5,6,10,8,15,15,3,15,5,15,15,15,15,3,15,5,5,5,8,5,10,5,10,8,13,15,12,3,3};
static const uint8_t bc7_anchor3b[64] = {15,8,8,3,15,15,3,8,15,15,15,15,15,15,15,8,15,8,15,3,15,8,15,8,3,15,6,10,15,15,10,8,15,3,15,10,10,8,9,10,6,15,8,15,3,6,6,8,15,3,15,15,15,15,15,15,15,15,15,15,3,15,15,8};

static const uint8_t bc7_w2[4] = { 0, 21, 43, 64 };
static const uint8_t bc7_w3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const uint8_t bc7_w4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

typedef struct {
    uint8_t ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2;
} Bc7Mode;

static const Bc7Mode bc7_modes[8] = {
    { 3, 4, 0, 0, 4, 0, 1, 0, 3, 0 },
    { 2, 6, 0, 0, 6, 0, 0, 1, 3, 0 },
    { 3, 6, 0, 0, 5, 0, 0, 0, 2, 0 },
    { 2, 6, 0, 0, 7, 0, 1, 0, 2, 0 },
    { 1, 0, 2, 1, 5, 6, 0, 0, 2, 3 },
    { 1, 0, 2, 0, 7, 8, 0, 0, 2, 2 },
    { 1, 0, 0, 0, 7, 7, 1, 0, 4, 0 },
    { 2, 6, 0, 0, 5, 5, 1, 0, 2, 0 },
};

typedef struct {
    const uint8_t *b;
    unsigned       pos;
} Bits;

static unsigned getbits(Bits *s, unsigned n)
{
    unsigned v = 0, i;
    for (i = 0; i < n; i++, s->pos++)
        if (s->pos < 128) v |= (unsigned)((s->b[s->pos >> 3] >> (s->pos & 7)) & 1) << i;
    return v;
}

static const uint8_t *bc7_weights(unsigned bits)
{
    return bits == 2 ? bc7_w2 : bits == 3 ? bc7_w3 : bc7_w4;
}

void ot_decode_bc7(const uint8_t *blk, uint32_t out[16])
{
    Bits s = { blk, 0 };
    const Bc7Mode *m;
    unsigned mode = 0, part, rot, isel, sub, e, c, i;
    uint8_t ep[3][2][4];
    uint8_t idx[16], idx2[16];
    const uint8_t *ptab = NULL;

    while (mode < 8 && !((blk[0] >> mode) & 1)) mode++;
    if (mode == 8) {
        for (i = 0; i < 16; i++) out[i] = 0;
        return;
    }
    s.pos = mode + 1;
    m = &bc7_modes[mode];
    part = getbits(&s, m->pb);
    rot = getbits(&s, m->rb);
    isel = getbits(&s, m->isb);
    for (c = 0; c < 3; c++)
        for (sub = 0; sub < m->ns; sub++)
            for (e = 0; e < 2; e++) ep[sub][e][c] = (uint8_t)getbits(&s, m->cb);
    for (sub = 0; sub < m->ns; sub++)
        for (e = 0; e < 2; e++) ep[sub][e][3] = m->ab ? (uint8_t)getbits(&s, m->ab) : 255;
    {
        unsigned cbits = m->cb, abits = m->ab;
        if (m->epb || m->spb) {
            uint8_t p[3][2];
            for (sub = 0; sub < m->ns; sub++) {
                if (m->epb) { p[sub][0] = (uint8_t)getbits(&s, 1); p[sub][1] = (uint8_t)getbits(&s, 1); }
            }
            if (m->spb)
                for (sub = 0; sub < m->ns; sub++) p[sub][0] = p[sub][1] = (uint8_t)getbits(&s, 1);
            for (sub = 0; sub < m->ns; sub++)
                for (e = 0; e < 2; e++) {
                    for (c = 0; c < 3; c++) ep[sub][e][c] = (uint8_t)((ep[sub][e][c] << 1) | p[sub][e]);
                    if (m->ab) ep[sub][e][3] = (uint8_t)((ep[sub][e][3] << 1) | p[sub][e]);
                }
            cbits++;
            if (abits) abits++;
        }
        for (sub = 0; sub < m->ns; sub++)
            for (e = 0; e < 2; e++) {
                for (c = 0; c < 3; c++) {
                    unsigned v = ep[sub][e][c];
                    ep[sub][e][c] = (uint8_t)((v << (8 - cbits)) | (v >> (2 * cbits - 8)));
                }
                if (abits) {
                    unsigned v = ep[sub][e][3];
                    ep[sub][e][3] = (uint8_t)((v << (8 - abits)) | (v >> (2 * abits - 8)));
                }
            }
    }
    if (m->ns == 2) ptab = bc7_part2[part];
    else if (m->ns == 3) ptab = bc7_part3[part];
    for (i = 0; i < 16; i++) {
        bool anchor = i == 0 ||
                      (m->ns == 2 && i == bc7_anchor2[part]) ||
                      (m->ns == 3 && (i == bc7_anchor3a[part] || i == bc7_anchor3b[part]));
        idx[i] = (uint8_t)getbits(&s, m->ib - (anchor ? 1 : 0));
    }
    if (m->ib2)
        for (i = 0; i < 16; i++) idx2[i] = (uint8_t)getbits(&s, m->ib2 - (i == 0 ? 1 : 0));
    for (i = 0; i < 16; i++) {
        unsigned sb = ptab ? ptab[i] : 0, wc, wa;
        uint8_t px[4];
        if (m->ib2) {
            if (isel) { wc = bc7_weights(m->ib2)[idx2[i]]; wa = bc7_weights(m->ib)[idx[i]]; }
            else      { wc = bc7_weights(m->ib)[idx[i]];   wa = bc7_weights(m->ib2)[idx2[i]]; }
        } else {
            wc = wa = bc7_weights(m->ib)[idx[i]];
        }
        for (c = 0; c < 3; c++) px[c] = (uint8_t)(((64 - wc) * ep[sb][0][c] + wc * ep[sb][1][c] + 32) >> 6);
        px[3] = (uint8_t)(((64 - wa) * ep[sb][0][3] + wa * ep[sb][1][3] + 32) >> 6);
        if (rot) {
            uint8_t t = px[3];
            px[3] = px[rot - 1];
            px[rot - 1] = t;
        }
        out[i] = ((uint32_t)px[3] << 24) | ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
    }
}

/* ── DDS ─────────────────────────────────────────────────────────── */

enum { F_NONE, F_BC1, F_BC2, F_BC3, F_BC7, F_RGBA, F_BGRA, F_BGRX };

static int dds_format(const uint8_t *d, size_t n, size_t *data_off)
{
    uint32_t pf_flags, fourcc;
    if (n < 128 || memcmp(d, "DDS ", 4) != 0 || rd32(d + 4) != 124) return F_NONE;
    pf_flags = rd32(d + 80);
    fourcc = rd32(d + 84);
    *data_off = 128;
    if (pf_flags & 0x4) {                                     /* DDPF_FOURCC */
        if (fourcc == 0x30315844u) {                          /* "DX10" */
            uint32_t dxgi;
            if (n < 148) return F_NONE;
            dxgi = rd32(d + 128);
            *data_off = 148;
            switch (dxgi) {
            case 71: case 72: return F_BC1;
            case 74: case 75: return F_BC2;
            case 77: case 78: return F_BC3;
            case 98: case 99: return F_BC7;
            case 28: case 29: return F_RGBA;
            case 87: case 91: return F_BGRA;
            case 88: case 93: return F_BGRX;
            default: return F_NONE;
            }
        }
        if (!memcmp(d + 84, "DXT1", 4)) return F_BC1;
        if (!memcmp(d + 84, "DXT2", 4) || !memcmp(d + 84, "DXT3", 4)) return F_BC2;
        if (!memcmp(d + 84, "DXT4", 4) || !memcmp(d + 84, "DXT5", 4)) return F_BC3;
        return F_NONE;
    }
    if ((pf_flags & 0x40) && rd32(d + 88) == 32) {            /* DDPF_RGB, 32 bits */
        uint32_t rmask = rd32(d + 92), amask = rd32(d + 104);
        if (rmask == 0x00FF0000u) return (pf_flags & 0x1) && amask ? F_BGRA : F_BGRX;
        if (rmask == 0x000000FFu) return F_RGBA;
    }
    return F_NONE;
}

static size_t level_size(int fmt, int w, int h)
{
    size_t bw = (size_t)((w + 3) / 4), bh = (size_t)((h + 3) / 4);
    switch (fmt) {
    case F_BC1: return bw * bh * 8;
    case F_BC2: case F_BC3: case F_BC7: return bw * bh * 16;
    default: return (size_t)w * h * 4;
    }
}

bool ot_dds_info(const uint8_t *d, size_t n, int *w, int *h, int *mips)
{
    size_t off;
    if (dds_format(d, n, &off) == F_NONE) return false;
    *h = (int)rd32(d + 12);
    *w = (int)rd32(d + 16);
    *mips = (rd32(d + 8) & 0x20000) && rd32(d + 28) ? (int)rd32(d + 28) : 1;
    return *w > 0 && *h > 0 && *w <= 16384 && *h <= 16384;
}

bool ot_dds_decode(const uint8_t *d, size_t n, int min_w, OtImage *out)
{
    size_t off;
    int fmt = dds_format(d, n, &off), w, h, mips, level = 0, x, y, k;
    if (fmt == F_NONE || !ot_dds_info(d, n, &w, &h, &mips)) return false;
    while (min_w > 0 && level + 1 < mips && (w / 2) >= min_w && w > 1) {
        off += level_size(fmt, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        level++;
    }
    if (off > n || level_size(fmt, w, h) > n - off) return false;
    if (!ot_image_alloc(out, w, h)) return false;
    d += off;
    if (fmt == F_RGBA || fmt == F_BGRA || fmt == F_BGRX) {
        for (k = 0; k < w * h; k++) {
            const uint8_t *p = d + (size_t)k * 4;
            uint32_t r = fmt == F_RGBA ? p[0] : p[2], b = fmt == F_RGBA ? p[2] : p[0];
            uint32_t a = fmt == F_BGRX ? 255 : p[3];
            out->px[k] = (a << 24) | (r << 16) | ((uint32_t)p[1] << 8) | b;
        }
        return true;
    }
    {
        size_t bsz = fmt == F_BC1 ? 8 : 16, bw = (size_t)((w + 3) / 4);
        for (y = 0; y < h; y += 4)
            for (x = 0; x < w; x += 4) {
                const uint8_t *b = d + ((size_t)(y / 4) * bw + (size_t)(x / 4)) * bsz;
                uint32_t px[16];
                switch (fmt) {
                case F_BC1: ot_decode_bc1(b, px, true); break;
                case F_BC2: ot_decode_bc2(b, px); break;
                case F_BC3: ot_decode_bc3(b, px); break;
                default:    ot_decode_bc7(b, px); break;
                }
                for (k = 0; k < 16; k++) {
                    int xx = x + (k & 3), yy = y + (k >> 2);
                    if (xx < w && yy < h) out->px[(size_t)yy * w + xx] = px[k];
                }
            }
    }
    return true;
}
