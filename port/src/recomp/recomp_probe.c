/* recomp_probe.c -- see recomp_probe.h for why this exists. */

#include "recomp_probe.h"
#include "recomp_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

volatile int g_probe_armed = 0;

#define MAX_PROBES   64
#define MAX_SHOW     16
#define EXPR_LEN     64
#define HASH_SLOTS 4096          /* power of two, >> MAX_PROBES so probing is short */

typedef struct {
    uint32_t addr;
    char     when[EXPR_LEN];     /* empty = unconditional */
    char     show[MAX_SHOW][EXPR_LEN];
    int      nshow;
    int      limit;
    int      hits;
    int      used;
} probe_t;

static probe_t  g_probes[MAX_PROBES];
static int      g_nprobes;
/* addr+1 is stored so that 0 can mean "empty slot". */
static uint32_t g_slots[HASH_SLOTS];

static unsigned slot_of(uint32_t a)
{
    return (unsigned)((a * 2654435761u) >> 20) & (HASH_SLOTS - 1);
}

static void hash_insert(uint32_t a)
{
    unsigned s = slot_of(a);
    for (unsigned i = 0; i < HASH_SLOTS; i++) {
        unsigned k = (s + i) & (HASH_SLOTS - 1);
        if (g_slots[k] == 0 || g_slots[k] == a + 1) { g_slots[k] = a + 1; return; }
    }
}

static int hash_has(uint32_t a)
{
    unsigned s = slot_of(a);
    for (unsigned i = 0; i < HASH_SLOTS; i++) {
        unsigned k = (s + i) & (HASH_SLOTS - 1);
        uint32_t v = g_slots[k];
        if (v == 0) return 0;
        if (v == a + 1) return 1;
    }
    return 0;
}

/* ---------------- tiny expression evaluator ----------------
 * EXPR := TERM (('+'|'-') TERM)*
 * TERM := reg | 0xhex | decimal | '[' EXPR ']'
 */

typedef struct { const char *p; int bad; } scan_t;

static void skip_ws(scan_t *s) { while (*s->p == ' ' || *s->p == '\t') s->p++; }

static uint32_t eval_expr(scan_t *s);

static uint32_t eval_term(scan_t *s)
{
    skip_ws(s);
    if (*s->p == '[') {
        s->p++;
        uint32_t a = eval_expr(s);
        skip_ws(s);
        if (*s->p != ']') { s->bad = 1; return 0; }
        s->p++;
        if (s->bad) return 0;
        return MEM32(a);
    }
    if (s->p[0] == '0' && (s->p[1] == 'x' || s->p[1] == 'X')) {
        char *end = NULL;
        uint32_t v = (uint32_t)strtoul(s->p, &end, 16);
        s->p = end;
        return v;
    }
    if (s->p[0] >= '0' && s->p[0] <= '9') {
        char *end = NULL;
        uint32_t v = (uint32_t)strtoul(s->p, &end, 10);
        s->p = end;
        return v;
    }
    {
        static const char *names[8] = { "eax", "ecx", "edx", "esp", "ebx", "esi", "edi", "ebp" };
        uint32_t *slots[8] = { &g_eax, &g_ecx, &g_edx, &g_esp, &g_ebx, &g_esi, &g_edi, &g_seh_ebp };
        for (int i = 0; i < 8; i++)
            if (!strncmp(s->p, names[i], 3)) { s->p += 3; return *slots[i]; }
    }
    s->bad = 1;
    return 0;
}

static uint32_t eval_expr(scan_t *s)
{
    uint32_t v = eval_term(s);
    for (;;) {
        if (s->bad) return 0;
        skip_ws(s);
        if (*s->p == '+')      { s->p++; v += eval_term(s); }
        else if (*s->p == '-') { s->p++; v -= eval_term(s); }
        /* Bitwise AND, left to right like +/-: `[esp+8]&1==1` finds odd
         * pointers, `eax&0xF!=0` misaligned ones. */
        else if (*s->p == '&') { s->p++; v &= eval_term(s); }
        else return v;
    }
}

static uint32_t eval(const char *e, int *bad)
{
    scan_t s;
    uint32_t v;
    s.p = e; s.bad = 0;
    v = eval_expr(&s);
    if (bad) *bad = s.bad;
    return v;
}

static int eval_cond(const char *c)
{
    /* Split on the first comparison operator at bracket depth 0. */
    int depth = 0;
    const char *op = NULL;
    const char *p;
    char lhs[EXPR_LEN];
    char o[3];
    const char *rhs;
    size_t n;
    int b1 = 0, b2 = 0;
    uint32_t a, b;

    for (p = c; *p; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') depth--;
        else if (depth == 0 && (*p == '=' || *p == '!' || *p == '<' || *p == '>')) { op = p; break; }
    }
    if (!op) return 0;

    n = (size_t)(op - c);
    if (n >= sizeof(lhs)) return 0;
    memcpy(lhs, c, n);
    lhs[n] = 0;

    o[0] = op[0]; o[1] = 0; o[2] = 0;
    rhs = op + 1;
    if (op[1] == '=') { o[1] = '='; rhs = op + 2; }

    a = eval(lhs, &b1);
    b = eval(rhs, &b2);
    if (b1 || b2) return 0;

    if (!strcmp(o, "==")) return a == b;
    if (!strcmp(o, "!=")) return a != b;
    if (!strcmp(o, "<"))  return a <  b;
    if (!strcmp(o, ">"))  return a >  b;
    if (!strcmp(o, "<=")) return a <= b;
    if (!strcmp(o, ">=")) return a >= b;
    return 0;
}

/* ---------------- hit path ---------------- */

/* XBOX_ESP_GUARD=1: at every label, check this thread's guest esp is inside
 * RAM, and report the first label where it is not together with the label
 * the same thread passed just before -- the stack pointer broke between
 * those two. Kernel calls are too far apart to catch it: once a
 * thread's esp reached the top of the APU aperture (0xFE87FFFC) inside one
 * frame callback without making a single kernel call, so every push became
 * an APU register write. Costs a call per label while enabled. */
static int g_esp_guard = 0;
static __thread uint32_t t_prev_label = 0;

static void esp_guard_check(uint32_t addr)
{
    static volatile LONG reported = 0;
    if ((g_esp >= 0x08000000u || g_esp < 0x00010000u) &&
        InterlockedExchange(&reported, 1) == 0) {
        fprintf(stderr, "[ESPGUARD] guest esp=0x%08X at loc_%08X (host tid %lu); "
                "previous label on this thread loc_%08X, ebp-bridge g_seh_ebp=0x%08X\n",
                g_esp, addr, GetCurrentThreadId(), t_prev_label, g_seh_ebp);
        fflush(stderr);
    }
    t_prev_label = addr;
}

void recomp_probe_hit(uint32_t addr)
{
    int i;

    if (g_esp_guard) esp_guard_check(addr);
    if (!hash_has(addr)) return;

    for (i = 0; i < g_nprobes; i++) {
        probe_t *p = &g_probes[i];
        char line[1024];
        int n;
        int k;

        if (!p->used || p->addr != addr) continue;
        if (p->hits >= p->limit) continue;
        if (p->when[0] && !eval_cond(p->when)) continue;
        p->hits++;

        /* The thread id is printed unconditionally: guest registers are
         * __thread, so "which thread" is the first question whenever a probe
         * reports a value that could not have come from the expected caller. */
        {   /* ms since the first hit (QPC): latency questions need a clock */
            static LARGE_INTEGER f, t0;
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            if (!f.QuadPart) { QueryPerformanceFrequency(&f); t0 = q; }
            n = snprintf(line, sizeof(line), "  [PROBE] 0x%08X t%lu @%.1fms",
                         addr, (unsigned long)GetCurrentThreadId(),
                         (double)(q.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
        }
        for (k = 0; k < p->nshow && n > 0 && (size_t)n < sizeof(line); k++) {
            int bad = 0;
            uint32_t v;
            if (!strcmp(p->show[k], "bt")) {
                /* The host call stack is the guest call chain: translated
                 * functions call each other directly, while the guest
                 * return slot holds a dummy 0. Resolve the addresses with
                 * `addr2line -f -e "SSX Tricky.exe"` (image base 0x140000000). */
                void *fr[14];
                unsigned short c = (unsigned short)CaptureStackBackTrace(1, 14, fr, NULL), j;
                uintptr_t mb = (uintptr_t)GetModuleHandleW(NULL);
                n += snprintf(line + n, sizeof(line) - (size_t)n, " bt=");
                for (j = 0; j < c && n > 0 && (size_t)n < sizeof(line); j++)
                    n += snprintf(line + n, sizeof(line) - (size_t)n, "%s0x%llX", j ? "," : "",
                                  (unsigned long long)(0x140000000ULL + ((uintptr_t)fr[j] - mb)));
                continue;
            }
            v = eval(p->show[k], &bad);
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=%s0x%08X",
                          p->show[k], bad ? "?" : "", v);
        }
        if (p->nshow == 0 && n > 0 && (size_t)n < sizeof(line)) {
            snprintf(line + n, sizeof(line) - (size_t)n,
                     " eax=0x%08X ecx=0x%08X edx=0x%08X ebx=0x%08X esp=0x%08X esi=0x%08X edi=0x%08X",
                     g_eax, g_ecx, g_edx, g_ebx, g_esp, g_esi, g_edi);
        }
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
        return;
    }
}

/* ---------------- arming ---------------- */

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

void recomp_probe_clear(void)
{
    memset(g_probes, 0, sizeof(g_probes));
    memset(g_slots,  0, sizeof(g_slots));
    g_nprobes = 0;
    g_probe_armed = 0;
}

int recomp_probe_arm(const char *spec)
{
    char buf[4096];
    int armed = 0;
    char *save1 = NULL;
    char *one;

    if (!spec || !*spec) return 0;
    snprintf(buf, sizeof(buf), "%s", spec);

    for (one = strtok_r(buf, ";", &save1); one; one = strtok_r(NULL, ";", &save1)) {
        probe_t *p;
        char *f[4];
        int nf = 1;
        char *q;
        char *addr;
        char *end = NULL;

        one = trim(one);
        if (!*one) continue;
        if (g_nprobes >= MAX_PROBES) return -1;

        /* Split on '|' by hand -- strtok collapses empty fields, and an omitted
         * `when` (ADDR||show) must stay distinguishable from an omitted show. */
        f[0] = one; f[1] = NULL; f[2] = NULL; f[3] = NULL;
        for (q = one; *q && nf < 4; q++)
            if (*q == '|') { *q = 0; f[nf++] = q + 1; }

        p = &g_probes[g_nprobes];
        memset(p, 0, sizeof(*p));

        addr = trim(f[0]);
        p->addr = (uint32_t)strtoul(addr, &end, 0);
        if (end == addr) return -1;

        if (f[1]) snprintf(p->when, sizeof(p->when), "%s", trim(f[1]));
        if (f[2]) {
            char *save2 = NULL;
            char *sh;
            for (sh = strtok_r(f[2], ",", &save2); sh && p->nshow < MAX_SHOW;
                 sh = strtok_r(NULL, ",", &save2)) {
                sh = trim(sh);
                if (*sh) snprintf(p->show[p->nshow++], EXPR_LEN, "%s", sh);
            }
        }
        p->limit = f[3] ? atoi(trim(f[3])) : 4;
        if (p->limit <= 0) p->limit = 4;
        p->used = 1;

        hash_insert(p->addr);
        g_nprobes++;
        armed++;
    }

    if (armed) g_probe_armed = 1;
    return armed;
}

void recomp_probe_list(char *out, size_t cap)
{
    size_t n = 0;
    int i;
    n += (size_t)snprintf(out + n, cap - n, "%d probe(s) armed\n", g_nprobes);
    for (i = 0; i < g_nprobes && n < cap; i++) {
        probe_t *p = &g_probes[i];
        n += (size_t)snprintf(out + n, cap - n,
                              "  0x%08X hits=%d/%d when='%s' show=%d\n",
                              p->addr, p->hits, p->limit, p->when, p->nshow);
    }
}

void recomp_probe_init_from_env(void)
{
    const char *s = getenv("XBOX_PROBE");
    const char *g = getenv("XBOX_ESP_GUARD");
    int n;
    if (g && g[0] == '1') {
        g_esp_guard = 1;
        g_probe_armed = 1;
        fprintf(stderr, "[ESPGUARD] armed: reporting the first label with esp outside RAM\n");
        fflush(stderr);
    }
    if (!s || !*s) return;
    n = recomp_probe_arm(s);
    fprintf(stderr, "[PROBE] XBOX_PROBE: %d armed%s\n",
            n < 0 ? 0 : n, n < 0 ? " (parse error)" : "");
    fflush(stderr);
}
