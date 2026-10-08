/*
 * ot_assets_preview -- host test of the launcher's disc assets and themes.
 *
 *   ot_assets_preview <disc image> <output folder> [texture pack folder]
 *
 * Reads everything the launcher takes from the disc image (and the HD track
 * cards from the pack, if given), then writes into the output folder: the
 * cards, emblems and badges, the title font, the menu sounds and music
 * (WAV), and two sheets of the ten track themes plus the no-disc look, with
 * the tuned and the measured palettes. report.txt sums up the disc check,
 * timings, HD matches and the colour differences. Everything written is
 * taken from the player's own files: keep the output out of version control.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ot_disc.h"
#include "ot_image.h"
#include "ot_theme.h"
#include "png_write.h"

#define TW 640
#define TH 400

static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

static void out_path(char *buf, size_t n, const char *dir, const char *name)
{
    snprintf(buf, n, "%s/%s", dir, name);
}

static void save(const char *dir, const char *name, const OtImage *im)
{
    char p[1024];
    out_path(p, sizeof p, dir, name);
    if (!png_write(p, im)) fprintf(stderr, "cannot write %s\n", p);
}

static void fill_rect(OtImage *im, int x0, int y0, int w, int h, uint32_t c)
{
    int x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            if (x >= 0 && y >= 0 && x < im->w && y < im->h) im->px[(size_t)y * im->w + x] = c;
}

static int draw_text(OtImage *im, const OtFont *f, int x, int y, const char *s, uint32_t col)
{
    for (; *s; s++) {
        const OtGlyph *g;
        int gx, gy;
        if ((unsigned char)*s >= 128) continue;
        g = &f->g[(unsigned char)*s];
        if (!g->adv) continue;
        for (gy = 0; gy < g->h; gy++)
            for (gx = 0; gx < g->w; gx++) {
                int dx = x + g->ox + gx, dy = y + g->oy + gy;
                uint32_t cov = f->cov[(size_t)(g->y + gy) * f->w + g->x + gx];
                OtImage px;
                uint32_t one;
                if (!cov || dx < 0 || dy < 0 || dx >= im->w || dy >= im->h) continue;
                one = (col & 0xFFFFFFu) | (cov << 24);
                px.w = px.h = 1;
                px.px = &one;
                ot_image_blend(im, &px, dx, dy);
            }
        x += g->adv;
    }
    return x;
}

/* A rounded pill filled with a 135-degree gradient. */
static void pill(OtImage *im, int x, int y, int w, int h, uint32_t c0, uint32_t c1)
{
    OtImage p;
    uint8_t *m;
    int i, j;
    if (!ot_image_alloc(&p, w, h)) return;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            float t = (float)(i + j) / (float)(w + h - 2);
            uint32_t out = 0xFF000000u;
            int c;
            for (c = 0; c < 24; c += 8) {
                float v = ((c0 >> c) & 255) * (1 - t) + ((c1 >> c) & 255) * t;
                out |= (uint32_t)(v + 0.5f) << c;
            }
            p.px[(size_t)j * w + i] = out;
        }
    m = ot_mask_round_rect(w, h, 0, 0, (float)w, (float)h, h * 0.22f);
    if (m) ot_image_apply_mask(&p, m);
    free(m);
    ot_image_blend(im, &p, x, y);
    ot_image_free(&p);
}

static void scaled_blend(OtImage *dst, const OtImage *src, int x, int y, int w)
{
    OtImage s;
    if (!src->px || !ot_image_resize(src, w, src->h * w / src->w, &s)) return;
    ot_image_blend(dst, &s, x, y);
    ot_image_free(&s);
}

/* One home-screen-like tile: background, emblem (or OT badge), title, PLAY,
 * the track card badge, the palette. */
static void tile(OtImage *sheet, int tx, int ty, const OtTheme *t, const OtDiscAssets *a, int track,
                 const OtImage *badge, const OtImage *ot)
{
    OtImage bg, side;
    int i, x;
    if (!ot_theme_background(t, TW, TH, &bg)) return;
    if (ot_image_alloc(&side, 38, TH)) {
        for (i = 0; i < side.w * side.h; i++) side.px[i] = 0x99040A18u;
        ot_image_blend(&bg, &side, 0, 0);
        ot_image_free(&side);
    }
    if (track >= 0) scaled_blend(&bg, &a->emblem[track], 6, 10, 26);
    else scaled_blend(&bg, ot, 6, 10, 26);
    x = draw_text(&bg, &a->title, 68, 70, "OPEN", 0xFFFFFFFFu);
    draw_text(&bg, &a->title, x + 4, 70, "TRICKY", t->from_disc ? t->acc : t->acc_light);
    draw_text(&bg, &a->title, 68, 100, track >= 0 ? ot_track_name(track) : "NO DISC IMAGE YET", 0xFFB8C4DAu);
    if (t->from_disc || track >= 0) {
        pill(&bg, 66, 300, 150, 44, t->acc_light, t->acc_dark);
        x = 66 + (150 - ot_font_text_width(&a->title, "PLAY")) / 2;
        draw_text(&bg, &a->title, x, 312, "PLAY", t->play_text);
    } else {
        pill(&bg, 66, 300, 150, 44, 0xFF1A2440u, 0xFF141C33u);
        x = 66 + (150 - ot_font_text_width(&a->title, "PLAY")) / 2;
        draw_text(&bg, &a->title, x, 312, "PLAY", 0xFF5D6A85u);
    }
    if (badge && badge->px) scaled_blend(&bg, badge, 350, 50, 260);
    for (i = 0; i < 8; i++) {
        uint32_t c = i == 0 ? t->acc : i == 1 ? t->acc_dark : i == 2 ? t->acc_light : t->grad[i - 3];
        fill_rect(&bg, 350 + i * 32, 360, 28, 20, c | 0xFF000000u);
    }
    ot_image_blend(sheet, &bg, tx, ty);
    ot_image_free(&bg);
}

static float rgb_dist(uint32_t a, uint32_t b)
{
    int c;
    float s = 0;
    for (c = 0; c < 24; c += 8) {
        float d = (float)((a >> c) & 255) - (float)((b >> c) & 255);
        s += d * d;
    }
    return sqrtf(s / 3);
}

int main(int argc, char **argv)
{
    OtDiscAssets a;
    OtDiscCheck chk;
    OtImage hd[OT_TRACK_COUNT], badge[OT_TRACK_COUNT], ot, sheet;
    float score[OT_TRACK_COUNT];
    const char *out;
    char p[1024];
    FILE *rep;
    double t0, t1;
    int t, i, nhd = 0, pass;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <disc image> <output folder> [texture pack folder]\n", argv[0]);
        return 2;
    }
    out = argv[2];
    out_path(p, sizeof p, out, "report.txt");
    rep = fopen(p, "w");
    if (!rep) { fprintf(stderr, "cannot write %s\n", p); return 2; }

    t0 = now_ms();
    chk = ot_disc_check(argv[1]);
    t1 = now_ms();
    fprintf(rep, "disc check: status %d (0 OK, 1 no path, 2 not found, 3 wrong), xbox %d, title id %08X, region %08X, title \"%s\" (%.1f ms)\n",
            chk.status, chk.xbox, chk.title_id, chk.region, chk.title, t1 - t0);
    {
        OtDiscCheck c1 = ot_disc_check(""), c2 = ot_disc_check("Z:/no/such/file.iso"), c3 = ot_disc_check(argv[0]);
        fprintf(rep, "states: empty path -> %d, missing file -> %d, not a disc image (this tool) -> %d\n",
                c1.status, c2.status, c3.status);
    }

    t0 = now_ms();
    if (!ot_disc_load(argv[1], &a)) {
        fprintf(rep, "ot_disc_load FAILED\n");
        fclose(rep);
        return 1;
    }
    t1 = now_ms();
    fprintf(rep, "ot_disc_load: ok, %.1f ms; sounds %d, music %d (%d frames = %.2f s)\n", t1 - t0,
            a.sounds_ok, a.music_ok, a.music.frames, a.music.frames / 44100.0);

    memset(hd, 0, sizeof hd);
    if (argc > 3) {
        t0 = now_ms();
        nhd = ot_hdpack_cards(argv[3], &a, hd, score);
        t1 = now_ms();
        fprintf(rep, "hd pack: %d of %d cards, %.1f ms\n", nhd, OT_TRACK_COUNT, t1 - t0);
        for (t = 0; t < OT_TRACK_COUNT; t++)
            fprintf(rep, "  %s: %s, distance %.2f\n", ot_track_id(t), hd[t].px ? "found" : "-", score[t]);
    }

    for (t = 0; t < OT_TRACK_COUNT; t++) {
        char name[64];
        OtImage disc_badge = { 0 };
        snprintf(name, sizeof name, "card_%s.png", ot_track_id(t));       save(out, name, &a.card[t]);
        snprintf(name, sizeof name, "emblem_%s.png", ot_track_id(t));     save(out, name, &a.emblem[t]);
        if (ot_badge_from_disc(&a, t, 1040, &disc_badge)) {
            snprintf(name, sizeof name, "badge_disc_%s.png", ot_track_id(t));
            save(out, name, &disc_badge);
        }
        if (hd[t].px) {
            /* the card as the launcher ships it (ui/cards/<track>.png): the
             * pack's picture in the card's 16:5 shape, alpha already doubled */
            OtImage shaped = { 0 };
            if (ot_image_resize(&hd[t], OT_CARD_HD_W, OT_CARD_HD_H, &shaped)) {
                snprintf(name, sizeof name, "cardhd_%s.png", ot_track_id(t));
                save(out, name, &shaped);
            }
            ot_image_free(&shaped);
        }
        if (hd[t].px && ot_badge_make(&hd[t], 0, 1040, &badge[t])) {
            snprintf(name, sizeof name, "badge_hd_%s.png", ot_track_id(t));
            save(out, name, &badge[t]);
            ot_image_free(&disc_badge);
        } else {
            badge[t] = disc_badge;
        }
        fprintf(rep, "  %s badge rim %06X\n", ot_track_id(t), ot_badge_rim_colour(&badge[t]) & 0xFFFFFFu);
    }
    {
        OtImage font;
        if (ot_image_alloc(&font, a.title.w, a.title.h)) {
            for (i = 0; i < font.w * font.h; i++) font.px[i] = ((uint32_t)a.title.cov[i] << 24) | 0xFFFFFFu;
            save(out, "font_title.png", &font);
            ot_image_free(&font);
        }
    }
    for (i = 0; i < OT_SND_COUNT && a.sounds_ok; i++) {
        static const char *const nm[OT_SND_COUNT] = { "move", "change", "select", "back" };
        snprintf(p, sizeof p, "%s/sound_%s.wav", out, nm[i]);
        wav_write(p, a.sound[i].pcm, a.sound[i].frames, a.sound[i].channels, a.sound[i].rate);
    }
    if (a.music_ok) {
        snprintf(p, sizeof p, "%s/music_title_loop.wav", out);
        wav_write(p, a.music.pcm, a.music.frames, 2, 44100);
    }

    ot_badge_ot(92, &ot);
    save(out, "badge_ot.png", &ot);
    for (pass = 0; pass < 2; pass++) {
        if (!ot_image_alloc(&sheet, 4 * TW, 3 * TH)) break;
        for (t = 0; t <= OT_TRACK_COUNT; t++) {
            OtTheme th;
            int tr = t < OT_TRACK_COUNT ? t : -1;
            if (tr >= 0) ot_theme_for_track(&a, tr, pass == 0, &th);
            else ot_theme_default(&th);
            tile(&sheet, (t % 4) * TW, (t / 4) * TH, &th, &a, tr, tr >= 0 ? &badge[tr] : NULL, &ot);
        }
        save(out, pass == 0 ? "themes_tuned.png" : "themes_measured.png", &sheet);
        ot_image_free(&sheet);
    }
    {
        OtTheme bg;
        OtImage full;
        ot_theme_default(&bg);
        if (ot_theme_background(&bg, 1280, 800, &full)) { save(out, "background_nodisc.png", &full); ot_image_free(&full); }
        ot_theme_for_track(&a, OT_TRACK_SNOW, true, &bg);
        if (ot_theme_background(&bg, 1280, 800, &full)) { save(out, "background_snow.png", &full); ot_image_free(&full); }
    }

    fprintf(rep, "\nmeasured vs tuned (RMS distance per colour, 0..255):\n");
    for (t = 0; t < OT_TRACK_COUNT; t++) {
        OtTheme tu, me;
        ot_theme_for_track(&a, t, true, &tu);
        ot_theme_measure(&a, t, &me);
        fprintf(rep, "  %s acc %06X/%06X (%.0f) dark %06X/%06X (%.0f) light %06X/%06X (%.0f) text %06X/%06X | grad",
                ot_track_id(t), me.acc & 0xFFFFFF, tu.acc & 0xFFFFFF, rgb_dist(me.acc, tu.acc),
                me.acc_dark & 0xFFFFFF, tu.acc_dark & 0xFFFFFF, rgb_dist(me.acc_dark, tu.acc_dark),
                me.acc_light & 0xFFFFFF, tu.acc_light & 0xFFFFFF, rgb_dist(me.acc_light, tu.acc_light),
                me.play_text & 0xFFFFFF, tu.play_text & 0xFFFFFF);
        for (i = 0; i < 5; i++)
            fprintf(rep, " %06X/%06X(%.0f)", me.grad[i] & 0xFFFFFF, tu.grad[i] & 0xFFFFFF, rgb_dist(me.grad[i], tu.grad[i]));
        fprintf(rep, "\n");
    }

    for (t = 0; t < OT_TRACK_COUNT; t++) { ot_image_free(&hd[t]); ot_image_free(&badge[t]); }
    ot_image_free(&ot);
    ot_disc_free(&a);
    fclose(rep);
    printf("done: %s/report.txt\n", out);
    return 0;
}
