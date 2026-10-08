/*
 * ot_theme.c -- see ot_theme.h.
 */
#include "ot_theme.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

/* ── Colour helpers ──────────────────────────────────────────────── */

#define ARGB(r, g, b) (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define CH(c, s) ((float)(((c) >> (s)) & 255) / 255.0f)

static void to_hls(uint32_t c, float *h, float *l, float *s)
{
    float r = CH(c, 16), g = CH(c, 8), b = CH(c, 0);
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *l = (mx + mn) / 2;
    if (d <= 0.0f) { *h = 0; *s = 0; return; }
    *s = *l <= 0.5f ? d / (mx + mn) : d / (2.0f - mx - mn);
    if (mx == r)      *h = (g - b) / d;
    else if (mx == g) *h = 2.0f + (b - r) / d;
    else              *h = 4.0f + (r - g) / d;
    *h /= 6.0f;
    if (*h < 0) *h += 1.0f;
}

static float hls_v(float m1, float m2, float h)
{
    h -= floorf(h);
    if (h < 1.0f / 6) return m1 + (m2 - m1) * h * 6;
    if (h < 0.5f) return m2;
    if (h < 2.0f / 3) return m1 + (m2 - m1) * (2.0f / 3 - h) * 6;
    return m1;
}

/* Truncating, like the mock-ups' tool. */
static uint32_t from_hls(float h, float l, float s)
{
    float r, g, b, m2, m1;
    if (s > 1) s = 1;
    if (s <= 0) { r = g = b = l; }
    else {
        m2 = l <= 0.5f ? l * (1 + s) : l + s - l * s;
        m1 = 2 * l - m2;
        r = hls_v(m1, m2, h + 1.0f / 3);
        g = hls_v(m1, m2, h);
        b = hls_v(m1, m2, h - 1.0f / 3);
    }
    return ARGB((int)(r * 255), (int)(g * 255), (int)(b * 255));
}

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    int c;
    uint32_t out = 0;
    for (c = 0; c < 32; c += 8) {
        float v = CH(a, c) * (1 - t) + CH(b, c) * t;
        out |= (uint32_t)(v * 255.0f + 0.5f) << c;
    }
    return out;
}

static float luma(uint32_t c) { return 0.2126f * CH(c, 16) + 0.7152f * CH(c, 8) + 0.0722f * CH(c, 0); }

static const float s_track_pos[5] = { 0.0f, 0.30f, 0.62f, 0.88f, 1.0f };

/* ── The tuned palette ───────────────────────────────────────────────
 * The colours of the approved mock-ups: accent (from the emblem), its dark
 * and light ends, PLAY's lettering, and the five stops of the background
 * (two main hues of the card). */
typedef struct {
    uint32_t acc, dark, light, text;
    uint32_t grad[5];
} Tuned;

static const Tuned s_tuned[OT_TRACK_COUNT] = {
    /* alas */ { 0xff6a1a, 0xc43d00, 0xff9a52, 0xffffff, { 0x030515, 0x090c34, 0x131b70, 0x00b5cb, 0x0be4fe } },
    /* aloh */ { 0x2fc9d0, 0x0f7f8a, 0x7fe9ee, 0x06262a, { 0x03150a, 0x093319, 0x146f37, 0x2e479d, 0x425fc6 } },
    /* elys */ { 0xd63a9c, 0x8f1866, 0xf07cc4, 0xffffff, { 0x140805, 0x30150c, 0x692d1a, 0xad1f72, 0xd92f94 } },
    /* gari */ { 0x2a9dff, 0x1450b8, 0x5fd0ff, 0xffffff, { 0x001219, 0x012c3c, 0x026082, 0x2753a4, 0x396ecf } },
    /* merq */ { 0x8fa3c4, 0x4a5d80, 0xc6d3ea, 0x0b0f18, { 0x030e16, 0x072335, 0x104d74, 0x1e37ad, 0x2f4cda } },
    /* mesa */ { 0xff8c1a, 0xc45a00, 0xffb45e, 0x1f1004, { 0x180d01, 0x3a1f02, 0x7f4405, 0xb78014, 0xe6a422 } },
    /* pipe */ { 0x6cc62e, 0x3a7f12, 0xa5e86e, 0x0b1805, { 0x0e1107, 0x222a13, 0x495b29, 0x4b9338, 0x64bb4d } },
    /* snow */ { 0xe8222e, 0x9c0d16, 0xff6a6f, 0xffffff, { 0x170102, 0x380405, 0x7a090b, 0x9d5d2e, 0xc77a42 } },
    /* toky */ { 0x7a4cff, 0x4120b8, 0xa98bff, 0xffffff, { 0x18000a, 0x3b0119, 0x800336, 0xaa4922, 0xd66233 } },
    /* untr */ { 0xffc21a, 0xc48a00, 0xffdb6b, 0x1c1504, { 0x140412, 0x320a2d, 0x6c1761, 0x9e682d, 0xc88741 } },
};

void ot_theme_default(OtTheme *t)
{
    static const uint32_t g[5] = { 0xFF050C1C, 0xFF081836, 0xFF0A2350, 0xFF0E316B, 0xFF123F86 };
    static const float pos[5] = { 0.0f, 0.275f, 0.55f, 0.775f, 1.0f };
    memset(t, 0, sizeof *t);
    t->acc = 0xFF2F8CFF;
    t->acc_dark = 0xFF1450B8;
    t->acc_light = 0xFF5FD0FF;
    t->glow = 0x592F8CFF;
    t->play_text = 0xFFFFFFFF;
    memcpy(t->grad, g, sizeof g);
    memcpy(t->grad_pos, pos, sizeof pos);
    t->grad_angle = 135.0f;
    t->from_disc = false;
}

/* ── Measuring a theme ───────────────────────────────────────────── */

/* Accent: the emblem's dominant vivid hue (HSV s and v > 0.45), averaged,
 * brought to full brightness. A mostly silver emblem (under 8 % vivid
 * pixels) keeps its light grey-blue. */
static void measure_accent(const OtImage *em, OtTheme *t)
{
    float hist[24] = { 0 }, sum[24][3] = { { 0 } }, gr[3] = { 0 }, ng = 0;
    int i, best = 0, n = 0, nv = 0;
    for (i = 0; i < em->w * em->h; i++) {
        uint32_t p = em->px[i];
        float r = CH(p, 16), g = CH(p, 8), b = CH(p, 0), mx, mn, h, l, s;
        if ((p >> 24) < 200) continue;
        n++;
        mx = fmaxf(r, fmaxf(g, b)); mn = fminf(r, fminf(g, b));
        if (mx > 0.45f && (mx - mn) / mx > 0.45f) {
            int bin;
            to_hls(p, &h, &l, &s);
            bin = (int)(h * 24) % 24;
            hist[bin] += 1; sum[bin][0] += r; sum[bin][1] += g; sum[bin][2] += b;
            nv++;
        } else if (mx > 0.55f) {
            gr[0] += r; gr[1] += g; gr[2] += b; ng++;
        }
    }
    for (i = 1; i < 24; i++) if (hist[i] > hist[best]) best = i;
    if (n && nv * 100 >= n * 8) {
        float c[3] = { 0, 0, 0 }, w = 0, h, l, s;
        int k;
        for (k = -1; k <= 1; k++) {
            int bi = (best + k + 24) % 24;
            c[0] += sum[bi][0]; c[1] += sum[bi][1]; c[2] += sum[bi][2]; w += hist[bi];
        }
        to_hls(ARGB((int)(c[0] / w * 255), (int)(c[1] / w * 255), (int)(c[2] / w * 255)), &h, &l, &s);
        t->acc = from_hls(h, 0.55f, fmaxf(s, 0.80f));
        t->acc_dark = from_hls(h, 0.36f, fmaxf(s, 0.85f));
        t->acc_light = from_hls(h, 0.70f, 1.0f);
    } else {
        float h, l, s;
        if (ng < 1) ng = 1;
        to_hls(ARGB((int)(gr[0] / ng * 255), (int)(gr[1] / ng * 255), (int)(gr[2] / ng * 255)), &h, &l, &s);
        t->acc = from_hls(h, 0.66f, fminf(fmaxf(s, 0.25f), 0.35f));
        t->acc_dark = from_hls(h, 0.40f, 0.27f);
        t->acc_light = from_hls(h, 0.85f, 0.45f);
    }
    t->glow = (t->acc & 0xFFFFFFu) | 0x66000000u;
    {
        float h, l, s;
        to_hls(t->acc, &h, &l, &s);
        t->play_text = luma(t->acc) > 0.45f ? from_hls(h, 0.07f, 0.6f) : 0xFFFFFFFFu;
    }
}

/* Median cut to `k` colours (largest box first, split at the median of its
 * widest channel); colour = box mean, count = box size. */
typedef struct { uint8_t c[3]; } Rgb;

static int cmp_ch;
static int cmp_rgb(const void *a, const void *b)
{
    return (int)((const Rgb *)a)->c[cmp_ch] - (int)((const Rgb *)b)->c[cmp_ch];
}

static int median_cut(Rgb *px, int n, int k, uint32_t *col, int *count)
{
    int lo[16], hi[16], nb = 1, i, c;
    lo[0] = 0; hi[0] = n;
    while (nb < k) {
        int bi = -1, ch = 0;
        for (i = 0; i < nb; i++) {
            int mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 }, j, wide = 0;
            if (hi[i] - lo[i] < 2 || (bi >= 0 && hi[i] - lo[i] <= hi[bi] - lo[bi])) continue;
            for (j = lo[i]; j < hi[i]; j++)
                for (c = 0; c < 3; c++) {
                    if (px[j].c[c] < mn[c]) mn[c] = px[j].c[c];
                    if (px[j].c[c] > mx[c]) mx[c] = px[j].c[c];
                }
            for (c = 1; c < 3; c++)
                if (mx[c] - mn[c] > mx[wide] - mn[wide]) wide = c;
            if (mx[wide] > mn[wide]) { bi = i; ch = wide; }
        }
        if (bi < 0) break;
        cmp_ch = ch;
        qsort(px + lo[bi], (size_t)(hi[bi] - lo[bi]), sizeof(Rgb), cmp_rgb);
        lo[nb] = (lo[bi] + hi[bi]) / 2;
        hi[nb] = hi[bi];
        hi[bi] = lo[nb];
        nb++;
    }
    for (i = 0; i < nb; i++) {
        double s[3] = { 0, 0, 0 };
        int j, m = hi[i] - lo[i];
        for (j = lo[i]; j < hi[i]; j++)
            for (c = 0; c < 3; c++) s[c] += px[j].c[c];
        col[i] = m ? ARGB((int)(s[0] / m + 0.5), (int)(s[1] / m + 0.5), (int)(s[2] / m + 0.5)) : 0;
        count[i] = m;
    }
    return nb;
}

/* Background: the card (128 x 40, opaque texels) cut to 8 colours; the most
 * common one with HLS saturation over 0.35 and lightness in 0.2..0.85 gives
 * the dark stops (lightness 0.05, 0.12, 0.26), the next one of a clearly
 * different hue (over 0.06 of the circle) the bright stops (0.40, 0.52). */
static bool measure_gradient(const OtImage *card_tex, OtTheme *t)
{
    OtImage sm;
    Rgb *px;
    uint32_t col[8];
    int cnt[8], order[8], n = 0, k, i, j, a = -1, b = -1;
    float hh[8], ll[8], ss[8];
    if (!ot_image_resize(card_tex, 128, 40, &sm)) return false;
    px = (Rgb *)malloc(sizeof(Rgb) * (size_t)sm.w * sm.h);
    if (!px) { ot_image_free(&sm); return false; }
    for (i = 0; i < sm.w * sm.h; i++)
        if ((sm.px[i] >> 24) > 200) {
            px[n].c[0] = (uint8_t)(sm.px[i] >> 16); px[n].c[1] = (uint8_t)(sm.px[i] >> 8); px[n].c[2] = (uint8_t)sm.px[i];
            n++;
        }
    ot_image_free(&sm);
    k = n ? median_cut(px, n, 8, col, cnt) : 0;
    free(px);
    for (i = 0; i < k; i++) order[i] = i;
    for (i = 1; i < k; i++)
        for (j = i; j > 0 && cnt[order[j]] > cnt[order[j - 1]]; j--) { int x = order[j]; order[j] = order[j - 1]; order[j - 1] = x; }
    for (i = 0; i < k; i++) {
        int o = order[i];
        to_hls(col[o], &hh[o], &ll[o], &ss[o]);
        if (!(ss[o] > 0.35f && ll[o] > 0.2f && ll[o] < 0.85f)) continue;
        if (a < 0) a = o;
        else if (b < 0) {
            float d = fabsf(hh[o] - hh[a]);
            if (fminf(d, 1 - d) > 0.06f) b = o;
        }
    }
    if (a < 0) return false;
    if (b < 0) b = a;
    t->grad[0] = from_hls(hh[a], 0.05f, ss[a]);
    t->grad[1] = from_hls(hh[a], 0.12f, ss[a]);
    t->grad[2] = from_hls(hh[a], 0.26f, ss[a]);
    t->grad[3] = from_hls(hh[b], 0.40f, ss[b]);
    t->grad[4] = from_hls(hh[b], 0.52f, ss[b]);
    memcpy(t->grad_pos, s_track_pos, sizeof s_track_pos);
    t->grad_angle = 120.0f;
    return true;
}

bool ot_theme_measure(const OtDiscAssets *a, int track, OtTheme *t)
{
    ot_theme_default(t);
    if (!a || !a->ok || track < 0 || track >= OT_TRACK_COUNT) return false;
    measure_accent(&a->emblem[track], t);
    if (!measure_gradient(&a->card_tex[track], t)) return false;
    t->from_disc = true;
    return true;
}

void ot_theme_for_track(const OtDiscAssets *a, int track, bool tuned, OtTheme *t)
{
    if (!tuned) {
        if (!ot_theme_measure(a, track, t)) ot_theme_default(t);
        return;
    }
    ot_theme_default(t);
    if (track < 0 || track >= OT_TRACK_COUNT) return;
    {
        const Tuned *u = &s_tuned[track];
        int i;
        t->acc = 0xFF000000u | u->acc;
        t->acc_dark = 0xFF000000u | u->dark;
        t->acc_light = 0xFF000000u | u->light;
        t->glow = 0x66000000u | u->acc;
        t->play_text = 0xFF000000u | u->text;
        for (i = 0; i < 5; i++) t->grad[i] = 0xFF000000u | u->grad[i];
        memcpy(t->grad_pos, s_track_pos, sizeof s_track_pos);
        t->grad_angle = 120.0f;
        t->from_disc = a && a->ok;
    }
}

/* ── Background ──────────────────────────────────────────────────── */

bool ot_theme_background(const OtTheme *t, int w, int h, OtImage *out)
{
    float rad = t->grad_angle * 3.14159265f / 180.0f, dx = sinf(rad), dy = -cosf(rad);
    float len = fabsf((float)w * dx) + fabsf((float)h * dy);
    int x, y, i;
    if (!ot_image_alloc(out, w, h)) return false;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float p = (((float)x + 0.5f - w / 2.0f) * dx + ((float)y + 0.5f - h / 2.0f) * dy) / len + 0.5f;
            uint32_t c = t->grad[4];
            if (p <= t->grad_pos[0]) c = t->grad[0];
            else
                for (i = 0; i < 4; i++)
                    if (p <= t->grad_pos[i + 1]) {
                        float span = t->grad_pos[i + 1] - t->grad_pos[i];
                        c = mix(t->grad[i], t->grad[i + 1], span > 0 ? (p - t->grad_pos[i]) / span : 1);
                        break;
                    }
            if (!t->from_disc) {
                /* glow: ellipse 900 x 600 (at 1280 x 800) centred at 85 %, 20 %, light blue at 25 % fading out by 60 % */
                float ex = ((float)x - 0.85f * w) / (900.0f * w / 1280.0f), ey = ((float)y - 0.20f * h) / (600.0f * h / 800.0f);
                float r = sqrtf(ex * ex + ey * ey) / 0.6f, gl = r < 1 ? 0.25f * (1 - r) : 0;
                float cell = 26.0f * w / 1280.0f, cx = fmodf((float)x, cell) - cell / 2, cy = fmodf((float)y, cell) - cell / 2;
                float d = sqrtf(cx * cx + cy * cy), dot = d < 1.2f ? 1.0f : d < 1.6f ? (1.6f - d) / 0.4f : 0.0f;
                float fx = (float)x / w, m = fx < 0.2f ? 0 : fx > 0.8f ? 1 : (fx - 0.2f) / 0.6f;
                c = mix(c, 0xFF5FD0FFu, gl);
                c = mix(c, 0xFFFFFFFFu, 0.10f * dot * m);
            }
            out->px[(size_t)y * w + x] = c | 0xFF000000u;
        }
    return true;
}

/* ── Badges ──────────────────────────────────────────────────────── */

uint32_t ot_badge_rim_colour(const OtImage *im)
{
    int x, y, n = 0, c;
    int hist[3][256];
    memset(hist, 0, sizeof hist);
    for (y = (int)(im->h * 0.72f); y < (int)(im->h * 0.93f); y++)
        for (x = (int)(im->w * 0.62f); x < (int)(im->w * 0.96f); x++) {
            uint32_t p = im->px[(size_t)y * im->w + x];
            int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if ((p >> 24) < 200 || (r + g + b) <= 3 * 150) continue;
            hist[0][r]++; hist[1][g]++; hist[2][b]++;
            n++;
        }
    if (n < 50) return OT_BADGE_RIM_DEFAULT;
    {
        uint32_t out = 0xFF000000u;
        for (c = 0; c < 3; c++) {
            int acc = 0, v = 0;
            while (v < 255 && (acc += hist[c][v]) * 2 < n) v++;
            out |= (uint32_t)v << (16 - 8 * c);
        }
        return out;
    }
}

bool ot_badge_make(const OtImage *card, float blur_sigma, int w, OtImage *out)
{
    OtImage src, img;
    int h = w * 5 / 16, i;
    float k = (float)w / 1040.0f, inset = 9 * k, rad = 92 * k, rim = 13 * k;
    uint8_t *outer, *inner;
    uint32_t rc;
    if (!card || !card->px || w < 32) return false;
    if (!ot_image_copy(card, &src)) return false;
    if (!ot_image_blur(&src, blur_sigma) || !ot_image_resize(&src, w, h, &img)) { ot_image_free(&src); return false; }
    ot_image_free(&src);
    rc = OT_BADGE_RIM_DEFAULT;   /* fixed #c5c5c3, not measured per card */
    outer = ot_mask_round_rect(w, h, inset, inset, (float)w - inset, (float)h - inset, rad);
    inner = ot_mask_round_rect(w, h, inset + rim, inset + rim, (float)w - inset - rim, (float)h - inset - rim, rad - rim);
    if (!outer || !inner || !ot_image_alloc(out, w, h)) { free(outer); free(inner); ot_image_free(&img); return false; }
    for (i = 0; i < w * h; i++) {
        /* the picture over the rim colour (no transparent texel shows through), inside the inner shape */
        uint32_t p = img.px[i], a = p >> 24, pic = 0xFF000000u, c;
        for (c = 0; c < 24; c += 8)
            pic |= ((((p >> c) & 255) * a + ((rc >> c) & 255) * (255 - a) + 127) / 255) << c;
        pic = mix(rc, pic, inner[i] / 255.0f);
        out->px[i] = (pic & 0xFFFFFFu) | ((uint32_t)outer[i] << 24);
    }
    free(outer);
    free(inner);
    ot_image_free(&img);
    return true;
}

bool ot_badge_from_disc(const OtDiscAssets *a, int track, int w, OtImage *out)
{
    if (!a || !a->ok || track < 0 || track >= OT_TRACK_COUNT) return false;
    return ot_badge_make(&a->card_tex[track], 0.8f, w, out);
}

bool ot_badge_ot(int size, OtImage *out)
{
    uint8_t *m;
    int x, y;
    if (!ot_image_alloc(out, size, size)) return false;
    m = ot_mask_round_rect(size, size, 0, 0, (float)size, (float)size, size * 13.0f / 46.0f);
    if (!m) { ot_image_free(out); return false; }
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
            out->px[(size_t)y * size + x] = mix(0xFF3AA0FFu, 0xFF1450B8u, (float)(x + y) / (2.0f * size - 2));
    ot_image_apply_mask(out, m);
    free(m);
    return true;
}

/* ── HD cards from a texture pack ────────────────────────────────── */

typedef struct {
    char **path;
    int    n, cap;
} PathList;

static bool ends_with_ci(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf), i;
    if (a < b) return false;
    for (i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suf[i])) return false;
    return true;
}

static void list_add(PathList *l, const char *p)
{
    char *d;
    if (l->n >= 256) return;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        char **np = (char **)realloc(l->path, sizeof(char *) * (size_t)cap);
        if (!np) return;
        l->path = np;
        l->cap = cap;
    }
    d = (char *)malloc(strlen(p) + 1);
    if (!d) return;
    strcpy(d, p);
    l->path[l->n++] = d;
}

static void find_cards(const char *dir, int depth, PathList *l)
{
    char sub[2048];
#ifdef _WIN32
    wchar_t wpat[1100];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    snprintf(sub, sizeof sub, "%s\\*", dir);
    if (!MultiByteToWideChar(CP_UTF8, 0, sub, -1, wpat, 1100)) return;
    h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        snprintf(sub, sizeof sub, "%s\\%s", dir, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) find_cards(sub, depth - 1, l);
        } else if (ends_with_ci(name, "-0000621b.dds")) {
            list_add(l, sub);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        if (stat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) find_cards(sub, depth - 1, l);
        } else if (ends_with_ci(e->d_name, "-0000621b.dds")) {
            list_add(l, sub);
        }
    }
    closedir(d);
#endif
}

static uint8_t *read_all(const char *path, size_t *n)
{
    FILE *f;
    uint8_t *buf = NULL;
    long len;
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) return NULL;
    f = _wfopen(w, L"rb");
#else
    f = fopen(path, "rb");
#endif
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && len < (64L << 20) && fseek(f, 0, SEEK_SET) == 0) {
        buf = (uint8_t *)malloc((size_t)len);
        if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); buf = NULL; }
        *n = (size_t)len;
    }
    fclose(f);
    return buf;
}

/* PlayStation 2 alpha (0x80 = opaque) to 0..255. */
static void double_alpha(OtImage *im)
{
    int i;
    for (i = 0; i < im->w * im->h; i++) {
        uint32_t a = (im->px[i] >> 24) * 255 / 128;
        im->px[i] = (im->px[i] & 0xFFFFFFu) | ((a > 255 ? 255 : a) << 24);
    }
}

/* Mean RGB distance (0..255) over the texels opaque in both. */
static float distance64(const OtImage *a, const OtImage *b)
{
    double s = 0;
    int i, n = 0;
    for (i = 0; i < 64 * 64; i++) {
        uint32_t p = a->px[i], q = b->px[i];
        if ((p >> 24) < 200 || (q >> 24) < 200) continue;
        s += abs((int)((p >> 16) & 255) - (int)((q >> 16) & 255)) +
             abs((int)((p >> 8) & 255) - (int)((q >> 8) & 255)) +
             abs((int)(p & 255) - (int)(q & 255));
        n++;
    }
    return n > 64 * 64 / 4 ? (float)(s / (3.0 * n)) : 255.0f;
}

/* A pack card is taken for a track when its distance is under HD_MATCH_MAX
 * and clearly below every other file's for that track (HD_MATCH_RATIO). On
 * the pack tested, the right card scores 31..37 and the next best 55 or more:
 * the pack's cards are repainted, never identical to the disc's. */
#define HD_MATCH_MAX   48.0f
#define HD_MATCH_RATIO 0.80f

int ot_hdpack_cards(const char *pack_dir, const OtDiscAssets *a, OtImage hd[OT_TRACK_COUNT],
                    float score[OT_TRACK_COUNT])
{
    PathList l = { 0 };
    OtImage ref[OT_TRACK_COUNT];
    float *dist = NULL;
    int t, i, found = 0;
    memset(ref, 0, sizeof ref);
    for (t = 0; t < OT_TRACK_COUNT; t++) {
        memset(&hd[t], 0, sizeof hd[t]);
        if (score) score[t] = 255.0f;
    }
    if (!pack_dir || !pack_dir[0] || !a || !a->ok) return 0;
    find_cards(pack_dir, 4, &l);
    if (!l.n) return 0;
    dist = (float *)malloc(sizeof(float) * (size_t)l.n * OT_TRACK_COUNT);
    for (t = 0; t < OT_TRACK_COUNT; t++)
        if (!ot_image_resize(&a->card_tex[t], 64, 64, &ref[t])) goto done;
    if (!dist) goto done;
    for (i = 0; i < l.n; i++) {
        size_t n = 0;
        uint8_t *d = read_all(l.path[i], &n);
        OtImage small = { 0 }, s64 = { 0 };
        for (t = 0; t < OT_TRACK_COUNT; t++) dist[i * OT_TRACK_COUNT + t] = 255.0f;
        /* a small mip level is enough to recognise the card */
        if (d && ot_dds_decode(d, n, 64, &small)) {
            double_alpha(&small);
            if (ot_image_resize(&small, 64, 64, &s64))
                for (t = 0; t < OT_TRACK_COUNT; t++) dist[i * OT_TRACK_COUNT + t] = distance64(&s64, &ref[t]);
        }
        ot_image_free(&small);
        ot_image_free(&s64);
        free(d);
    }
    /* a track whose two best files are too close to call keeps the disc's card */
    for (t = 0; t < OT_TRACK_COUNT; t++) {
        float b1 = 255.0f, b2 = 255.0f;
        for (i = 0; i < l.n; i++) {
            float d = dist[i * OT_TRACK_COUNT + t];
            if (d < b1) { b2 = b1; b1 = d; } else if (d < b2) b2 = d;
        }
        if (b1 > HD_MATCH_RATIO * b2)
            for (i = 0; i < l.n; i++) dist[i * OT_TRACK_COUNT + t] = 255.0f;
    }
    /* best pairs first, each file and each track used once */
    for (;;) {
        int bi = -1, bt = -1;
        float best = HD_MATCH_MAX;
        for (i = 0; i < l.n; i++)
            for (t = 0; t < OT_TRACK_COUNT; t++)
                if (!hd[t].px && dist[i * OT_TRACK_COUNT + t] < best) { best = dist[i * OT_TRACK_COUNT + t]; bi = i; bt = t; }
        if (bi < 0) break;
        {
            size_t n = 0;
            uint8_t *d = read_all(l.path[bi], &n);
            if (d && ot_dds_decode(d, n, 0, &hd[bt])) {
                double_alpha(&hd[bt]);
                if (score) score[bt] = best;
                found++;
            }
            free(d);
            for (t = 0; t < OT_TRACK_COUNT; t++) dist[bi * OT_TRACK_COUNT + t] = 255.0f;   /* file used */
            if (!hd[bt].px)                                                                 /* unreadable: track stays free */
                continue;
        }
    }
done:
    for (t = 0; t < OT_TRACK_COUNT; t++) ot_image_free(&ref[t]);
    for (i = 0; i < l.n; i++) free(l.path[i]);
    free(l.path);
    free(dist);
    return found;
}
