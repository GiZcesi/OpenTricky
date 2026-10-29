/*
 * terrain_seams -- texture seams between terrain patches. See terrain_seams.h.
 *
 * The title's patch (record of 0x2D0 bytes, array at obj+0x9CC0, count at
 * obj+0x2C), as sub_000F8BA0 / F8E20 / F8A30 lay it into a cache slot of 64
 * vertices of 0x30 bytes, vertex (i, j) at 8j + i, a = i/7, b = j/7:
 *   +0x00 position  P(a, b) = sum_r sum_k B_r(b) B_k(a) C[r][k],
 *                   B = (t^3, t^2, t, 1), C[r][k] = vec4 at rec+0x50+0x40r+0x10k
 *   +0x0C texture u, +0x1C texture v   bilinear over the patch, corners
 *                   (a, b) = (0,0) rec+0x10, (0,1) rec+0x20, (1,0) rec+0x30,
 *                   (1,1) rec+0x40 (u, v the first two floats)
 *   +0x2C lightmap u, v (short2)       a tile of the atlas, rect at rec+0x00
 * Textures: rec+0x1C0 int16 snow (t0), +0x1C2 normal map (t2, same
 * coordinates), +0x1C4 lightmap atlas (t1). Drawn as one 112-index snake
 * strip whose variants remap the index of an edge onto the nodes of a coarser
 * neighbour; the filter keeps the title's index list as it is.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "recomp/recomp_types.h"
#include "nv2a/nv2a_pgraph_d3d11.h"
#include "terrain_seams.h"

#define VSZ         0x30u
#define SLOT_BYTES  0xC00u
#define REC_SIZE    0x2D0u
#define NKEY        8192u
#define APP_GLOBAL  0x001E3C7Cu
#define GEO_TOL     5.0         /* two edges are one curve when their midpoints are this close (units) */
#define J_HIGH      0.25        /* relative difference of the texture-per-world matrices across the edge */
#define LM_STEP     20.0        /* lightmap step across the edge, mean |RGB| / 255 */

enum { SEAM_NONE, SEAM_OK, SEAM_STRETCH, SEAM_OFFSET, SEAM_ROTATED, SEAM_TEXTURE, SEAM_KINDS };
static const char *const k_kind[SEAM_KINDS] = {
    "unshared", "continuous", "stretched", "2-texel-offset", "rotated/mirrored", "other-texture"
};

int g_terrain_seams;
static int s_b0, s_b1, s_rows = 2, s_debug, s_log;
static int s_blend, s_blend_rows = 1;   /* XBOX_TERRAIN_BLEND, XBOX_TERRAIN_BLEND_ROWS */
static double s_width;                   /* XBOX_TERRAIN_BLEND_WIDTH: band in metres (0 = in rows) */
static int s_blend_tangent = 1;          /* XBOX_TERRAIN_BLEND_TANGENT=0: the copies keep the patch's tangents */

/* a heap address, cached or through the uncached alias (0x80000000 | a); the title heap ends at 0x08C00000 */
static int valid_va(uint32_t a) { a &= 0x7FFFFFFFu; return a >= 0x1000u && a < 0x08C00000u; }

/* ---- patch geometry and texture placement (double) ---- */

typedef struct { double x, y, z; } v3;

static double dot3(const v3 *a, const v3 *b) { return a->x * b->x + a->y * b->y + a->z * b->z; }
static v3 cross3(const v3 *a, const v3 *b)
{
    v3 o;
    o.x = a->y * b->z - a->z * b->y; o.y = a->z * b->x - a->x * b->z; o.z = a->x * b->y - a->y * b->x;
    return o;
}
static v3 unit3(v3 a)
{
    double l = sqrt(dot3(&a, &a));
    if (l > 0.0) { a.x /= l; a.y /= l; a.z /= l; }
    return a;
}
static v3 sub3(const v3 *a, const v3 *b) { v3 o; o.x = a->x - b->x; o.y = a->y - b->y; o.z = a->z - b->z; return o; }
static float rdf(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
static int16_t rds(const uint8_t *p) { int16_t s; memcpy(&s, p, 2); return s; }

/* P(a, b) and, when asked, dP/da and dP/db */
static void rec_eval(const uint8_t *rec, double a, double b, v3 *p, v3 *pa, v3 *pb)
{
    double ba[4], bb[4], da[4], db[4];
    int r, k;
    ba[0] = a * a * a; ba[1] = a * a; ba[2] = a; ba[3] = 1.0;
    bb[0] = b * b * b; bb[1] = b * b; bb[2] = b; bb[3] = 1.0;
    da[0] = 3 * a * a; da[1] = 2 * a; da[2] = 1.0; da[3] = 0.0;
    db[0] = 3 * b * b; db[1] = 2 * b; db[2] = 1.0; db[3] = 0.0;
    memset(p, 0, sizeof *p);
    if (pa) memset(pa, 0, sizeof *pa);
    if (pb) memset(pb, 0, sizeof *pb);
    for (r = 0; r < 4; r++)
        for (k = 0; k < 4; k++) {
            const uint8_t *c = rec + 0x50 + 0x40 * r + 0x10 * k;
            double x = rdf(c), y = rdf(c + 4), z = rdf(c + 8), w = bb[r] * ba[k];
            p->x += w * x; p->y += w * y; p->z += w * z;
            if (pa) { w = bb[r] * da[k]; pa->x += w * x; pa->y += w * y; pa->z += w * z; }
            if (pb) { w = db[r] * ba[k]; pb->x += w * x; pb->y += w * y; pb->z += w * z; }
        }
}

/* edge e at parameter t: 0 (t,0), 1 (1,t), 2 (t,1), 3 (0,t) */
static void edge_ab(int e, double t, double *a, double *b)
{
    switch (e) {
    case 0:  *a = t;   *b = 0.0; break;
    case 1:  *a = 1.0; *b = t;   break;
    case 2:  *a = t;   *b = 1.0; break;
    default: *a = 0.0; *b = t;   break;
    }
}

/* node at step s along edge e, r rows inwards */
static int edge_node(int e, int s, int r)
{
    switch (e) {
    case 0:  return 8 * r + s;
    case 1:  return 8 * s + (7 - r);
    case 2:  return 8 * (7 - r) + s;
    default: return 8 * s + r;
    }
}

typedef double corners_t[4][2];         /* (0,0) (0,1) (1,0) (1,1) */

static void rec_corners(const uint8_t *rec, corners_t c)
{
    int k;
    for (k = 0; k < 4; k++) { c[k][0] = rdf(rec + 0x10 + 0x10 * k); c[k][1] = rdf(rec + 0x14 + 0x10 * k); }
}

static void uv_at(corners_t c, double a, double b, double o[2])
{
    int q;
    for (q = 0; q < 2; q++)
        o[q] = (1 - a) * (1 - b) * c[0][q] + (1 - a) * b * c[1][q] + a * (1 - b) * c[2][q] + a * b * c[3][q];
}

static void duv_at(corners_t c, double a, double b, double da[2], double db[2])
{
    int q;
    for (q = 0; q < 2; q++) {
        da[q] = -(1 - b) * c[0][q] - b * c[1][q] + (1 - b) * c[2][q] + b * c[3][q];
        db[q] = -(1 - a) * c[0][q] + (1 - a) * c[1][q] - a * c[2][q] + a * c[3][q];
    }
}

/* b0: corners inset by one texel (fraction 0.008 / 0.992) moved to the whole
 * number; a patch with any other fraction is left as it is. */
static int b0_deltas(corners_t c, corners_t d)
{
    int k, q, any = 0;
    for (k = 0; k < 4; k++)
        for (q = 0; q < 2; q++) {
            double x = c[k][q], f = x - floor(x);
            if (f < 1e-4 || f > 1.0 - 1e-4) d[k][q] = 0.0;
            else if (f >= 0.004 && f <= 0.012) { d[k][q] = -f; any = 1; }
            else if (f >= 0.988 && f <= 0.996) { d[k][q] = 1.0 - f; any = 1; }
            else { memset(d, 0, sizeof(corners_t)); return 0; }
        }
    if (!any) memset(d, 0, sizeof(corners_t));
    return any;
}

/* texture per world unit, in A's tangent plane (e1, e2), from dP/da, dP/db and
 * duv/da, duv/db: J = U inv(M), M = [[Pa.e1, Pb.e1], [Pa.e2, Pb.e2]] */
static int jac2(const v3 *pa, const v3 *pb, const double da[2], const double db[2],
                const v3 *e1, const v3 *e2, double J[2][2])
{
    double m00 = dot3(pa, e1), m01 = dot3(pb, e1), m10 = dot3(pa, e2), m11 = dot3(pb, e2);
    double det = m00 * m11 - m01 * m10, i00, i01, i10, i11;
    int q;
    if (fabs(det) < 1e-12) return 0;
    i00 = m11 / det; i01 = -m01 / det; i10 = -m10 / det; i11 = m00 / det;
    for (q = 0; q < 2; q++) {
        J[q][0] = da[q] * i00 + db[q] * i10;
        J[q][1] = da[q] * i01 + db[q] * i11;
    }
    return 1;
}

/* Kind of the edge e of patch rp shared with edge f of rq (rev: the other way). */
static int classify(const uint8_t *rp, const uint8_t *rq, int e, int f, int rev,
                    corners_t cp, corners_t cq, float *jm_out, double D[2], int *cont)
{
    double aA, bA, aB, bB, d[2][2], dd, fr, JA[2][2], JB[2][2], da[2], db[2], nA = 0, nB = 0, nD = 0;
    v3 PA, PaA, PbA, PB, PaB, PbB, t, n, e1, e2;
    int s, q, k;

    *jm_out = -1.0f;
    *cont = 0;
    D[0] = D[1] = 0.0;
    edge_ab(e, 0.5, &aA, &bA);
    edge_ab(f, 0.5, &aB, &bB);
    rec_eval(rp, aA, bA, &PA, &PaA, &PbA);
    rec_eval(rq, aB, bB, &PB, &PaB, &PbB);
    t = sub3(&PA, &PB);
    if (sqrt(dot3(&t, &t)) >= GEO_TOL) return SEAM_NONE;
    for (s = 0; s < 2; s++) {
        double a1, b1, a2, b2, u1[2], u2[2];
        edge_ab(e, (double)s, &a1, &b1);
        edge_ab(f, rev ? 1.0 - s : (double)s, &a2, &b2);
        uv_at(cp, a1, b1, u1);
        uv_at(cq, a2, b2, u2);
        for (q = 0; q < 2; q++) d[s][q] = u2[q] - u1[q];
    }
    dd = hypot(d[0][0] - d[1][0], d[0][1] - d[1][1]);
    fr = hypot(d[0][0] - floor(d[0][0] + 0.5), d[0][1] - floor(d[0][1] + 0.5));
    D[0] = floor(d[0][0] + 0.5); D[1] = floor(d[0][1] + 0.5);
    if (rds(rp + 0x1C0) != rds(rq + 0x1C0)) {
        /* another texture; *cont: the placement itself follows across the edge */
        *cont = dd < 1e-3 && fr < 1e-3;
        return SEAM_TEXTURE;
    }
    t = (e == 0 || e == 2) ? PaA : PbA;
    n = cross3(&PaA, &PbA);
    e1 = unit3(t); n = unit3(n); e2 = cross3(&n, &e1);
    duv_at(cp, aA, bA, da, db);
    if (jac2(&PaA, &PbA, da, db, &e1, &e2, JA)) {
        duv_at(cq, aB, bB, da, db);
        if (jac2(&PaB, &PbB, da, db, &e1, &e2, JB)) {
            for (q = 0; q < 2; q++)
                for (k = 0; k < 2; k++) {
                    nA += JA[q][k] * JA[q][k]; nB += JB[q][k] * JB[q][k];
                    nD += (JA[q][k] - JB[q][k]) * (JA[q][k] - JB[q][k]);
                }
            *jm_out = (float)(sqrt(nD) / sqrt(nA > nB ? nA : nB));
        }
    }
    if (dd >= 1e-3) return SEAM_ROTATED;
    if (fr >= 1e-3) return SEAM_OFFSET;
    return *jm_out > J_HIGH ? SEAM_STRETCH : SEAM_OK;
}

/* b1 on edge e of patch rp: the neighbour's placement (rq, edge f), carried
 * across the edge by its texture-per-world derivative, blended into rows
 * 1..R of interior nodes (weights 1/2 (1 - r/(R+1)): the slope changes in
 * steps instead of at the edge). Nodes on the patch border never move. */
static void b1_edge(const uint8_t *rp, const uint8_t *rq, int e, int f, int rev,
                    corners_t cp, corners_t cq, const double D[2], float *duv)
{
    int s, r, q;
    for (s = 1; s < 7; s++) {
        double t = s / 7.0, aq, bq, uvN[2], da[2], db[2], g00, g01, g11, det, i00, i01, i11;
        v3 Pq, Paq, Pbq, row0, row1, JN[2];
        edge_ab(f, rev ? 1.0 - t : t, &aq, &bq);
        rec_eval(rq, aq, bq, &Pq, &Paq, &Pbq);
        uv_at(cq, aq, bq, uvN);
        uvN[0] -= D[0]; uvN[1] -= D[1];
        duv_at(cq, aq, bq, da, db);
        g00 = dot3(&Paq, &Paq); g01 = dot3(&Paq, &Pbq); g11 = dot3(&Pbq, &Pbq);
        det = g00 * g11 - g01 * g01;
        if (fabs(det) < 1e-12) continue;
        i00 = g11 / det; i01 = -g01 / det; i11 = g00 / det;
        /* pseudo-inverse of [Pa Pb]: (a, b) per world unit */
        row0.x = i00 * Paq.x + i01 * Pbq.x; row0.y = i00 * Paq.y + i01 * Pbq.y; row0.z = i00 * Paq.z + i01 * Pbq.z;
        row1.x = i01 * Paq.x + i11 * Pbq.x; row1.y = i01 * Paq.y + i11 * Pbq.y; row1.z = i01 * Paq.z + i11 * Pbq.z;
        for (q = 0; q < 2; q++) {
            JN[q].x = da[q] * row0.x + db[q] * row1.x;
            JN[q].y = da[q] * row0.y + db[q] * row1.y;
            JN[q].z = da[q] * row0.z + db[q] * row1.z;
        }
        for (r = 1; r <= s_rows; r++) {
            int nd = edge_node(e, s, r);
            double a = (nd % 8) / 7.0, b = (nd / 8) / 7.0, own[2], w = 0.5 * (1.0 - r / (s_rows + 1.0));
            v3 P, dP;
            rec_eval(rp, a, b, &P, NULL, NULL);
            uv_at(cp, a, b, own);
            dP = sub3(&P, &Pq);
            for (q = 0; q < 2; q++) {
                double ext = uvN[q] + dot3(&JN[q], &dP);
                duv[2 * nd + q] += (float)(w * (ext - own[q]));
            }
        }
    }
}

/* The neighbour's placement (rq, edge f, corners cq) carried across edge e of
 * rp to every node of rp: at the node's step along the edge, the neighbour's
 * coordinates plus its texture-per-world derivative times the offset from the
 * edge point (as b1_edge, without the whole-repeat shift: the neighbour's own
 * textures are sampled with its own placement). */
static void ext_edge(const uint8_t *rp, const uint8_t *rq, int e, int f, int rev, corners_t cq, float *out)
{
    int nd, q;
    for (nd = 0; nd < 64; nd++) {
        int i = nd % 8, j = nd / 8, s = (e == 0 || e == 2) ? i : j;
        double t = s / 7.0, aq, bq, uvN[2], da[2], db[2], g00, g01, g11, det, i00, i01, i11;
        v3 P, Pq, Paq, Pbq, row0, row1, dP;
        edge_ab(f, rev ? 1.0 - t : t, &aq, &bq);
        rec_eval(rq, aq, bq, &Pq, &Paq, &Pbq);
        uv_at(cq, aq, bq, uvN);
        duv_at(cq, aq, bq, da, db);
        rec_eval(rp, i / 7.0, j / 7.0, &P, NULL, NULL);
        g00 = dot3(&Paq, &Paq); g01 = dot3(&Paq, &Pbq); g11 = dot3(&Pbq, &Pbq);
        det = g00 * g11 - g01 * g01;
        {   /* the neighbour's tangent: its own texture u axis, dP/du (the title's tangents follow it) */
            double du = da[0] * db[1] - db[0] * da[1];
            v3 T;
            if (fabs(du) > 1e-12) {
                double ia = db[1] / du, ib = -da[1] / du;   /* (a, b) per u */
                T.x = Paq.x * ia + Pbq.x * ib; T.y = Paq.y * ia + Pbq.y * ib; T.z = Paq.z * ia + Pbq.z * ib;
                T = unit3(T);
            } else
                T = unit3(Paq);
            out[5 * nd + 2] = (float)T.x; out[5 * nd + 3] = (float)T.y; out[5 * nd + 4] = (float)T.z;
        }
        if (fabs(det) < 1e-12) { out[5 * nd] = (float)uvN[0]; out[5 * nd + 1] = (float)uvN[1]; continue; }
        i00 = g11 / det; i01 = -g01 / det; i11 = g00 / det;
        row0.x = i00 * Paq.x + i01 * Pbq.x; row0.y = i00 * Paq.y + i01 * Pbq.y; row0.z = i00 * Paq.z + i01 * Pbq.z;
        row1.x = i01 * Paq.x + i11 * Pbq.x; row1.y = i01 * Paq.y + i11 * Pbq.y; row1.z = i01 * Paq.z + i11 * Pbq.z;
        dP = sub3(&P, &Pq);
        for (q = 0; q < 2; q++) {
            double ja = dot3(&row0, &dP), jb = dot3(&row1, &dP);   /* (a, b) of the neighbour per offset */
            out[5 * nd + q] = (float)(uvN[q] + da[q] * ja + db[q] * jb);
        }
    }
}

/* ---- level tables (built on the game thread, read by the pump) ---- */

typedef struct {
    volatile uint32_t gen;      /* published last; 0 while (re)built */
    uint32_t obj, base, n;
    int32_t *partner;           /* n * 4: partner patch * 4 + its edge, or -1 */
    uint8_t *rev;               /* n * 4: the partner runs the edge the other way */
    uint8_t *kind;              /* n * 4: SEAM_* (the title's placement) */
    uint8_t *kindf;             /* n * 4: SEAM_* of the placement as drawn (after b0), for the clamp */
    float   *jm;                /* n * 4 */
    float   *lm;                /* n * 4: lightmap step, < 0 not measured (pump only) */
    float   *duv;               /* n * 64 * 2: texture offsets of b0 + b1, or NULL */
    uint8_t *moved;             /* n: some offset is not zero */
    uint8_t *cont;              /* n * 4: other texture, placement continuous (the blend's edges) */
    uint8_t *blendp;            /* n: the patch has such an edge */
    uint8_t *full;              /* n * 4: XBOX_TERRAIN_BLEND=2, rotated / mirrored, stretched, or other texture with
                                 * another placement: blended with the neighbour's own placement */
    float   *ext;               /* n * 4 * 64 * 5: that placement carried across the edge (u, v) and the
                                 * neighbour's tangent, per node */
    float   *box;               /* n * 6 (game thread only) */
    int16_t *dint;              /* n * 4 * 2: whole-repeat offset of the partner's placement (cont edges) */
} level_t;

static level_t s_lv[2];
static uint32_t s_gen;
static volatile int32_t s_rider = -1;   /* patch under the rider (debug view) */
static volatile uint32_t s_rider_gen;

typedef struct { int64_t a[3], b[3]; int32_t id; uint8_t sw; } edge_key;

static int key_cmp3(const int64_t *p, const int64_t *q)
{
    int k;
    for (k = 0; k < 3; k++) if (p[k] != q[k]) return p[k] < q[k] ? -1 : 1;
    return 0;
}

static int edge_cmp(const void *x, const void *y)
{
    const edge_key *p = (const edge_key *)x, *q = (const edge_key *)y;
    int c = key_cmp3(p->a, q->a);
    return c ? c : key_cmp3(p->b, q->b);
}

static void quant(const v3 *p, int64_t q[3])
{
    q[0] = llround(p->x * 4.0); q[1] = llround(p->y * 4.0); q[2] = llround(p->z * 4.0);
}

static void level_free(level_t *L)
{
    free(L->partner); free(L->rev); free(L->kind); free(L->kindf); L->kindf = NULL; free(L->jm); free(L->lm);
    free(L->duv); free(L->moved); free(L->box); free(L->cont); free(L->blendp); free(L->full); free(L->ext); free(L->dint); L->dint = NULL;
    L->cont = NULL; L->blendp = NULL; L->full = NULL; L->ext = NULL;
    L->partner = NULL; L->rev = NULL; L->kind = NULL; L->jm = NULL; L->lm = NULL;
    L->duv = NULL; L->moved = NULL; L->box = NULL; L->n = 0;
}

#define REC(L, p) ((const uint8_t *)XBOX_PTR((L)->base + (uint32_t)(p) * REC_SIZE))

static void level_build(uint32_t obj, uint32_t base, uint32_t n)
{
    static const int es[4] = { 0, 1, 2, 0 }, ee[4] = { 1, 3, 3, 2 };   /* corner ids of each edge */
    uint32_t gen = s_gen + 1u, p, m, i, j;
    level_t *L;
    edge_key *ek;
    corners_t *cn, *b0d;
    unsigned cnt[SEAM_KINDS] = { 0 }, b0n = 0, b1n = 0, paired = 0, blendn = 0, fulln = 0;

    if (gen == 0) gen = 1;
    L = &s_lv[gen & 1u];
    __atomic_store_n(&L->gen, 0u, __ATOMIC_RELEASE);
    level_free(L);
    s_gen = gen;
    s_rider = -1;
    L->obj = obj; L->base = base; L->n = n;
    L->partner = (int32_t *)malloc(sizeof(int32_t) * 4u * n);
    L->rev = (uint8_t *)calloc(4u * n, 1);
    L->kind = (uint8_t *)calloc(4u * n, 1);
    L->kindf = (uint8_t *)calloc(4u * n, 1);
    L->jm = (float *)calloc(4u * n, sizeof(float));
    L->lm = (float *)malloc(sizeof(float) * 4u * n);
    L->moved = (uint8_t *)calloc(n, 1);
    L->cont = (uint8_t *)calloc(4u * n, 1);
    L->blendp = (uint8_t *)calloc(n, 1);
    L->full = (uint8_t *)calloc(4u * n, 1);
    L->dint = (int16_t *)calloc(8u * n, sizeof(int16_t));
    L->ext = s_blend == 2 ? (float *)calloc((size_t)n * 1280u, sizeof(float)) : NULL;
    L->box = (float *)malloc(sizeof(float) * 6u * n);
    L->duv = (s_b0 || s_b1) ? (float *)calloc((size_t)n * 128u, sizeof(float)) : NULL;
    ek = (edge_key *)malloc(sizeof(edge_key) * 4u * n);
    cn = (corners_t *)malloc(sizeof(corners_t) * n);
    b0d = (corners_t *)calloc(n, sizeof(corners_t));
    if (!L->partner || !L->rev || !L->kind || !L->jm || !L->lm || !L->moved || !L->box || !L->cont || !L->blendp || !L->full || !L->dint || (s_blend == 2 && !L->ext) ||
        ((s_b0 || s_b1) && !L->duv) || !ek || !cn || !b0d) {
        free(ek); free(cn); free(b0d); level_free(L);
        return;                                         /* gen stays 0: nothing changed */
    }
    for (p = 0; p < n; p++) {
        const uint8_t *rec = REC(L, p);
        v3 c[4], P;
        int64_t q[4][3];
        int e, k;
        float *bx = L->box + 6u * p;
        rec_eval(rec, 0.0, 0.0, &c[0], NULL, NULL); rec_eval(rec, 1.0, 0.0, &c[1], NULL, NULL);
        rec_eval(rec, 0.0, 1.0, &c[2], NULL, NULL); rec_eval(rec, 1.0, 1.0, &c[3], NULL, NULL);
        for (e = 0; e < 4; e++) quant(&c[e], q[e]);
        for (e = 0; e < 4; e++) {
            edge_key *kk = &ek[p * 4u + (uint32_t)e];
            int sw = key_cmp3(q[es[e]], q[ee[e]]) > 0;
            memcpy(kk->a, q[sw ? ee[e] : es[e]], sizeof kk->a);
            memcpy(kk->b, q[sw ? es[e] : ee[e]], sizeof kk->b);
            kk->id = (int32_t)(p * 4u + (uint32_t)e);
            kk->sw = (uint8_t)sw;
            L->partner[p * 4u + (uint32_t)e] = -1;
            L->lm[p * 4u + (uint32_t)e] = -1.0f;
        }
        bx[0] = bx[1] = bx[2] = 1e30f; bx[3] = bx[4] = bx[5] = -1e30f;
        for (k = 0; k < 64; k++) {
            rec_eval(rec, (k % 8) / 7.0, (k / 8) / 7.0, &P, NULL, NULL);
            if (P.x < bx[0]) bx[0] = (float)P.x;
            if (P.y < bx[1]) bx[1] = (float)P.y;
            if (P.z < bx[2]) bx[2] = (float)P.z;
            if (P.x > bx[3]) bx[3] = (float)P.x;
            if (P.y > bx[4]) bx[4] = (float)P.y;
            if (P.z > bx[5]) bx[5] = (float)P.z;
        }
        rec_corners(rec, cn[p]);
        if (s_b0 && b0_deltas(cn[p], b0d[p])) b0n++;
    }
    m = 4u * n;
    qsort(ek, m, sizeof *ek, edge_cmp);
    for (i = 0; i < m; i = j) {
        for (j = i + 1; j < m && !edge_cmp(&ek[i], &ek[j]); j++) ;
        if (j - i == 2 && (ek[i].id >> 2) != (ek[i + 1].id >> 2)) {   /* exactly two patches share it */
            L->partner[ek[i].id] = ek[i + 1].id;
            L->partner[ek[i + 1].id] = ek[i].id;
            L->rev[ek[i].id] = L->rev[ek[i + 1].id] = (uint8_t)(ek[i].sw ^ ek[i + 1].sw);
        }
    }
    free(ek);
    /* kinds, then the texture offsets */
    for (p = 0; p < n; p++) {
        int e;
        corners_t fixed;
        int k, qq;
        if (L->duv) {
            for (k = 0; k < 64; k++) {      /* b0: bilinear of the corner moves */
                double a = (k % 8) / 7.0, b = (k / 8) / 7.0, o[2];
                uv_at(b0d[p], a, b, o);
                L->duv[128u * p + 2u * (uint32_t)k] = (float)o[0];
                L->duv[128u * p + 2u * (uint32_t)k + 1u] = (float)o[1];
            }
        }
        for (k = 0; k < 4; k++) for (qq = 0; qq < 2; qq++) fixed[k][qq] = cn[p][k][qq] + b0d[p][k][qq];
        for (e = 0; e < 4; e++) {
            int32_t pq = L->partner[p * 4u + (uint32_t)e];
            uint32_t q;
            int f, rev, kd, cont;
            double D[2];
            float jm;
            corners_t qfixed;
            if (pq < 0) { cnt[SEAM_NONE]++; continue; }
            q = (uint32_t)pq >> 2; f = pq & 3; rev = L->rev[p * 4u + (uint32_t)e];
            kd = classify(REC(L, p), REC(L, q), e, f, rev, cn[p],
                          cn[q], &jm, D, &cont);
            L->kind[p * 4u + (uint32_t)e] = (uint8_t)kd;
            if (L->kindf) {
                if (s_b0) {         /* b0 moves inset corners: the edge may follow once drawn */
                    double Df[2];
                    float jf;
                    int cf;
                    for (k = 0; k < 4; k++) for (qq = 0; qq < 2; qq++) qfixed[k][qq] = cn[q][k][qq] + b0d[q][k][qq];
                    L->kindf[p * 4u + (uint32_t)e] = (uint8_t)classify(REC(L, p), REC(L, q), e, f, rev, fixed, qfixed, &jf, Df, &cf);
                } else
                    L->kindf[p * 4u + (uint32_t)e] = (uint8_t)kd;
            }
            if (kd == SEAM_TEXTURE && cont) {
                L->cont[p * 4u + (uint32_t)e] = 1;
                L->dint[8u * p + 2u * (uint32_t)e] = (int16_t)D[0];
                L->dint[8u * p + 2u * (uint32_t)e + 1u] = (int16_t)D[1];
                L->blendp[p] = 1;
                blendn++;
            }
            if (s_blend == 2 && (kd == SEAM_ROTATED || kd == SEAM_STRETCH || (kd == SEAM_TEXTURE && !cont))) {
                for (k = 0; k < 4; k++) for (qq = 0; qq < 2; qq++) qfixed[k][qq] = cn[q][k][qq] + b0d[q][k][qq];
                ext_edge(REC(L, p), REC(L, q), e, f, rev, qfixed, L->ext + 1280u * p + 320u * (uint32_t)e);
                L->full[p * 4u + (uint32_t)e] = 1;
                L->blendp[p] = 1;
                fulln++;
            }
            L->jm[p * 4u + (uint32_t)e] = jm;
            cnt[kd]++;
            if (kd != SEAM_NONE) paired++;
            if (!s_b1 || !L->duv) continue;
            for (k = 0; k < 4; k++) for (qq = 0; qq < 2; qq++) qfixed[k][qq] = cn[q][k][qq] + b0d[q][k][qq];
            if (classify(REC(L, p), REC(L, q), e, f, rev, fixed,
                         qfixed, &jm, D, &cont) != SEAM_STRETCH)
                continue;
            b1_edge(REC(L, p), REC(L, q), e, f, rev, fixed,
                    qfixed, D, L->duv + 128u * p);
            b1n++;
        }
        if (L->duv)
            for (k = 0; k < 128; k++)
                if (L->duv[128u * p + (uint32_t)k] != 0.0f) { L->moved[p] = 1; break; }
    }
    free(cn); free(b0d);
    __atomic_store_n(&L->gen, gen, __ATOMIC_RELEASE);
    if (s_log || s_debug) {
        /* each shared edge is counted from both sides */
        fprintf(stderr, "[SEAMS] level: obj %08X, %u patches at %08X; edges shared %u (x2): continuous %u, "
                "stretched %u, 2-texel offset %u, rotated/mirrored %u, other texture %u; unshared %u | "
                "b0 patches %u, b1 edge sides %u, blend edge sides %u (+ %u other placement)\n",
                obj, n, base, paired / 2u, cnt[SEAM_OK] / 2u, cnt[SEAM_STRETCH] / 2u, cnt[SEAM_OFFSET] / 2u,
                cnt[SEAM_ROTATED] / 2u, cnt[SEAM_TEXTURE] / 2u, cnt[SEAM_NONE], b0n, b1n, blendn, fulln);
        fflush(stderr);
    }
}

static void rider_lookup(level_t *L)
{
    uint32_t app = MEM32(APP_GLOBAL), lvl, race, r0, p;
    float x, y, z;
    double best = 1e30;
    int32_t bp = -1;
    if (!valid_va(app)) return;
    lvl = MEM32(app + 0x72Cu);
    if (!valid_va(lvl)) return;
    race = MEM32(lvl + 0x1Cu);
    if (!valid_va(race) || !MEM32(race + 0x88u)) return;
    r0 = MEM32(race + 0xC4u);
    if (!valid_va(r0)) return;
    x = rdf((const uint8_t *)XBOX_PTR(r0 + 0x170u));
    y = rdf((const uint8_t *)XBOX_PTR(r0 + 0x174u));
    z = rdf((const uint8_t *)XBOX_PTR(r0 + 0x178u));
    for (p = 0; p < L->n; p++) {
        const float *bx = L->box + 6u * p;
        const uint8_t *rec;
        int i, j;
        if (x < bx[0] - 100.0f || x > bx[3] + 100.0f || y < bx[1] - 100.0f || y > bx[4] + 100.0f ||
            z < bx[2] - 100.0f || z > bx[5] + 100.0f)
            continue;
        rec = REC(L, p);
        for (j = 0; j <= 14; j++)
            for (i = 0; i <= 14; i++) {
                v3 P;
                double d;
                rec_eval(rec, i / 14.0, j / 14.0, &P, NULL, NULL);
                d = (P.x - x) * (P.x - x) + (P.y - y) * (P.y - y) + (P.z - z) * (P.z - z);
                if (d < best) { best = d; bp = (int32_t)p; }
            }
    }
    if (bp != s_rider) {
        s_rider = bp;
        s_rider_gen++;
        if (bp >= 0) {
            fprintf(stderr, "[SEAMS] rider at %.0f %.0f %.0f: patch %d (%.0f units away)\n", x, y, z, bp, sqrt(best));
            fflush(stderr);
        }
    }
}

/* ---- slot notes (written by the game thread, read by the pump) ---- */

typedef struct {
    volatile uint32_t seq;      /* odd while written */
    uint32_t addr, rec, pidx, lvl;
} slot_t;

static slot_t s_slot[NKEY];

void terrain_seams_note(uint32_t obj, uint32_t vb, uint32_t pidx)
{
    static ULONGLONG t_rider;
    uint32_t base, n, addr, s;
    level_t *L;
    slot_t *sl;

    if (!g_terrain_seams) return;
    base = MEM32(obj + 0x9CC0u);
    n = MEM32(obj + 0x2Cu);
    if (s_log) {
        static unsigned shown;
        if (shown < 4) {
            shown++;
            fprintf(stderr, "[SEAMS] note: obj %08X slot %08X patch %u, patches %u at %08X\n", obj, vb, pidx, n, base);
            fflush(stderr);
        }
    }
    if (n == 0 || n > 65536u || !valid_va(base) || pidx >= n) return;
    L = &s_lv[s_gen & 1u];
    if (!s_gen || L->obj != obj || L->base != base || L->n != n) {
        level_build(obj, base, n);
        L = &s_lv[s_gen & 1u];
    }
    if (!L->gen) return;
    if (s_debug) {
        ULONGLONG t = GetTickCount64();
        if (t - t_rider >= 250) { t_rider = t; rider_lookup(L); }
    }
    addr = vb & 0x03FFFFFFu;
    sl = &s_slot[(addr / SLOT_BYTES) & (NKEY - 1u)];
    s = sl->seq;
    __atomic_store_n(&sl->seq, s | 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    sl->addr = addr; sl->rec = base + pidx * REC_SIZE; sl->pidx = pidx; sl->lvl = L->gen;
    __atomic_store_n(&sl->seq, (s | 1u) + 1u, __ATOMIC_RELEASE);
}

/* ---- the pump side ---- */

static uint32_t s_l8[112];      /* the title's 8x8 strip with no edge remapped */
/* host buffer: the slot's 64 vertices, then 4 x 64 copies, one per blended
 * edge (vertex numbers 64 (1 + edge) + node: the patch's own vertices, or
 * with a neighbour's placement), then the per-vertex alphas: 64 floats per
 * edge for the slot's own vertices (debug lines), then 64 per edge for the
 * copies (the blends: one base of weights for every edge, so that the edges
 * of the same state are drawn together) */
#define ALTV        SLOT_BYTES
#define WEIGHTS     (5u * SLOT_BYTES)
#define WALT        (WEIGHTS + 4u * 256u)
#define VBUF_BYTES  (WALT + 4u * 256u)
static uint32_t s_bl[4][98 * 3];
static uint32_t s_cat[4 * 98 * 3];      /* the blends of a group of edges, one after the other */

/* the textures the pump has seen bound for each snow (t0) / normal map (t2)
 * number of the level's patches */
#define TEXN 4096u
static struct {
    uint32_t gen; int32_t id[2][TEXN]; uint32_t off[2][TEXN], fmt[2][TEXN];
    int8_t has_mean[TEXN]; float mean[TEXN][3];     /* the snow texture's mean colour (0..255) */
    float hist[TEXN][64], sd[TEXN];                 /* colour histogram (4 levels a channel), luminance spread */
    uint32_t pair_key[1024]; int8_t pair_ok[1024];  /* decisions already taken (and logged) */
} s_tex;
static double s_similar = 80.0;     /* XBOX_TERRAIN_BLEND_SIMILAR (loose match): mean colours this close */
/* XBOX_TERRAIN_BLEND_MATCH: strict (default) -- the same texture, or two
 * textures with the same content (close mean, same colour histogram, same
 * spread): plain snow with plain snow, never a marked or striped texture with
 * a plain one; same -- the same texture only; loose -- the mean colours only,
 * and the continuous edges unchecked (as first tried). */
static int s_match = 1;             /* 0 loose, 1 strict, 2 same */
static double s_strict_mean = 40.0, s_strict_hist = 0.70, s_strict_sd = 15.0;   /* from the content-match bench */

/* Mean colour of a texture's first level, from up to 256 DXT colour blocks
 * (both end points) or 1024 texels of a 32-bit format; 0 when unknown. */
static int tex_mean(uint32_t off, uint32_t fmt, float out[3], float hist[64], float *sd)
{
    uint32_t color = (fmt >> 8) & 0xFFu, w = 1u << ((fmt >> 20) & 0xFu), h = 1u << ((fmt >> 24) & 0xFu), n, k, step, bs;
    const uint8_t *b = (const uint8_t *)XBOX_PTR(off & 0x03FFFFFFu);
    double s[3] = { 0, 0, 0 }, l1 = 0.0, l2 = 0.0;
    memset(hist, 0, 64 * sizeof(float));
    if (w > 4096u || h > 4096u) return 0;
    if (color == 0x0C || color == 0x0E || color == 0x0F) {
        bs = color == 0x0C ? 8u : 16u;
        n = (w / 4u ? w / 4u : 1u) * (h / 4u ? h / 4u : 1u);
        step = n > 256u ? n / 256u : 1u;
        for (k = 0; k < n; k += step) {
            const uint8_t *q = b + bs * k + (bs == 16u ? 8u : 0u);
            int e;
            for (e = 0; e < 2; e++) {
                uint32_t c = (uint32_t)q[2 * e] | ((uint32_t)q[2 * e + 1] << 8);
                double r = ((c >> 11) & 31u) * 255.0 / 31.0, g = ((c >> 5) & 63u) * 255.0 / 63.0, bl = (c & 31u) * 255.0 / 31.0;
                double lu = 0.299 * r + 0.587 * g + 0.114 * bl;
                s[0] += r; s[1] += g; s[2] += bl; l1 += lu; l2 += lu * lu;
                hist[16 * ((int)r >> 6) + 4 * ((int)g >> 6) + ((int)bl >> 6)] += 1.0f;
            }
        }
        n = 2u * ((n + step - 1u) / step);
    } else if (color == 0x06 || color == 0x07 || color == 0x12 || color == 0x1E) {
        n = w * h; step = n > 1024u ? n / 1024u : 1u;
        for (k = 0; k < n; k += step) {
            int r = b[4 * k + 2], g = b[4 * k + 1], bl = b[4 * k];
            double lu = 0.299 * r + 0.587 * g + 0.114 * bl;
            s[0] += r; s[1] += g; s[2] += bl; l1 += lu; l2 += lu * lu;
            hist[16 * (r >> 6) + 4 * (g >> 6) + (bl >> 6)] += 1.0f;
        }
        n = (n + step - 1u) / step;
    } else
        return 0;
    for (k = 0; k < 3; k++) out[k] = (float)(s[k] / n);
    for (k = 0; k < 64; k++) hist[k] /= (float)n;
    l1 /= n; *sd = (float)sqrt(l2 / n - l1 * l1 > 0.0 ? l2 / n - l1 * l1 : 0.0);
    return 1;
}

static void tex_note(uint32_t gen, const uint8_t *rec)
{
    int k;
    if (s_tex.gen != gen) { memset(&s_tex, 0, sizeof s_tex); s_tex.gen = gen; }
    for (k = 0; k < 2; k++) {
        int id = rds(rec + 0x1C0 + 2 * k);
        uint32_t off, fmt, h = (uint32_t)id & (TEXN - 1u);
        pgraph_d3d11_tex_binding(k ? 2 : 0, &off, &fmt);
        if (id < 0 || !off) continue;
        if (k == 0 && s_blend && (s_tex.id[0][h] != id + 1 || s_tex.off[0][h] != off)) {
            s_tex.has_mean[h] = (int8_t)tex_mean(off, fmt, s_tex.mean[h], s_tex.hist[h], &s_tex.sd[h]);
            if (s_log) {
                fprintf(stderr, "[SEAMS] texture %d at %08X fmt %08X: mean %.0f %.0f %.0f spread %.1f%s\n", id, off, fmt,
                        s_tex.mean[h][0], s_tex.mean[h][1], s_tex.mean[h][2], s_tex.sd[h], s_tex.has_mean[h] ? "" : " (unknown)");
                fflush(stderr);
            }
        }
        s_tex.id[k][h] = id + 1; s_tex.off[k][h] = off; s_tex.fmt[k][h] = fmt;
    }
}

/* Two snow textures that may be blended into each other (see s_match). */
static int tex_similar(int a, int b)
{
    uint32_t ha = (uint32_t)a & (TEXN - 1u), hb = (uint32_t)b & (TEXN - 1u), key, slot;
    double d = 0.0, inter = 0.0, dsd;
    int k, ok;
    if (a == b && a >= 0) return 1;                 /* the same texture, placed otherwise */
    if (s_match == 2) return 0;
    if (a < 0 || b < 0 || s_tex.id[0][ha] != a + 1 || s_tex.id[0][hb] != b + 1 || !s_tex.has_mean[ha] || !s_tex.has_mean[hb])
        return 0;
    key = a < b ? ((uint32_t)a << 16 | (uint32_t)b) + 1u : ((uint32_t)b << 16 | (uint32_t)a) + 1u;
    slot = (key * 2654435761u) >> 22;
    if (s_tex.pair_key[slot] == key) return s_tex.pair_ok[slot];
    for (k = 0; k < 3; k++) d += (s_tex.mean[ha][k] - s_tex.mean[hb][k]) * (s_tex.mean[ha][k] - s_tex.mean[hb][k]);
    d = sqrt(d);
    for (k = 0; k < 64; k++) inter += s_tex.hist[ha][k] < s_tex.hist[hb][k] ? s_tex.hist[ha][k] : s_tex.hist[hb][k];
    dsd = fabs(s_tex.sd[ha] - s_tex.sd[hb]);
    ok = s_match == 0 ? d <= s_similar : (d <= s_strict_mean && inter >= s_strict_hist && dsd <= s_strict_sd);
    s_tex.pair_key[slot] = key; s_tex.pair_ok[slot] = (int8_t)ok;
    if (s_log) {
        fprintf(stderr, "[SEAMS] textures %d / %d: mean %.0f apart, histograms %.2f in common, spread %.1f apart -> %s\n",
                a, b, d, inter, dsd, ok ? "blend" : "kept sharp");
        fflush(stderr);
    }
    return ok;
}

static int tex_find(int k, int id, uint32_t *off, uint32_t *fmt)
{
    uint32_t h = (uint32_t)id & (TEXN - 1u);
    if (id < 0 || s_tex.id[k][h] != id + 1) return 0;
    *off = s_tex.off[k][h]; *fmt = s_tex.fmt[k][h];
    return 1;
}

static int edge_rows(int e, int nd)     /* rows of nodes between node nd and edge e */
{
    int i = nd % 8, j = nd / 8;
    return e == 0 ? j : e == 1 ? 7 - i : e == 2 ? 7 - j : i;
}
static uint32_t s_ov[SEAM_KINDS + 2][98 * 3];

/* lightmap atlases as the pump sees them bound (t1) for each atlas number */
#define ATLN 1024             /* lightmap atlas numbers (rec+0x1C4) seen up to ~170 */
static struct { uint32_t gen, off[ATLN], fmt[ATLN]; } s_atl;

static struct {
    unsigned long long draws, terrain, moved, subst, race, stale, corner, overlays, blends, clamped;
    unsigned long long hits, misses, changed, edges;    /* the patch cache */
    ULONGLONG t0;
} ps;

/* The patch cache (pump only). What a substituted draw hands out -- the
 * vertices with their texture coordinates moved / clamped, the copies and
 * alphas of each blended edge, the triangles of each band -- depends only on
 * the slot's 64 vertices, the patch's record, the title's 112 indices and the
 * level's tables. An entry is keyed by (level, patch) and used only when the
 * slot, the record and the indices are byte for byte the ones it was built
 * from (one byte changed: built again); the level's tables change with the
 * level's number. What is decided per draw stays per draw: whether an edge is
 * blended and with which textures (learnt in flight), the address words (the
 * title's state), the neighbour's clamp (its record: compared with the one
 * the edge was built with). 128 sets of 4 (the patch number picks the set),
 * the least recently used one goes: bounded, and the same draws make the
 * same choices. The debug view builds into a scratch entry, every draw. */
#define SC_SETS 128u
#define SC_WAYS 4u
typedef struct {
    uint32_t gen, pidx;                 /* key; gen 0: empty */
    unsigned long long use;             /* last use, for the eviction */
    uint8_t ebuilt;                     /* bit per edge: its copy, alphas and band are built */
    int8_t ecmask[4];                   /* the neighbour's clamped axes they were built with */
    double eqshift[4][2];
    uint32_t enb[4];                    /* indices of each edge's band */
    uint32_t idx[112];                  /* the title's data the entry was built from */
    uint8_t slot[SLOT_BYTES], rec[REC_SIZE];
    uint8_t vbuf[VBUF_BYTES];           /* handed to the translator as it is */
    uint32_t bl[4][98 * 3];             /* each edge's band (triangle list over its copy) */
} sc_entry;
static sc_entry *s_sc, s_scratch;
static int s_sc_failed;
static unsigned long long s_sc_use;

/* The entry of this patch with these very bytes (*hit = 1), or the one to
 * build it in (*hit = 0; gen cleared until it is built). */
static sc_entry *sc_get(uint32_t gen, uint32_t pidx, const uint8_t *slot, const uint8_t *rec,
                        const uint32_t *idx, int *hit)
{
    sc_entry *set, *victim = NULL;
    uint32_t w;
    int keyed = 0;
    *hit = 0;
    if (!s_sc && !s_sc_failed) {
        s_sc = (sc_entry *)calloc(SC_SETS * SC_WAYS, sizeof *s_sc);
        if (!s_sc) {
            s_sc_failed = 1;
            fprintf(stderr, "[SEAMS] patch cache: out of memory, built every draw\n");
            fflush(stderr);
        }
    }
    if (!s_sc) { ps.misses++; s_scratch.gen = 0; return &s_scratch; }
    set = s_sc + (pidx & (SC_SETS - 1u)) * SC_WAYS;
    for (w = 0; w < SC_WAYS; w++) {
        sc_entry *E = &set[w];
        if (E->gen == gen && E->pidx == pidx) {
            if (!memcmp(E->idx, idx, sizeof E->idx) && !memcmp(E->rec, rec, REC_SIZE) &&
                !memcmp(E->slot, slot, SLOT_BYTES)) {
                E->use = ++s_sc_use;
                *hit = 1;
                ps.hits++;
                return E;
            }
            keyed = 1;
        }
        if (!victim || (victim->gen && (!E->gen || E->use < victim->use))) victim = E;
    }
    ps.misses++;
    if (keyed) ps.changed++;
    victim->gen = 0;
    victim->use = ++s_sc_use;
    return victim;
}

static void snake(uint32_t *out, int dims)
{
    int r, c, n = 0;
    for (r = 0; r < dims - 1; r++)
        for (c = 0; c < dims; c++) {
            int col = (r & 1) ? dims - 1 - c : c;
            out[n++] = (uint32_t)(col + r * dims);
            out[n++] = (uint32_t)(col + (r + 1) * dims);
        }
}

static uint32_t swz128(int x, int y)
{
    uint32_t i = 0;
    int b, bit = 0;
    for (b = 0; b < 7; b++) {
        i |= (uint32_t)((x >> b) & 1) << bit++;
        i |= (uint32_t)((y >> b) & 1) << bit++;
    }
    return i;
}

/* one lightmap texel of patch rec at node (i, j), -1 when the atlas is unknown */
static int lm_texel(const uint8_t *rec, int i, int j, int rgb[3])
{
    int a = rds(rec + 0x1C4), x, y, swz;
    uint32_t off, fmt, k;
    const uint8_t *t;
    if (a < 0 || a >= ATLN || !s_atl.off[a]) return -1;
    off = s_atl.off[a]; fmt = s_atl.fmt[a];
    if (((fmt >> 20) & 0xF) != 7 || ((fmt >> 24) & 0xF) != 7) return -1;    /* 128 x 128 only */
    if (((fmt >> 8) & 0xFF) == 0x06) swz = 1;
    else if (((fmt >> 8) & 0xFF) == 0x12) swz = 0;
    else return -1;
    x = (int)lrint(rdf(rec + 0x00) * 128.0) + i;
    y = (int)lrint(rdf(rec + 0x04) * 128.0) + j;
    if (x < 0 || x > 127 || y < 0 || y > 127) return -1;
    k = swz ? swz128(x, y) : (uint32_t)(y * 128 + x);
    t = (const uint8_t *)XBOX_PTR((off & 0x03FFFFFFu) + 4u * k);
    rgb[0] = t[2]; rgb[1] = t[1]; rgb[2] = t[0];
    return 0;
}

static float lm_step(level_t *L, uint32_t p, int e)
{
    int32_t pq = L->partner[p * 4u + (uint32_t)e];
    const uint8_t *rp, *rq;
    uint32_t q;
    int s, f, rev;
    double sum = 0.0;
    if (pq < 0 || L->kind[p * 4u + (uint32_t)e] == SEAM_NONE) return -2.0f;
    if (L->lm[p * 4u + (uint32_t)e] >= 0.0f) return L->lm[p * 4u + (uint32_t)e];
    q = (uint32_t)pq >> 2; f = pq & 3; rev = L->rev[p * 4u + (uint32_t)e];
    rp = REC(L, p); rq = REC(L, q);
    for (s = 0; s < 8; s++) {
        int na = edge_node(e, s, 0), nb = edge_node(f, rev ? 7 - s : s, 0), ca[3], cb[3];
        if (lm_texel(rp, na % 8, na / 8, ca) || lm_texel(rq, nb % 8, nb / 8, cb)) return -1.0f;
        sum += (abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) + abs(ca[2] - cb[2])) / 3.0;
    }
    L->lm[p * 4u + (uint32_t)e] = (float)(sum / 8.0);
    return L->lm[p * 4u + (uint32_t)e];
}

static void pump_report(void)
{
    ULONGLONG t = GetTickCount64();
    if (!ps.t0) { ps.t0 = t; return; }
    if (t - ps.t0 < 2000) return;
    ps.t0 = t;
    fprintf(stderr, "[SEAMS] pump: draws %llu terrain %llu moved %llu substituted %llu overlays %llu blends %llu | "
            "clamped %llu | refused: race %llu stale %llu corner %llu | cache: hits %llu misses %llu (changed %llu) edges built %llu\n",
            ps.draws, ps.terrain, ps.moved, ps.subst, ps.overlays, ps.blends, ps.clamped, ps.race, ps.stale, ps.corner,
            ps.hits, ps.misses, ps.changed, ps.edges);
    fflush(stderr);
}

static void rider_report(level_t *L, uint32_t p)
{
    int e;
    fprintf(stderr, "[SEAMS] rider patch %u: t0 %d t2 %d lightmap %d\n", p, rds(REC(L, p) + 0x1C0),
            rds(REC(L, p) + 0x1C2), rds(REC(L, p) + 0x1C4));
    for (e = 0; e < 4; e++) {
        int32_t pq = L->partner[p * 4u + (uint32_t)e];
        float lm = lm_step(L, p, e);
        if (pq < 0) { fprintf(stderr, "[SEAMS]   edge %d: unshared\n", e); continue; }
        fprintf(stderr, "[SEAMS]   edge %d: patch %d edge %d%s, %s, J %.2f, lightmap step %.1f/255%s\n", e,
                pq >> 2, pq & 3, L->rev[p * 4u + (uint32_t)e] ? " (reversed)" : "",
                k_kind[L->kind[p * 4u + (uint32_t)e]], L->jm[p * 4u + (uint32_t)e], lm,
                lm >= LM_STEP ? " STEP" : "");
    }
    fflush(stderr);
}

/* XBOX_TERRAIN_CLAMP: the axes (bit 0 u, bit 1 v) of the snow / normal map
 * coordinates on which a patch lays a single repeat of its texture (all four
 * corners inside one whole tile [k, k + 1]); shift[] = k. The title samples
 * with wrap, so at the patch's edge the bilinear (and mip) footprint reads the
 * texels of the opposite side of the texture. 1: every such axis is clamped.
 * 2: only when one of the axis's two edges is not a continuation of the same
 * texture with the same placement (there, wrap is what the neighbour shows). */
static int s_clamp;
static int clamp_axes(level_t *L, uint32_t p, corners_t c, double shift[2])
{
    static const int ec[4][2] = { { 0, 2 }, { 2, 3 }, { 1, 3 }, { 0, 1 } };   /* corners of each edge */
    int q, m = 0, e;
    for (q = 0; q < 2; q++) {
        double lo = c[0][q], hi = c[0][q], k;
        int i, need = s_clamp == 1;
        for (i = 1; i < 4; i++) { if (c[i][q] < lo) lo = c[i][q]; if (c[i][q] > hi) hi = c[i][q]; }
        k = floor(lo + 1e-4);
        if (hi > k + 1.0 + 1e-4 || hi - lo < 0.5) continue;      /* several repeats, or a part of one */
        if (!need)
            for (e = 0; e < 4 && !need; e++) {
                double x0 = c[ec[e][0]][q], x1 = c[ec[e][1]][q];
                int32_t pq;
                int kd;
                if (!((fabs(x0 - lo) < 1e-3 && fabs(x1 - lo) < 1e-3) || (fabs(x0 - hi) < 1e-3 && fabs(x1 - hi) < 1e-3)))
                    continue;                                       /* not an edge of this axis */
                pq = L->partner[p * 4u + (uint32_t)e];
                kd = pq >= 0 ? (L->kindf ? L->kindf : L->kind)[p * 4u + (uint32_t)e] : SEAM_NONE;
                if (kd != SEAM_OK && kd != SEAM_STRETCH) need = 1;
            }
        if (!need) continue;
        shift[q] = k;
        m |= 1 << q;
    }
    return m;
}

/* Edge ed of the patch in E (built, its vertices final): the copy of its
 * vertices the blend reads (the patch's own, or the neighbour's placement
 * carried across the edge, or its own coordinates moved into the neighbour's
 * clamped tile), the alphas (1/2 on the edge, 0 at the band's end) and the
 * strip's triangles in the band, over the copy. */
static void sc_edge(sc_entry *E, level_t *L, uint32_t pidx, int ed, int full, int cmask, const double qshift[2],
                    int clampm, const double shift[2])
{
    uint32_t vofs = 64u * (1u + (uint32_t)ed), nb = 0, w;   /* vertex numbers of the copy */
    float *wt = (float *)(E->vbuf + WALT + 256u * (uint32_t)ed);
    uint8_t *cv = E->vbuf + ALTV + SLOT_BYTES * (uint32_t)ed;
    int Re = s_blend_rows, m;
    if (s_width > 0.0) {
        /* alpha 1/2 on the edge, 0 at s_width metres (39.4 units each) from it, whatever
         * the cells' size: the weights run on below 0 and the pixel shader clamps them */
        double Wu = s_width * 39.4;
        Re = 1;
        for (w = 0; w < 64; w++) {
            int r = edge_rows(ed, (int)w), en = (int)w, s;
            const uint8_t *a = E->vbuf + VSZ * w, *b;
            double dx, dy, dz, dd;
            /* the node of the same step on the edge */
            s = (ed == 0 || ed == 2) ? en % 8 : en / 8;
            b = E->vbuf + VSZ * (uint32_t)edge_node(ed, s, 0);
            dx = rdf(a) - rdf(b); dy = rdf(a + 4) - rdf(b + 4); dz = rdf(a + 8) - rdf(b + 8);
            dd = sqrt(dx * dx + dy * dy + dz * dz);
            wt[w] = (float)(0.5 * (1.0 - dd / Wu));
            if (wt[w] < -8.0f) wt[w] = -8.0f;
            if (r > 0 && r <= 3 && dd < Wu && r + 1 > Re) Re = r + 1;
        }
        if (Re > 3) Re = 3;
    } else
        for (w = 0; w < 64; w++) {
            int r = edge_rows(ed, (int)w);
            wt[w] = r < s_blend_rows ? (float)(0.5 * (1.0 - r / (double)s_blend_rows)) : 0.0f;
        }
    memcpy(cv, E->vbuf, SLOT_BYTES);
    if (full) {             /* the neighbour's placement */
        const float *x = L->ext + 1280u * pidx + 320u * (uint32_t)ed;
        for (w = 0; w < 64; w++) {
            memcpy(cv + VSZ * w + 0x0C, &x[5 * w], 4);
            memcpy(cv + VSZ * w + 0x1C, &x[5 * w + 1], 4);
            if (s_blend_tangent) memcpy(cv + VSZ * w + 0x10, &x[5 * w + 2], 12);   /* relief lit in its own frame */
        }
    } else if (cmask) {     /* the same placement, as the neighbour's own coordinates: own + whole repeats */
        for (w = 0; w < 64; w++) {
            float u = rdf(cv + VSZ * w + 0x0C) + (float)(((clampm & 1) ? shift[0] : 0.0) + L->dint[8u * pidx + 2u * (uint32_t)ed]),
                  v = rdf(cv + VSZ * w + 0x1C) + (float)(((clampm & 2) ? shift[1] : 0.0) + L->dint[8u * pidx + 2u * (uint32_t)ed + 1u]);
            memcpy(cv + VSZ * w + 0x0C, &u, 4);
            memcpy(cv + VSZ * w + 0x1C, &v, 4);
        }
    }
    if (cmask) {            /* into the neighbour's tile, read with clamp like its own pass */
        for (w = 0; w < 64; w++) {
            float u = rdf(cv + VSZ * w + 0x0C) - (float)qshift[0], v = rdf(cv + VSZ * w + 0x1C) - (float)qshift[1];
            if (cmask & 1) memcpy(cv + VSZ * w + 0x0C, &u, 4);
            if (cmask & 2) memcpy(cv + VSZ * w + 0x1C, &v, 4);
        }
    }
    for (m = 0; m + 2 < 112; m++) {
        uint32_t a0 = s_l8[m], a1 = s_l8[m + 1], a2 = s_l8[m + 2];
        uint32_t r0 = E->idx[m], r1 = E->idx[m + 1], r2 = E->idx[m + 2];
        int rmin;
        if (a0 == a1 || a1 == a2 || a0 == a2 || r0 == r1 || r1 == r2 || r0 == r2) continue;
        rmin = edge_rows(ed, (int)a0);
        if (edge_rows(ed, (int)a1) < rmin) rmin = edge_rows(ed, (int)a1);
        if (edge_rows(ed, (int)a2) < rmin) rmin = edge_rows(ed, (int)a2);
        if (rmin >= Re) continue;                    /* the triangle's cell is outside the band */
        if (m & 1) { uint32_t tt = r0; r0 = r1; r1 = tt; }
        E->bl[ed][nb] = r0 + vofs; E->bl[ed][nb + 1] = r1 + vofs; E->bl[ed][nb + 2] = r2 + vofs; nb += 3;
    }
    E->enb[ed] = nb;
    E->ecmask[ed] = (int8_t)cmask;
    E->eqshift[ed][0] = qshift[0]; E->eqshift[ed][1] = qshift[1];
    E->ebuilt |= (uint8_t)(1u << ed);
    ps.edges++;
}

static int subst(uint32_t mode, const uint32_t *idx, uint32_t count,
                 const uint32_t offset[16], const uint32_t format[16], PgraphDrawSubst *out)
{
    static const int corner[4] = { 0, 7, 56, 63 };
    static const double ca[4] = { 0, 1, 0, 1 }, cb[4] = { 0, 0, 1, 1 };
    static const float col[SEAM_KINDS + 2][4] = {
        { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
        { 1.0f, 0.0f, 1.0f, 0.5f },     /* stretched: magenta */
        { 1.0f, 1.0f, 0.0f, 0.5f },     /* 2-texel offset: yellow */
        { 1.0f, 0.1f, 0.1f, 0.55f },    /* rotated / mirrored: red */
        { 0.0f, 0.9f, 1.0f, 0.45f },    /* other texture: cyan */
        { 1.0f, 0.55f, 0.0f, 0.5f },    /* lightmap step: orange */
        { 0.1f, 1.0f, 0.1f, 0.2f },     /* the patch under the rider: green */
    };
    static const int prio[SEAM_KINDS + 1] = { 0, 0, 3, 2, 5, 4, 1 };
    static uint32_t last_rider_gen = ~0u;
    uint32_t addr, key, s1, novl[SEAM_KINDS + 2] = { 0 }, k;
    slot_t e;
    level_t *L;
    sc_entry *ent;
    int moved, dbg = s_debug, m, blend = 0, clampm = 0;
    double shift[2] = { 0.0, 0.0 };

    if (s_log) { ps.draws++; pump_report(); }
    if (mode != 6) return 0;
    if (count != 112 || ((format[0] >> 8) & 0xFFFFFFu) != VSZ || (format[0] & 0xFFu) != 0x32u) return 0;
    addr = offset[0] & 0x03FFFFFFu;
    key = (addr / SLOT_BYTES) & (NKEY - 1u);
    {
        slot_t *sl = &s_slot[key];
        s1 = __atomic_load_n(&sl->seq, __ATOMIC_ACQUIRE);
        e = *sl;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if ((s1 & 1u) || __atomic_load_n(&sl->seq, __ATOMIC_RELAXED) != s1) { ps.race++; return 0; }
    }
    if (e.addr != addr || !e.lvl) return 0;
    ps.terrain++;
    if (s_log) {
        /* the sampling the title asks for on the terrain's stages (first draws of each new state) */
        static uint32_t seen[8][4];
        static unsigned nseen;
        uint32_t f[4], fl[4], ad[4], c0[4], st, j;
        int en[4];
        for (st = 0; st < 4; st++) pgraph_d3d11_tex_state((int)st, &f[st], &fl[st], &ad[st], &c0[st], &en[st]);
        for (j = 0; j < nseen; j++) if (!memcmp(seen[j], fl, sizeof fl) && seen[j][3] == (ad[0] ^ (ad[1] << 4) ^ (ad[2] << 8))) break;
        if (j == nseen && nseen < 8) {
            memcpy(seen[nseen], fl, sizeof fl); seen[nseen][3] = ad[0] ^ (ad[1] << 4) ^ (ad[2] << 8); nseen++;
            for (st = 0; st < 4; st++) {
                int32_t bias = (int32_t)(fl[st] & 0x1FFFu);
                if (bias & 0x1000) bias -= 0x2000;
                fprintf(stderr, "[SEAMS] terrain stage %u: %s fmt %08X (colour %02X, %ux%u, %u mip levels) filter %08X (min %u mag %u, bias %.2f) "
                        "address %08X (U %u V %u P %u) control0 %08X (aniso %u, max lod %.2f, min lod %.2f)\n",
                        st, en[st] ? "on " : "off", f[st], (f[st] >> 8) & 0xFFu, 1u << ((f[st] >> 20) & 0xFu), 1u << ((f[st] >> 24) & 0xFu),
                        (f[st] >> 16) & 0xFu, fl[st], (fl[st] >> 16) & 0xFFu, (fl[st] >> 24) & 0xFu, bias / 256.0,
                        ad[st], ad[st] & 0xFu, (ad[st] >> 8) & 0xFu, (ad[st] >> 16) & 0xFu, c0[st], 1u << ((c0[st] >> 4) & 3u),
                        ((c0[st] >> 18) & 0xFFFu) / 256.0, ((c0[st] >> 6) & 0xFFFu) / 256.0);
            }
            fflush(stderr);
        }
    }
    L = &s_lv[e.lvl & 1u];
    if (__atomic_load_n(&L->gen, __ATOMIC_ACQUIRE) != e.lvl || e.pidx >= L->n) { ps.stale++; return 0; }
    moved = L->duv && L->moved[e.pidx];
    if (moved) ps.moved++;
    if (s_blend) {
        const uint8_t *rec = (const uint8_t *)XBOX_PTR(e.rec);
        tex_note(e.lvl, rec);
        if (L->blendp[e.pidx]) {                /* a neighbour's textures known for one of its edges? */
            int ed;
            for (ed = 0; ed < 4; ed++) {
                int32_t pq = L->partner[e.pidx * 4u + (uint32_t)ed];
                uint32_t o, f;
                if ((L->cont[e.pidx * 4u + (uint32_t)ed] || L->full[e.pidx * 4u + (uint32_t)ed]) && pq >= 0 &&
                    ((L->cont[e.pidx * 4u + (uint32_t)ed] && s_match == 0) ||
                     tex_similar(rds(rec + 0x1C0), rds(REC(L, (uint32_t)pq >> 2) + 0x1C0))) &&
                    tex_find(0, rds(REC(L, (uint32_t)pq >> 2) + 0x1C0), &o, &f) &&
                    tex_find(1, rds(REC(L, (uint32_t)pq >> 2) + 0x1C2), &o, &f)) { blend = 1; break; }
            }
        }
    }
    {
        corners_t c;
        rec_corners((const uint8_t *)XBOX_PTR(e.rec), c);
        clampm = s_clamp ? clamp_axes(L, e.pidx, c, shift) : 0;
    }
    if (!moved && !dbg && !blend && !clampm) return 0;

    {
        const uint8_t *gslot = (const uint8_t *)XBOX_PTR(e.addr), *grec = (const uint8_t *)XBOX_PTR(e.rec);
        int hit = 0;
        if (dbg) { ent = &s_scratch; ent->gen = 0; }
        else ent = sc_get(e.lvl, e.pidx, gslot, grec, idx, &hit);
        if (!hit) {
            memcpy(ent->slot, gslot, SLOT_BYTES);
            memcpy(ent->rec, grec, REC_SIZE);
            memcpy(ent->idx, idx, sizeof ent->idx);
            memcpy(ent->vbuf, ent->slot, SLOT_BYTES);
            for (k = 0; k < 4; k++) {           /* the slot really holds this patch */
                v3 p;
                const uint8_t *nv = ent->vbuf + VSZ * (uint32_t)corner[k];
                double x = rdf(nv), y = rdf(nv + 4), z = rdf(nv + 8);
                double tol = 0.5 + 1e-4 * (fabs(x) + fabs(y) + fabs(z));
                rec_eval(ent->rec, ca[k], cb[k], &p, NULL, NULL);
                if (!(fabs(p.x - x) <= tol && fabs(p.y - y) <= tol && fabs(p.z - z) <= tol)) { ps.corner++; return 0; }
            }
            if (moved) {
                const float *d = L->duv + 128u * e.pidx;
                for (k = 0; k < 64; k++) {
                    float u = rdf(ent->vbuf + VSZ * k + 0x0C) + d[2 * k], v = rdf(ent->vbuf + VSZ * k + 0x1C) + d[2 * k + 1];
                    memcpy(ent->vbuf + VSZ * k + 0x0C, &u, 4);
                    memcpy(ent->vbuf + VSZ * k + 0x1C, &v, 4);
                }
            }
            if (clampm) {
                /* the patch's own tile, moved to [0, 1] (a whole repeat: the image is
                 * the same), then sampled with clamp on that axis: the filter's
                 * footprint at the edge no longer reaches the far side of the texture */
                for (k = 0; k < 64; k++) {
                    float u = rdf(ent->vbuf + VSZ * k + 0x0C) - (float)shift[0], v = rdf(ent->vbuf + VSZ * k + 0x1C) - (float)shift[1];
                    if (clampm & 1) memcpy(ent->vbuf + VSZ * k + 0x0C, &u, 4);
                    if (clampm & 2) memcpy(ent->vbuf + VSZ * k + 0x1C, &v, 4);
                }
            }
            ent->ebuilt = 0;
            if (!dbg) { ent->gen = e.lvl; ent->pidx = e.pidx; }
        }
    }
    if (clampm) ps.clamped++;
    memset(out, 0, sizeof *out);
    if (clampm) {
        uint32_t st;
        for (st = 0; st < 4; st += 2) {     /* t0 snow and t2 normal map: same coordinates */
            uint32_t ad = pgraph_d3d11_tex_address((int)st);
            if (clampm & 1) ad = (ad & ~0xFu) | 3u;
            if (clampm & 2) ad = (ad & ~0xF00u) | 0x300u;
            out->address[st] = ad;
        }
    }
    out->indices = ent->idx;
    out->count = 112;
    out->host = ent->vbuf;
    out->size = sizeof ent->vbuf;
    out->base = offset[0] & 0x0FFFFFFFu;
    out->span = VSZ;
    ps.subst++;
    if (blend) {
        /* fade into the neighbour's textures along each such edge: alpha 1/2 on
         * the edge, 0 at the band's end. The edges that follow one another with
         * the same textures and address words are one draw (their triangles in
         * the same order: the same image) */
        struct { int ed; uint32_t o0, f0, o2, f2, ad[4], eff[4]; } bc[4];
        int ed, nbc = 0, i;
        uint32_t cat = 0;
        for (ed = 0; ed < 4; ed++) {
            int32_t pq = L->partner[e.pidx * 4u + (uint32_t)ed];
            uint32_t o0, f0, o2, f2, st;
            int full = L->full[e.pidx * 4u + (uint32_t)ed];
            int cmask = 0;                                         /* XBOX_TERRAIN_CLAMP: the neighbour's clamped axes */
            double qshift[2] = { 0.0, 0.0 };
            if (s_clamp && pq >= 0) {
                corners_t cq;
                rec_corners(REC(L, (uint32_t)pq >> 2), cq);
                cmask = clamp_axes(L, (uint32_t)pq >> 2, cq, qshift);
            }
            if (pq >= 0 && (full || s_match != 0) &&
                !tex_similar(rds(ent->rec + 0x1C0), rds(REC(L, (uint32_t)pq >> 2) + 0x1C0))) continue;
            if (!(L->cont[e.pidx * 4u + (uint32_t)ed] || full) || pq < 0 ||
                !tex_find(0, rds(REC(L, (uint32_t)pq >> 2) + 0x1C0), &o0, &f0) ||
                !tex_find(1, rds(REC(L, (uint32_t)pq >> 2) + 0x1C2), &o2, &f2)) continue;
            if (!(ent->ebuilt & (1u << ed)) || ent->ecmask[ed] != cmask ||
                ent->eqshift[ed][0] != qshift[0] || ent->eqshift[ed][1] != qshift[1])
                sc_edge(ent, L, e.pidx, ed, full, cmask, qshift, clampm, shift);
            if (!ent->enb[ed]) continue;
            bc[nbc].ed = ed;
            bc[nbc].o0 = o0; bc[nbc].f0 = f0; bc[nbc].o2 = o2; bc[nbc].f2 = f2;
            for (st = 0; st < 4; st++) {
                uint32_t ad = pgraph_d3d11_tex_address((int)st);
                bc[nbc].ad[st] = 0;
                if (cmask && (st == 0 || st == 2)) {
                    if (cmask & 1) ad = (ad & ~0xFu) | 3u;
                    if (cmask & 2) ad = (ad & ~0xF00u) | 0x300u;
                    bc[nbc].ad[st] = ad;
                }
                bc[nbc].eff[st] = ad;                       /* the word the draw samples with */
            }
            nbc++;
            ps.blends++;
        }
        for (i = 0; i < nbc; ) {
            int j = i + 1, g;
            PgraphDrawOverlay *ov;
            while (j < nbc && bc[j].o0 == bc[i].o0 && bc[j].f0 == bc[i].f0 && bc[j].o2 == bc[i].o2 &&
                   bc[j].f2 == bc[i].f2 && !memcmp(bc[j].eff, bc[i].eff, sizeof bc[i].eff))
                j++;
            if (out->n_overlay >= PGRAPH_SUBST_OVERLAYS) break;
            ov = &out->overlay[out->n_overlay++];
            if (j - i == 1) {
                ov->indices = ent->bl[bc[i].ed];
                ov->count = ent->enb[bc[i].ed];
            } else {
                ov->indices = s_cat + cat;
                ov->count = 0;
                for (g = i; g < j; g++) {
                    memcpy(s_cat + cat, ent->bl[bc[g].ed], 4u * ent->enb[bc[g].ed]);
                    cat += ent->enb[bc[g].ed];
                    ov->count += ent->enb[bc[g].ed];
                }
            }
            ov->blend = 1;
            ov->weights = WALT - 256u;      /* edge ed's alphas at WALT + 256 ed, its vertices from 64 (1 + ed) */
            ov->tex_offset[0] = bc[i].o0; ov->tex_format[0] = bc[i].f0;
            ov->tex_offset[2] = bc[i].o2; ov->tex_format[2] = bc[i].f2;
            memcpy(ov->address, bc[i].ad, sizeof ov->address);
            ps.overlays++;
            i = j;
        }
    }
    if (!dbg) return 1;

    if (dbg == 2) {
        /* debug lines: each edge's kind as a thin line on the edge (the last
         * eighth of a ramp over the first row of cells); other texture in cyan
         * when the placement follows, in blue when it does not */
        static const float lc[SEAM_KINDS + 1][4] = {
            { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 1.0f, 0.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 0.0f, 1.0f },
            { 1.0f, 0.1f, 0.1f, 1.0f }, { 0.0f, 0.9f, 1.0f, 1.0f }, { 1.0f, 0.55f, 0.0f, 1.0f } };
        static const float blue[4] = { 0.15f, 0.25f, 1.0f, 1.0f };
        int ed;
        for (ed = 0; ed < 4; ed++) {
            int kd = L->kind[e.pidx * 4u + (uint32_t)ed];
            uint32_t nb = 0, w;
            float *wt = (float *)(ent->vbuf + WEIGHTS + 256u * (uint32_t)ed);
            PgraphDrawOverlay *ov;
            if (kd == SEAM_OK && lm_step(L, e.pidx, ed) >= LM_STEP) kd = SEAM_KINDS;
            if (kd <= SEAM_OK || out->n_overlay >= PGRAPH_SUBST_OVERLAYS) continue;
            for (w = 0; w < 64; w++) wt[w] = edge_rows(ed, (int)w) == 0 ? 1.0f : 0.0f;
            for (m = 0; m + 2 < 112; m++) {
                uint32_t a0 = s_l8[m], a1 = s_l8[m + 1], a2 = s_l8[m + 2];
                uint32_t r0 = idx[m], r1 = idx[m + 1], r2 = idx[m + 2];
                int rmin;
                if (a0 == a1 || a1 == a2 || a0 == a2 || r0 == r1 || r1 == r2 || r0 == r2) continue;
                rmin = edge_rows(ed, (int)a0);
                if (edge_rows(ed, (int)a1) < rmin) rmin = edge_rows(ed, (int)a1);
                if (edge_rows(ed, (int)a2) < rmin) rmin = edge_rows(ed, (int)a2);
                if (rmin >= 1) continue;
                if (m & 1) { uint32_t tt = r0; r0 = r1; r1 = tt; }
                s_bl[ed][nb] = r0; s_bl[ed][nb + 1] = r1; s_bl[ed][nb + 2] = r2; nb += 3;
            }
            if (!nb) continue;
            ov = &out->overlay[out->n_overlay++];
            ov->indices = s_bl[ed];
            ov->count = nb;
            ov->blend = 1;
            ov->weights = WEIGHTS + 256u * (uint32_t)ed;
            memcpy(ov->rgba, kd == SEAM_TEXTURE && !L->cont[e.pidx * 4u + (uint32_t)ed] ? blue : lc[kd], sizeof ov->rgba);
        }
        return 1;
    }
    /* debug view: the strip's triangles in the cells along each tinted edge */
    {
        uint32_t t1off, t1fmt;
        int a = rds(ent->rec + 0x1C4), ek[4], ed;
        if (s_atl.gen != e.lvl) { memset(&s_atl, 0, sizeof s_atl); s_atl.gen = e.lvl; }
        pgraph_d3d11_tex_binding(1, &t1off, &t1fmt);
        if (a >= 0 && a < ATLN && t1off) { s_atl.off[a] = t1off; s_atl.fmt[a] = t1fmt; }
        for (ed = 0; ed < 4; ed++) {
            int kd = L->kind[e.pidx * 4u + (uint32_t)ed];
            if (kd == SEAM_OK && lm_step(L, e.pidx, ed) >= LM_STEP) kd = SEAM_KINDS;  /* lightmap */
            ek[ed] = kd;
        }
        if ((int32_t)e.pidx == s_rider && last_rider_gen != s_rider_gen) {
            last_rider_gen = s_rider_gen;
            rider_report(L, e.pidx);
        }
        for (m = 0; m + 2 < 112; m++) {
            uint32_t o0 = s_l8[m], o1 = s_l8[m + 1], o2 = s_l8[m + 2];
            uint32_t r0 = idx[m], r1 = idx[m + 1], r2 = idx[m + 2], ci, cj;
            int best = -1, bk = 0;
            if (o0 == o1 || o1 == o2 || o0 == o2 || r0 == r1 || r1 == r2 || r0 == r2) continue;
            if (m & 1) { uint32_t t = r0; r0 = r1; r1 = t; }
            ci = o0 % 8; if (o1 % 8 < ci) ci = o1 % 8; if (o2 % 8 < ci) ci = o2 % 8;
            cj = o0 / 8; if (o1 / 8 < cj) cj = o1 / 8; if (o2 / 8 < cj) cj = o2 / 8;
            if ((int32_t)e.pidx == s_rider) {
                uint32_t *o = s_ov[SEAM_KINDS + 1] + novl[SEAM_KINDS + 1];
                o[0] = r0; o[1] = r1; o[2] = r2; novl[SEAM_KINDS + 1] += 3;
            }
            for (ed = 0; ed < 4; ed++) {
                int on = ed == 0 ? cj == 0 : ed == 1 ? ci == 6 : ed == 2 ? cj == 6 : ci == 0;
                if (on && ek[ed] > SEAM_OK && prio[ek[ed]] > best) { best = prio[ek[ed]]; bk = ek[ed]; }
            }
            if (best > 0) {
                uint32_t *o = s_ov[bk] + novl[bk];
                o[0] = r0; o[1] = r1; o[2] = r2; novl[bk] += 3;
            }
        }
        for (m = 0; m < SEAM_KINDS; m++) {          /* the rider's patch first, its edges over it */
            k = m == 0 ? SEAM_KINDS + 1 : SEAM_STRETCH + (uint32_t)m - 1;
            if (novl[k] && out->n_overlay < PGRAPH_SUBST_OVERLAYS) {
                PgraphDrawOverlay *ov = &out->overlay[out->n_overlay++];
                ov->indices = s_ov[k];
                ov->count = novl[k];
                memcpy(ov->rgba, col[k], sizeof ov->rgba);
                ps.overlays++;
            }
        }
    }
    return 1;
}

void terrain_seams_init(void)
{
    /* default: every correction off, nothing hooked (as XBOX_TERRAIN_SEAMS=0,
     * which also overrides the other variables); the blend band is 0.75 m
     * when the blend is asked for (terrain_seams.h) */
    const char *e = getenv("XBOX_TERRAIN_SEAMS");
    int off = e && !strcmp(e, "0");
    s_b0 = 0; s_b1 = 0; s_blend = 0; s_width = 0.75; s_clamp = 0;
    if (e && !strcmp(e, "1")) s_b0 = s_b1 = 1;
    else if (e && !strcmp(e, "b0")) s_b0 = 1;
    else if (e && !strcmp(e, "b1")) s_b1 = 1;
    e = getenv("XBOX_TERRAIN_SEAMS_ROWS");
    if (e && e[0] >= '1' && e[0] <= '3' && !e[1]) s_rows = e[0] - '0';
    e = getenv("XBOX_TERRAIN_SEAMS_DEBUG");
    s_debug = e && (e[0] == '1' || e[0] == '2') && !e[1] ? e[0] - '0' : 0;
    e = getenv("XBOX_TERRAIN_SEAMS_LOG");
    s_log = e && e[0] == '1';
    e = getenv("XBOX_TERRAIN_BLEND");
    if (e && e[0] >= '0' && e[0] <= '2' && !e[1]) s_blend = e[0] - '0';
    e = getenv("XBOX_TERRAIN_BLEND_WIDTH");
    if (e && atof(e) > 0.0) s_width = atof(e);
    e = getenv("XBOX_TERRAIN_BLEND_TANGENT");
    if (e && e[0] == '0') s_blend_tangent = 0;
    e = getenv("XBOX_TERRAIN_BLEND_MATCH");
    if (e && !strcmp(e, "loose")) s_match = 0;
    else if (e && !strcmp(e, "same")) s_match = 2;
    e = getenv("XBOX_TERRAIN_BLEND_STRICT");     /* tests: mean,histogram,spread */
    if (e) sscanf(e, "%lf,%lf,%lf", &s_strict_mean, &s_strict_hist, &s_strict_sd);
    e = getenv("XBOX_TERRAIN_BLEND_SIMILAR");
    if (e && atof(e) > 0.0) s_similar = atof(e);
    e = getenv("XBOX_TERRAIN_BLEND_ROWS");
    if (e && e[0] >= '1' && e[0] <= '3' && !e[1]) s_blend_rows = e[0] - '0';
    e = getenv("XBOX_TERRAIN_CLAMP");
    if (e && e[0] >= '0' && e[0] <= '2' && !e[1]) s_clamp = e[0] - '0';
    if (off) { s_b0 = s_b1 = 0; s_blend = 0; s_clamp = 0; }
    g_terrain_seams = s_b0 || s_b1 || s_debug || s_blend || s_clamp;
    if (!g_terrain_seams) { fprintf(stderr, "[SEAMS] terrain seams: off\n"); fflush(stderr); return; }
    snake(s_l8, 8);
    pgraph_d3d11_set_draw_subst(subst);
    fprintf(stderr, "[SEAMS] terrain seams:%s%s (rows %d)%s", s_b0 ? " b0" : "", s_b1 ? " b1" : "", s_rows,
            s_debug ? ", debug view" : "");
    if (s_blend && s_width > 0.0) fprintf(stderr, ", texture blend over %.2f m%s", s_width,
                                          s_blend == 2 ? ", rotated / other placements too" : "");
    else if (s_blend) fprintf(stderr, ", texture blend over %d row(s)%s", s_blend_rows,
                              s_blend == 2 ? ", rotated / other placements too" : "");
    if (s_blend == 2 && !s_blend_tangent) fprintf(stderr, ", own tangents");
    if (s_clamp) fprintf(stderr, ", single-repeat axes clamped (%s)", s_clamp == 1 ? "all" : "where the neighbour does not continue");
    if (s_blend) fprintf(stderr, "%s", s_match == 0 ? ", loose texture match" : s_match == 2 ? ", same texture only" : ", strict texture match");
    fprintf(stderr, "\n");
    fflush(stderr);
}
