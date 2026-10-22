/* ot_assets.c -- see ot_assets.h. Non-Windows hosts only. */
#include "ot_assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

typedef struct { char name[64]; char *path; void *data; size_t size; int tried; } Entry;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static Entry *s_tab; static int s_n, s_loaded;
static char s_root[4096];

static int dir_has_map(const char *dir)
{
    char p[4200]; FILE *f;
    snprintf(p, sizeof p, "%s/resources.txt", dir);
    f = fopen(p, "r");
    if (f) fclose(f);
    return f != NULL;
}

static void find_root(void)
{
    const char *e = getenv("OT_ASSET_DIR");
    if (e && e[0] && dir_has_map(e)) { snprintf(s_root, sizeof s_root, "%s", e); return; }
#ifdef __APPLE__
    {
        char exe[4096], *slash; uint32_t k = (uint32_t)sizeof exe;
        if (_NSGetExecutablePath(exe, &k) == 0 && (slash = strrchr(exe, '/'))) {
            char d[4200];
            *slash = 0;
            snprintf(d, sizeof d, "%s/assets", exe);
            if (dir_has_map(d)) { snprintf(s_root, sizeof s_root, "%s", d); return; }
        }
    }
#endif
#ifdef OT_DEFAULT_ASSET_DIR
    if (dir_has_map(OT_DEFAULT_ASSET_DIR)) { snprintf(s_root, sizeof s_root, "%s", OT_DEFAULT_ASSET_DIR); return; }
#endif
    s_root[0] = 0;
}

static void load_map(void)
{
    char p[4200], line[1024]; FILE *f; int cap = 0;
    s_loaded = 1;
    find_root();
    if (!s_root[0]) { fprintf(stderr, "[ASSETS] no assets folder found (OT_ASSET_DIR): button icons and the HD index are unavailable\n"); return; }
    snprintf(p, sizeof p, "%s/resources.txt", s_root);
    f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char name[64], rel[900];
        if (sscanf(line, "%63s %899s", name, rel) != 2) continue;
        if (s_n == cap) { cap = cap ? cap * 2 : 128; s_tab = (Entry *)realloc(s_tab, (size_t)cap * sizeof *s_tab); if (!s_tab) { s_n = 0; fclose(f); return; } }
        memset(&s_tab[s_n], 0, sizeof s_tab[s_n]);
        snprintf(s_tab[s_n].name, sizeof s_tab[s_n].name, "%s", name);
        { size_t n = strlen(s_root) + strlen(rel) + 2; s_tab[s_n].path = (char *)malloc(n); if (s_tab[s_n].path) snprintf(s_tab[s_n].path, n, "%s/%s", s_root, rel); }
        s_n++;
    }
    fclose(f);
    fprintf(stderr, "[ASSETS] %d resources from %s\n", s_n, s_root);
}

const void *ot_asset(const char *name, size_t *size)
{
    const void *r = NULL;
    int i;
    pthread_mutex_lock(&s_lock);
    if (!s_loaded) load_map();
    for (i = 0; i < s_n; i++) {
        Entry *e = &s_tab[i];
        if (strcmp(e->name, name)) continue;
        if (!e->tried) {
            FILE *f = e->path ? fopen(e->path, "rb") : NULL;
            e->tried = 1;
            if (f) {
                long n;
                fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
                if (n > 0 && (e->data = malloc((size_t)n + 1)) != NULL) {
                    if (fread(e->data, 1, (size_t)n, f) == (size_t)n) { e->size = (size_t)n; ((char *)e->data)[n] = 0; }
                    else { free(e->data); e->data = NULL; }
                }
                fclose(f);
            }
        }
        if (e->data) { r = e->data; if (size) *size = e->size; }
        break;
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}
