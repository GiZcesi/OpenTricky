/**
 * The pump's shaders built before they are drawn (see
 * d3d8_shprewarm.h).
 *
 * One lock covers the recipe files, the set of known recipes and the table
 * of objects built ahead. The pump takes it only on a miss of its own
 * tables (a new shader) and once at start; the thread holds it only to
 * insert an object, never while it compiles.
 */
#include "d3d8_internal.h"
#include "d3d8_shprewarm.h"
#include "d3d8_shcache.h"
#include "../nv2a/nv2a_psh.h"
#include "../nv2a/nv2a_vsh_cpu.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHPW_VERSION   2u
#define SHPW_KNOWN     8192            /* known recipes (power of two) */
#define SHPW_READY     4096            /* objects built ahead (power of two) */
#define SHPW_MAX_REC   4096            /* recipes kept in memory for the thread */
#define SHPW_REC_MAX   65536           /* largest record */
#define SHPW_FILE_MAX  (4u << 20)      /* bound of the learned file */
#define SHPW_PS_CAP    98304           /* as the pump's buffers */
#define SHPW_VS_CAP    262144

/* The words of Nv2aPshState that come from the title's pixel shader; the
 * fields after them are the translator's own state for the draw. */
#define SHPW_PS_WORDS  offsetof(Nv2aPshState, tex_enabled)

typedef struct {
    char     magic[4];
    uint32_t version, ps_size, insn_size, ps_words;
    uint64_t xbe_id;                    /* xbe_id() of the default.xbe the file was made against */
} ShpwHeader;
typedef struct { uint16_t type, zero; uint32_t size; } ShpwRecHead;

/* The given list's records (no byte of the game in them). */
typedef struct {
    uint32_t psdef;                     /* file offset of a D3DPIXELSHADERDEF in default.xbe */
    uint8_t  lm;                        /* the lightmap filter applies (as SHPW_PS's last byte) */
    uint8_t  off;                       /* bit s: the mode of texture stage s was off (0) in the draw, not
                                         * the definition's (the title turns a stage off per draw) */
    uint8_t  zero[2];
    uint64_t key;                       /* nv2a_psh_key of the rebuilt (normalized) state */
    /* then the Nv2aPshState bytes from SHPW_PS_WORDS on (our state) */
} ShpwPsRef;
typedef struct {
    uint32_t prog;                      /* file offset of the program's first instruction in default.xbe */
    uint8_t  count, pts;                /* instructions (the last one is FINAL); point list */
    uint8_t  t0z, zero;                 /* the terrain seams' blend overlay (vshcpu_t0z_append) */
    uint8_t  kind[16];                  /* NV2A_VSH_IN_* of each input, as the draw had them */
    uint64_t key;                       /* the pump's key of the rebuilt recipe (d3d8_nv2a.c vsh_key) */
} ShpwVsRef;

/* D3DPIXELSHADERDEF (the XDK's layout): where each register's words are. */
enum {
    PSD_ALPHA_IN = 0x00, PSD_ABCD = 0x20, PSD_EFG = 0x24, PSD_ALPHA_OUT = 0x68, PSD_RGB_IN = 0x88,
    PSD_RGB_OUT = 0xB4, PSD_COUNT = 0xD4, PSD_TEXMODES = 0xD8, PSD_INPUTTEX = 0xE0, PSD_SIZE = 0xF0
};

typedef struct { int type; int size; void *rec; } ShpwRec;
typedef struct { uint64_t key; int type; void *obj, *code; } ShpwReady;

static struct {
    int started;
    CRITICAL_SECTION lock;
    ShpwPsHlsl ps_hlsl;
    const uint8_t *xbe;                 /* the disc's default.xbe (main.c) */
    size_t xbe_size;
    uint64_t xbe_id;
    uint64_t known[SHPW_KNOWN];
    int nknown;
    ShpwRec recs[SHPW_MAX_REC];
    int nrecs;
    ShpwReady ready[SHPW_READY];
    char learned_path[1024];
    FILE *learned;                      /* opened for append at the first note */
    int learned_bad;                    /* cannot be written: no more tries */
    long learned_bytes;
    int from_list, from_learned, list_bad;
} P;
static volatile LONG s_ready_n;         /* objects waiting in P.ready */

/* ---- small helpers ---------------------------------------------------- */

static uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) { h ^= *b++; h *= 0x100000001B3ull; }
    return h;
}

static uint32_t sum32(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

static uint64_t rec_id(int type, const void *rec, int size)
{
    uint64_t h = fnv64(0xCBF29CE484222325ull, &type, sizeof type);
    h = fnv64(h, rec, (size_t)size);
    return h ? h : 1;
}

static void header_make(ShpwHeader *h)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "OTSR", 4);
    h->version = SHPW_VERSION;
    h->ps_size = (uint32_t)sizeof(Nv2aPshState);
    h->insn_size = (uint32_t)sizeof(vshcpu_insn);
    h->ps_words = (uint32_t)SHPW_PS_WORDS;
    h->xbe_id = P.xbe_id;
}

/* 1 if new (added), 0 if already known or the set is full. Under the lock. */
static int known_add(uint64_t id)
{
    unsigned i = (unsigned)(id ^ (id >> 31)) & (SHPW_KNOWN - 1), n;
    if (P.nknown >= SHPW_KNOWN / 2) return 0;
    for (n = 0; n < SHPW_KNOWN; n++, i = (i + 1) & (SHPW_KNOWN - 1)) {
        if (P.known[i] == id) return 0;
        if (!P.known[i]) { P.known[i] = id; P.nknown++; return 1; }
    }
    return 0;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* ---- references into default.xbe -> recipes ------------------------------ */

/* A pixel-shader reference -> the SHPW_PS recipe it stands for (out: a
 * normalized Nv2aPshState and the lightmap flag); 0 if it does not rebuild
 * to its key. The pump fills the register words of the stages it uses only
 * (nv2a_pgraph_d3d11.c draw_program), so the others stay 0 here too. */
static int ps_from_ref(const uint8_t *rec, int size, uint8_t *out)
{
    ShpwPsRef r;
    Nv2aPshState st;
    const uint8_t *d;
    int s, n;
    if (size != (int)(sizeof r + sizeof st - SHPW_PS_WORDS)) return 0;
    memcpy(&r, rec, sizeof r);
    if (!P.xbe || r.psdef > P.xbe_size || P.xbe_size - r.psdef < PSD_SIZE || r.lm > 1 || r.off > 15
        || r.zero[0] || r.zero[1])
        return 0;
    d = P.xbe + r.psdef;
    memset(&st, 0, sizeof st);
    memcpy((uint8_t *)&st + SHPW_PS_WORDS, rec + sizeof r, sizeof st - SHPW_PS_WORDS);
    st.combiner_control = rd32(d + PSD_COUNT);
    st.shader_stage_program = rd32(d + PSD_TEXMODES);
    for (s = 0; s < 4; s++)
        if (r.off & (1u << s)) st.shader_stage_program &= ~(0x1Fu << (5 * s));
    st.other_stage_input = rd32(d + PSD_INPUTTEX);
    st.final0 = rd32(d + PSD_ABCD);
    st.final1 = rd32(d + PSD_EFG);
    n = (int)(st.combiner_control & 0xFF);
    if (n > 8) n = 8;
    for (s = 0; s < n; s++) {
        st.rgb_in[s] = rd32(d + PSD_RGB_IN + 4 * s);
        st.rgb_out[s] = rd32(d + PSD_RGB_OUT + 4 * s);
        st.alpha_in[s] = rd32(d + PSD_ALPHA_IN + 4 * s);
        st.alpha_out[s] = rd32(d + PSD_ALPHA_OUT + 4 * s);
    }
    if (nv2a_psh_key(&st) != r.key) return 0;
    memcpy(out, &st, sizeof st);
    out[sizeof st] = r.lm;
    return (int)sizeof st + 1;
}

/* A vertex-program reference -> the SHPW_VS recipe it stands for; its size,
 * or 0 if it does not rebuild to its key. */
static int vs_from_ref(const uint8_t *rec, int size, uint8_t *out, int cap)
{
    ShpwVsRef r;
    uint64_t key = 0;
    int n;
    if (size != (int)sizeof r) return 0;
    memcpy(&r, rec, sizeof r);
    if (!P.xbe || !r.count || r.count > VSHCPU_SLOTS || r.pts > 1 || r.t0z > 1 || r.zero || r.prog > P.xbe_size
        || (P.xbe_size - r.prog) / 16 < (size_t)r.count)
        return 0;
    {
        uint32_t words[VSHCPU_SLOTS][4];
        memcpy(words, P.xbe + r.prog, (size_t)r.count * 16);
        n = d3d8_nv2a_vs_recipe_make((const uint32_t (*)[4])words, r.count, r.t0z, r.kind, r.pts, out, cap, &key);
    }
    return n > 0 && key == r.key ? n : 0;
}

/* ---- files -------------------------------------------------------------- */

/* Keeps a recipe for the thread if it is new. Under no lock: before the
 * thread and the pump's notes. */
static int keep(int type, const void *rec, int size)
{
    void *copy;
    if (P.nrecs >= SHPW_MAX_REC || !known_add(rec_id(type, rec, size))) return 0;
    copy = malloc((size_t)size);
    if (!copy) return 0;
    memcpy(copy, rec, (size_t)size);
    P.recs[P.nrecs].type = type;
    P.recs[P.nrecs].size = size;
    P.recs[P.nrecs].rec = copy;
    P.nrecs++;
    return 1;
}

/* Reads the records of path into P.recs (new ones only): from the given
 * list (`given`) references only, from the learned file raw recipes only.
 * -1: no such file, -2: another build or disc, or not a recipe file; else the
 * number of records kept; *bytes: the length of the valid part, *tail: 1 if
 * more bytes follow it; *bad: references that did not rebuild. */
static int read_file(const char *path, int given, long *bytes, int *tail, int *bad)
{
    FILE *f = fopen(path, "rb");
    ShpwHeader h, want;
    ShpwRecHead rh;
    int kept = 0;
    uint8_t *buf, *rec;
    uint32_t sum;

    if (bytes) *bytes = 0;
    if (tail) *tail = 0;
    if (bad) *bad = 0;
    if (!f) return -1;
    header_make(&want);
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(&h, &want, sizeof h) != 0 || (given && !P.xbe)) {
        fclose(f);
        return -2;
    }
    if (bytes) *bytes = (long)sizeof h;
    buf = (uint8_t *)malloc(SHPW_REC_MAX);
    rec = (uint8_t *)malloc(SHPW_REC_MAX);
    while (buf && rec && fread(&rh, sizeof rh, 1, f) == 1) {
        int type = rh.type, size = (int)rh.size;
        if (rh.zero || !rh.size || rh.size > SHPW_REC_MAX) break;
        if (given ? (type != SHPW_PS_REF && type != SHPW_VS_REF) : (type != SHPW_PS && type != SHPW_VS)) break;
        if (fread(buf, 1, rh.size, f) != rh.size || fread(&sum, sizeof sum, 1, f) != 1) break;
        if (sum != sum32(buf, rh.size)) break;
        if (bytes) *bytes += (long)(sizeof rh + rh.size + sizeof sum);
        if (type == SHPW_PS_REF) {
            size = ps_from_ref(buf, size, rec);
            type = SHPW_PS;
        } else if (type == SHPW_VS_REF) {
            size = vs_from_ref(buf, size, rec, SHPW_REC_MAX);
            type = SHPW_VS;
        } else {
            memcpy(rec, buf, rh.size);
        }
        if (size <= 0) {
            if (bad) ++*bad;
            continue;
        }
        kept += keep(type, rec, size);
    }
    if (tail && bytes && fseek(f, 0, SEEK_END) == 0) *tail = ftell(f) != *bytes;
    free(buf);
    free(rec);
    fclose(f);
    return kept;
}

/* Cuts the learned file to its valid part (a crash during a write), so the
 * records appended next can be read. */
static void cut_file(const char *path, long len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER at;
    if (h == INVALID_HANDLE_VALUE) return;
    at.QuadPart = len;
    if (SetFilePointerEx(h, at, NULL, FILE_BEGIN)) SetEndOfFile(h);
    CloseHandle(h);
}

static void list_path(char *out, size_t cap)
{
    const char *e = getenv("XBOX_SHADER_PREWARM_LIST");
    char exe[MAX_PATH], *s;
    DWORD n;
    out[0] = '\0';
    if (e) { snprintf(out, cap, "%s", e); return; }      /* "" = no list */
    n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (!n || n >= sizeof exe) return;
    s = strrchr(exe, '\\');
    if (!s) return;
    s[1] = '\0';
    snprintf(out, cap, "%sshaders.otsr", exe);
}

/* ---- the objects built ahead ---------------------------------------------- */

static void obj_release(int type, void *obj, void *code)
{
    if (type == SHPW_PS && obj) ID3D11PixelShader_Release((ID3D11PixelShader *)obj);
    if (type == SHPW_VS && obj) ID3D11VertexShader_Release((ID3D11VertexShader *)obj);
    if (code) ID3D10Blob_Release((ID3D10Blob *)code);
}

/* Open addressing without deletion: a lookup stops at a slot never used
 * (key 0); a taken entry keeps its key with a NULL object, so the chains
 * through it stay whole, and a later insert may fill it again. */
static ShpwReady *ready_find(int type, uint64_t key)
{
    unsigned i = (unsigned)(key ^ (key >> 29) ^ (unsigned)type) & (SHPW_READY - 1), n;
    for (n = 0; n < SHPW_READY; n++, i = (i + 1) & (SHPW_READY - 1)) {
        ShpwReady *r = &P.ready[i];
        if (!r->key) return NULL;
        if (r->obj && r->key == key && r->type == type) return r;
    }
    return NULL;
}

/* 1 = kept; 0 = already there or no room (the object is released). */
static int ready_put(int type, uint64_t key, void *obj, void *code)
{
    int ok = 0;
    EnterCriticalSection(&P.lock);
    if (key && !ready_find(type, key)) {
        unsigned i = (unsigned)(key ^ (key >> 29) ^ (unsigned)type) & (SHPW_READY - 1), n;
        for (n = 0; n < SHPW_READY; n++, i = (i + 1) & (SHPW_READY - 1))
            if (!P.ready[i].obj) {
                P.ready[i].key = key; P.ready[i].type = type; P.ready[i].obj = obj; P.ready[i].code = code;
                InterlockedIncrement(&s_ready_n);
                ok = 1;
                break;
            }
    }
    LeaveCriticalSection(&P.lock);
    if (!ok) obj_release(type, obj, code);
    return ok;
}

static int take(int type, uint64_t key, void **obj, void **code)
{
    ShpwReady *r;
    int ok = 0;
    if (!P.started || InterlockedCompareExchange(&s_ready_n, 0, 0) == 0) return 0;
    EnterCriticalSection(&P.lock);
    r = ready_find(type, key);
    if (r) {
        *obj = r->obj;
        if (code) *code = r->code;
        else if (r->code) ID3D10Blob_Release((ID3D10Blob *)r->code);
        r->obj = NULL;
        r->code = NULL;
        InterlockedDecrement(&s_ready_n);
        ok = 1;
    }
    LeaveCriticalSection(&P.lock);
    return ok;
}

void *shpw_take_ps(uint64_t key)
{
    void *obj = NULL;
    return take(SHPW_PS, key, &obj, NULL) ? obj : NULL;
}

int shpw_take_vs(uint64_t key, void **vs, void **code)
{
    return take(SHPW_VS, key, vs, code);
}

/* ---- the thread ------------------------------------------------------------ */

static DWORD WINAPI prewarm_thread(LPVOID arg)
{
    char *ps_buf = (char *)malloc(SHPW_PS_CAP), *vs_buf = (char *)malloc(SHPW_VS_CAP);
    double t0 = d3d8_ms_since_start(), t1;
    int i, nps = 0, nvs = 0, hits = 0, failed = 0, len;
    (void)arg;
    d3d8_nv2a_prewarm_fixed();
    for (i = 0; ps_buf && vs_buf && i < P.nrecs; i++) {
        ShpwRec *r = &P.recs[i];
        uint64_t key = 0;
        int hit = 0;
        if (r->type == SHPW_PS) {
            void *ps;
            len = P.ps_hlsl ? P.ps_hlsl(r->rec, r->size, &key, ps_buf, SHPW_PS_CAP) : 0;
            ps = len > 0 ? d3d8_nv2a_build_ps(ps_buf, len, &hit) : NULL;
            if (!ps) { failed++; continue; }
            if (ready_put(SHPW_PS, key, ps, NULL)) nps++;
        } else {
            void *vs = NULL, *code = NULL;
            len = d3d8_nv2a_vs_recipe_hlsl(r->rec, r->size, &key, vs_buf, SHPW_VS_CAP);
            if (len <= 0 || !d3d8_nv2a_build_vs(vs_buf, len, &vs, &code, &hit)) { failed++; continue; }
            if (ready_put(SHPW_VS, key, vs, code)) nvs++;
        }
        hits += hit;
    }
    t1 = d3d8_ms_since_start();
    fprintf(stderr, "[SHPW] ready: %d pixel shaders, %d vertex programs (%d compiled, %d from the cache, "
            "%d failed) in %.0f ms (t+%.0f ms)\n", nps, nvs, nps + nvs - hits, hits, failed, t1 - t0, t1);
    free(ps_buf);
    free(vs_buf);
    for (i = 0; i < P.nrecs; i++) { free(P.recs[i].rec); P.recs[i].rec = NULL; }
    return 0;
}

/* ---- interface -------------------------------------------------------------- */

int shpw_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XBOX_SHADER_PREWARM");
        on = !(e && e[0] == '0') && shc_env_on();       /* no disk cache: the previous path */
    }
    return on;
}

/* The id of a default.xbe: FNV-1a 64 of its first 4 KB (header, signature
 * and certificate: one per build of the title). */
static uint64_t xbe_id(const uint8_t *x, size_t n)
{
    return fnv64(0xCBF29CE484222325ull ^ 0x0715u, x, n < 4096 ? n : 4096);
}

void shpw_set_xbe(const void *xbe, size_t size)
{
    if (!xbe || size < 4096) return;
    P.xbe = (const uint8_t *)xbe;
    P.xbe_size = size;
    P.xbe_id = xbe_id(P.xbe, size);
}

void shpw_start(ShpwPsHlsl ps_hlsl)
{
    char list[1024];
    const char *dir;
    int n_list, n_learned, tail = 0, bad = 0;
    long lb = 0;
    double t0 = d3d8_ms_since_start();
    HANDLE th;

    if (P.started || !shpw_on()) return;
    InitializeCriticalSection(&P.lock);
    P.ps_hlsl = ps_hlsl;
    dir = getenv("XBOX_SHADER_CACHE_DIR");
    if (!dir || !dir[0]) dir = "cache" "/" "shaders";
    /* Not "*.bin": the cache's own start-up scan deletes every .bin of the
     * folder that is not one of its entries (d3d8_shcache.c scan_one). */
    snprintf(P.learned_path, sizeof P.learned_path, "%s%crecipes.otsr", dir,
             strchr(dir, '\\') ? '\\' : '/');
    list_path(list, sizeof list);
    n_list = list[0] ? read_file(list, 1, NULL, NULL, &bad) : -1;
    n_learned = read_file(P.learned_path, 0, &lb, &tail, NULL);
    if (n_learned == -2) {                  /* another build's: started again at the first note */
        remove(P.learned_path);
        lb = 0;
    } else if (n_learned >= 0 && tail) {
        cut_file(P.learned_path, lb);
    }
    P.learned_bytes = lb;
    P.from_list = n_list > 0 ? n_list : 0;
    P.from_learned = n_learned > 0 ? n_learned : 0;
    P.list_bad = bad;
    P.started = 1;
    fprintf(stderr, "[SHPW] XBOX_SHADER_PREWARM=1: list %s (%s: %d, %d not rebuilt), learned %s (%s: %d); "
            "%d recipes, read in %.1f ms (t+%.0f ms)\n",
            list[0] ? list : "(none)",
            n_list == -1 ? "absent" : n_list == -2 ? "other build or disc, ignored" : "read", P.from_list, bad,
            P.learned_path, n_learned == -1 ? "absent" : n_learned == -2 ? "other build, started again" : "read",
            P.from_learned, P.nrecs, d3d8_ms_since_start() - t0, d3d8_ms_since_start());
    th = CreateThread(NULL, 0, prewarm_thread, NULL, 0, NULL);
    if (th) {
        SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(th);
    } else {
        int i;
        for (i = 0; i < P.nrecs; i++) { free(P.recs[i].rec); P.recs[i].rec = NULL; }
    }
}

void shpw_note(int type, const void *rec, int size)
{
    ShpwRecHead rh;
    uint32_t sum;
    if (!P.started || size <= 0 || size > SHPW_REC_MAX) return;
    EnterCriticalSection(&P.lock);
    if (!known_add(rec_id(type, rec, size)) || P.learned_bad
        || P.learned_bytes + (long)(sizeof rh + (size_t)size + sizeof sum) > (long)SHPW_FILE_MAX) {
        LeaveCriticalSection(&P.lock);
        return;
    }
    if (!P.learned) {
        P.learned = fopen(P.learned_path, "ab");
        if (P.learned && P.learned_bytes == 0) {
            ShpwHeader h;
            header_make(&h);
            if (fwrite(&h, sizeof h, 1, P.learned) != 1) { fclose(P.learned); P.learned = NULL; }
            else P.learned_bytes = (long)sizeof h;
        }
        if (!P.learned) { P.learned_bad = 1; LeaveCriticalSection(&P.lock); return; }
    }
    memset(&rh, 0, sizeof rh);
    rh.type = (uint16_t)type;
    rh.size = (uint32_t)size;
    sum = sum32(rec, (size_t)size);
    if (fwrite(&rh, sizeof rh, 1, P.learned) != 1 || fwrite(rec, 1, (size_t)size, P.learned) != (size_t)size
        || fwrite(&sum, sizeof sum, 1, P.learned) != 1 || fflush(P.learned) != 0)
        P.learned_bad = 1;
    else
        P.learned_bytes += (long)(sizeof rh + (size_t)size + sizeof sum);
    LeaveCriticalSection(&P.lock);
}
