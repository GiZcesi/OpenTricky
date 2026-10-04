/* recomp_mmx.h -- MMX packed-integer helpers for the recompiled code.
 *
 * Included from recomp_types.h immediately after the pack/saturate helpers,
 * which these build on. The mm registers are plain uint64_t locals in the
 * generated code, so every one of these is ordinary integer arithmetic.
 *
 * These exist because the lifter used to emit unimplemented MMX opcodes as
 * bare `/ * TODO: ... * /` comments. That is silent data loss, not a missing
 * nicety: the video's YUV-to-RGB converters (sub_00149450 / 500 / 5E0 / 670)
 * are written entirely in MMX, so a dropped opcode does not slow the picture
 * down, it corrupts it. With paddw, packuswb, paddusb, pmaddwd and the packed
 * shifts all dropped, only the pand/por survived -- luma reached the screen
 * roughly intact while chroma was never added at all, which is what produced
 * the flat magenta and grey slabs over a still-recognisable logo.
 */
#ifndef RECOMP_MMX_H
#define RECOMP_MMX_H

/* ---- saturating packed add / subtract ---------------------------------- */

/* The converters apply a per-channel bias to an already-packed pixel with
 * paddusb, where the saturation is the whole point: without it a bright
 * channel wraps around to black. */
static inline uint64_t mmx_paddusb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++) {
        unsigned s = (unsigned)((a >> (i*8)) & 0xFF)
                   + (unsigned)((b >> (i*8)) & 0xFF);
        r |= (uint64_t)(s > 255u ? 255u : s) << (i*8);
    }
    return r;
}
static inline uint64_t mmx_paddusw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++) {
        unsigned s = (unsigned)((a >> (i*16)) & 0xFFFF)
                   + (unsigned)((b >> (i*16)) & 0xFFFF);
        r |= (uint64_t)(s > 65535u ? 65535u : s) << (i*16);
    }
    return r;
}
static inline uint64_t mmx_psubusb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++) {
        int s = (int)((a >> (i*8)) & 0xFF) - (int)((b >> (i*8)) & 0xFF);
        r |= (uint64_t)(s < 0 ? 0 : s) << (i*8);
    }
    return r;
}
static inline uint64_t mmx_psubusw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++) {
        int s = (int)((a >> (i*16)) & 0xFFFF) - (int)((b >> (i*16)) & 0xFFFF);
        r |= (uint64_t)(s < 0 ? 0 : s) << (i*16);
    }
    return r;
}
static inline uint64_t mmx_paddsb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++) {
        int s = (int)(int8_t)((a >> (i*8)) & 0xFF)
              + (int)(int8_t)((b >> (i*8)) & 0xFF);
        s = s < -128 ? -128 : (s > 127 ? 127 : s);
        r |= (uint64_t)(uint8_t)(int8_t)s << (i*8);
    }
    return r;
}
static inline uint64_t mmx_psubsb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++) {
        int s = (int)(int8_t)((a >> (i*8)) & 0xFF)
              - (int)(int8_t)((b >> (i*8)) & 0xFF);
        s = s < -128 ? -128 : (s > 127 ? 127 : s);
        r |= (uint64_t)(uint8_t)(int8_t)s << (i*8);
    }
    return r;
}
static inline uint64_t mmx_paddsw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++) {
        int64_t s = (int64_t)(int16_t)((a >> (i*16)) & 0xFFFF)
                  + (int64_t)(int16_t)((b >> (i*16)) & 0xFFFF);
        r |= (uint64_t)(uint16_t)mmx_satsw(s) << (i*16);
    }
    return r;
}
static inline uint64_t mmx_psubsw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++) {
        int64_t s = (int64_t)(int16_t)((a >> (i*16)) & 0xFFFF)
                  - (int64_t)(int16_t)((b >> (i*16)) & 0xFFFF);
        r |= (uint64_t)(uint16_t)mmx_satsw(s) << (i*16);
    }
    return r;
}

/* ---- packed multiply --------------------------------------------------- */

/* pmaddwd: four signed 16x16 products, summed in pairs into two 32-bit lanes.
 * The converters use it against a constant coefficient vector to fold an
 * RGB888 triple down onto the bit positions of RGB565 in one instruction. */
static inline uint64_t mmx_pmaddwd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; i++) {
        int32_t lo = (int32_t)(int16_t)((a >> (i*32))      & 0xFFFF)
                   * (int32_t)(int16_t)((b >> (i*32))      & 0xFFFF);
        int32_t hi = (int32_t)(int16_t)((a >> (i*32 + 16)) & 0xFFFF)
                   * (int32_t)(int16_t)((b >> (i*32 + 16)) & 0xFFFF);
        r |= (uint64_t)(uint32_t)(lo + hi) << (i*32);
    }
    return r;
}
static inline uint64_t mmx_pmullw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++) {
        int32_t p = (int32_t)(int16_t)((a >> (i*16)) & 0xFFFF)
                  * (int32_t)(int16_t)((b >> (i*16)) & 0xFFFF);
        r |= (uint64_t)(uint16_t)(p & 0xFFFF) << (i*16);
    }
    return r;
}
static inline uint64_t mmx_pmulhw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++) {
        int32_t p = (int32_t)(int16_t)((a >> (i*16)) & 0xFFFF)
                  * (int32_t)(int16_t)((b >> (i*16)) & 0xFFFF);
        r |= (uint64_t)(uint16_t)((p >> 16) & 0xFFFF) << (i*16);
    }
    return r;
}
static inline uint64_t mmx_pmulhuw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++) {
        uint32_t p = (uint32_t)((a >> (i*16)) & 0xFFFF)
                   * (uint32_t)((b >> (i*16)) & 0xFFFF);
        r |= (uint64_t)(uint16_t)((p >> 16) & 0xFFFF) << (i*16);
    }
    return r;
}

/* ---- packed shifts ------------------------------------------------------
 * The count is NOT reduced modulo the lane width. x86 defines a count at or
 * above the width as producing zero in every lane, and the count operand is a
 * full 64-bit value, so a large count must not alias back to a small one --
 * and in C the shift itself would be undefined. Hence the explicit tests.
 * Arithmetic right shifts saturate to all-sign instead of to zero. */
static inline uint64_t mmx_psrlw(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    if (c > 15) return 0;
    for (i = 0; i < 4; i++)
        r |= (uint64_t)(uint16_t)(((a >> (i*16)) & 0xFFFF) >> c) << (i*16);
    return r;
}
static inline uint64_t mmx_psrld(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    if (c > 31) return 0;
    for (i = 0; i < 2; i++)
        r |= (uint64_t)(uint32_t)(((a >> (i*32)) & 0xFFFFFFFFu) >> c) << (i*32);
    return r;
}
static inline uint64_t mmx_psrlq(uint64_t a, uint64_t c)
{
    return c > 63 ? 0 : (a >> c);
}
static inline uint64_t mmx_psllw(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    if (c > 15) return 0;
    for (i = 0; i < 4; i++)
        r |= (uint64_t)(uint16_t)(((((a >> (i*16)) & 0xFFFF) << c)) & 0xFFFF) << (i*16);
    return r;
}
static inline uint64_t mmx_pslld(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    if (c > 31) return 0;
    for (i = 0; i < 2; i++)
        r |= (uint64_t)(uint32_t)((((a >> (i*32)) & 0xFFFFFFFFu) << c) & 0xFFFFFFFFu) << (i*32);
    return r;
}
static inline uint64_t mmx_psllq(uint64_t a, uint64_t c)
{
    return c > 63 ? 0 : (a << c);
}
static inline uint64_t mmx_psraw(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    unsigned n = (unsigned)(c > 15 ? 15 : c);
    for (i = 0; i < 4; i++)
        r |= (uint64_t)(uint16_t)((int16_t)((a >> (i*16)) & 0xFFFF) >> n) << (i*16);
    return r;
}
static inline uint64_t mmx_psrad(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    unsigned n = (unsigned)(c > 31 ? 31 : c);
    for (i = 0; i < 2; i++)
        r |= (uint64_t)(uint32_t)((int32_t)((a >> (i*32)) & 0xFFFFFFFFu) >> n) << (i*32);
    return r;
}

/* ---- unpack / interleave / average --------------------------------------
 * Motion compensation expands bytes to words through the unpacks, and
 * averages two reference blocks with pavgb for half-pel vectors. */
static inline uint64_t mmx_punpcklbw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++)
        r |= (((a >> (i*8)) & 0xFFu) | (((b >> (i*8)) & 0xFFu) << 8)) << (i*16);
    return r;
}
static inline uint64_t mmx_punpckhbw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++)
        r |= (((a >> (32 + i*8)) & 0xFFu)
           | (((b >> (32 + i*8)) & 0xFFu) << 8)) << (i*16);
    return r;
}
static inline uint64_t mmx_punpcklwd(uint64_t a, uint64_t b)
{
    return (a & 0xFFFFu) | ((b & 0xFFFFu) << 16)
         | (((a >> 16) & 0xFFFFu) << 32) | (((b >> 16) & 0xFFFFu) << 48);
}
static inline uint64_t mmx_punpckhwd(uint64_t a, uint64_t b)
{
    return ((a >> 32) & 0xFFFFu) | (((b >> 32) & 0xFFFFu) << 16)
         | (((a >> 48) & 0xFFFFu) << 32) | (((b >> 48) & 0xFFFFu) << 48);
}
static inline uint64_t mmx_punpckldq(uint64_t a, uint64_t b)
{
    return (a & 0xFFFFFFFFu) | ((b & 0xFFFFFFFFu) << 32);
}
static inline uint64_t mmx_punpckhdq(uint64_t a, uint64_t b)
{
    return ((a >> 32) & 0xFFFFFFFFu) | (((b >> 32) & 0xFFFFFFFFu) << 32);
}
static inline uint64_t mmx_pavgb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++)
        r |= (uint64_t)(((((a >> (i*8)) & 0xFFu)
                        + ((b >> (i*8)) & 0xFFu)) + 1u) >> 1) << (i*8);
    return r;
}
static inline uint64_t mmx_pavgw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++)
        r |= (uint64_t)(((((a >> (i*16)) & 0xFFFFu)
                        + ((b >> (i*16)) & 0xFFFFu)) + 1u) >> 1) << (i*16);
    return r;
}

/* ---- compare / min / max ------------------------------------------------ */
static inline uint64_t mmx_pcmpeqb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++)
        if (((a >> (i*8)) & 0xFFu) == ((b >> (i*8)) & 0xFFu))
            r |= (uint64_t)0xFFu << (i*8);
    return r;
}
static inline uint64_t mmx_pcmpeqw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++)
        if (((a >> (i*16)) & 0xFFFFu) == ((b >> (i*16)) & 0xFFFFu))
            r |= (uint64_t)0xFFFFu << (i*16);
    return r;
}
static inline uint64_t mmx_pcmpeqd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; i++)
        if (((a >> (i*32)) & 0xFFFFFFFFu) == ((b >> (i*32)) & 0xFFFFFFFFu))
            r |= (uint64_t)0xFFFFFFFFu << (i*32);
    return r;
}
static inline uint64_t mmx_pcmpgtb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++)
        if ((int8_t)((a >> (i*8)) & 0xFFu) > (int8_t)((b >> (i*8)) & 0xFFu))
            r |= (uint64_t)0xFFu << (i*8);
    return r;
}
static inline uint64_t mmx_pcmpgtw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++)
        if ((int16_t)((a >> (i*16)) & 0xFFFFu) > (int16_t)((b >> (i*16)) & 0xFFFFu))
            r |= (uint64_t)0xFFFFu << (i*16);
    return r;
}
static inline uint64_t mmx_pcmpgtd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; i++)
        if ((int32_t)((a >> (i*32)) & 0xFFFFFFFFu) > (int32_t)((b >> (i*32)) & 0xFFFFFFFFu))
            r |= (uint64_t)0xFFFFFFFFu << (i*32);
    return r;
}
static inline uint64_t mmx_pminub(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++) {
        unsigned x = (unsigned)((a >> (i*8)) & 0xFFu);
        unsigned y = (unsigned)((b >> (i*8)) & 0xFFu);
        r |= (uint64_t)(x < y ? x : y) << (i*8);
    }
    return r;
}
static inline uint64_t mmx_pmaxub(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++) {
        unsigned x = (unsigned)((a >> (i*8)) & 0xFFu);
        unsigned y = (unsigned)((b >> (i*8)) & 0xFFu);
        r |= (uint64_t)(x > y ? x : y) << (i*8);
    }
    return r;
}
static inline uint64_t mmx_pminsw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++) {
        int16_t x = (int16_t)((a >> (i*16)) & 0xFFFFu);
        int16_t y = (int16_t)((b >> (i*16)) & 0xFFFFu);
        r |= (uint64_t)(uint16_t)(x < y ? x : y) << (i*16);
    }
    return r;
}
static inline uint64_t mmx_pmaxsw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++) {
        int16_t x = (int16_t)((a >> (i*16)) & 0xFFFFu);
        int16_t y = (int16_t)((b >> (i*16)) & 0xFFFFu);
        r |= (uint64_t)(uint16_t)(x > y ? x : y) << (i*16);
    }
    return r;
}
/* Sum of absolute differences, into the low word. Used by motion search. */
static inline uint64_t mmx_psadbw(uint64_t a, uint64_t b)
{
    unsigned s = 0; int i;
    for (i = 0; i < 8; i++) {
        int d = (int)((a >> (i*8)) & 0xFFu) - (int)((b >> (i*8)) & 0xFFu);
        s += (unsigned)(d < 0 ? -d : d);
    }
    return (uint64_t)s;
}

#endif /* RECOMP_MMX_H */
