/*
 * png_write.c -- a minimal PNG writer for the launcher's host tools
 * (RGBA 8-bit, deflate "stored" blocks: larger files, no dependency).
 */
#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    uint32_t n, c, k;
    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t c, const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 255] ^ (c >> 8);
    return c;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8];
    uint32_t c;
    put32(h, len);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    c = crc_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
    c = crc_update(c, data, len) ^ 0xFFFFFFFFu;
    put32(h, c);
    fwrite(h, 1, 4, f);
}

bool png_write(const char *path, const OtImage *im)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    size_t raw_len = (size_t)im->h * (1 + (size_t)im->w * 4), nblocks = (raw_len + 65534) / 65535, i, o = 0;
    uint8_t *raw, *z, ihdr[13];
    uint32_t a = 1, b = 0;
    FILE *f;
    if (!im->px) return false;
    crc_init();
    raw = (uint8_t *)malloc(raw_len);
    z = (uint8_t *)malloc(raw_len + nblocks * 5 + 6);
    if (!raw || !z) { free(raw); free(z); return false; }
    for (i = 0; i < (size_t)im->h; i++) {
        uint8_t *row = raw + i * (1 + (size_t)im->w * 4);
        int x;
        row[0] = 0;
        for (x = 0; x < im->w; x++) {
            uint32_t p = im->px[i * im->w + x];
            row[1 + x * 4] = (uint8_t)(p >> 16);
            row[2 + x * 4] = (uint8_t)(p >> 8);
            row[3 + x * 4] = (uint8_t)p;
            row[4 + x * 4] = (uint8_t)(p >> 24);
        }
    }
    z[o++] = 0x78; z[o++] = 0x01;
    for (i = 0; i < raw_len; i += 65535) {
        size_t n = raw_len - i < 65535 ? raw_len - i : 65535;
        z[o++] = (uint8_t)(i + n >= raw_len);
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + i, n);
        o += n;
    }
    for (i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    put32(z + o, (b << 16) | a);
    o += 4;
    put32(ihdr, (uint32_t)im->w);
    put32(ihdr + 4, (uint32_t)im->h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    f = fopen(path, "wb");
    if (f) {
        fwrite(sig, 1, 8, f);
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, (uint32_t)o);
        chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw);
    free(z);
    return f != NULL;
}

bool wav_write(const char *path, const int16_t *pcm, int frames, int channels, int rate)
{
    uint8_t h[44];
    uint32_t data = (uint32_t)frames * channels * 2;
    FILE *f;
    int i;
    memcpy(h, "RIFF", 4);
    for (i = 0; i < 4; i++) h[4 + i] = (uint8_t)((36 + data) >> (8 * i));
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0;
    h[22] = (uint8_t)channels; h[23] = 0;
    for (i = 0; i < 4; i++) h[24 + i] = (uint8_t)((uint32_t)rate >> (8 * i));
    for (i = 0; i < 4; i++) h[28 + i] = (uint8_t)(((uint32_t)rate * channels * 2) >> (8 * i));
    h[32] = (uint8_t)(channels * 2); h[33] = 0;
    h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    for (i = 0; i < 4; i++) h[40 + i] = (uint8_t)(data >> (8 * i));
    f = fopen(path, "wb");
    if (!f) return false;
    fwrite(h, 1, 44, f);
    fwrite(pcm, 2, (size_t)frames * channels, f);     /* little-endian host */
    fclose(f);
    return true;
}
