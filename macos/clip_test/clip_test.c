/* Clip-rectangle arithmetic of the GPU backend (d3d8_gpu_clip.h): cc -I../../xboxrecomp/src/d3d clip_test.c && ./a.out */
#include <stdio.h>
#include "d3d8_gpu_clip.h"

typedef struct { long sc[4]; unsigned tw, th; int ok, x, y, w, h; const char *name; } Case;

int main(void)
{
    static const Case C[] = {
        { { 0, 0, 640, 480 }, 640, 480, 1, 0, 0, 640, 480, "full, 1x" },
        { { 10, 20, 110, 70 }, 640, 480, 1, 10, 20, 100, 50, "exclusive end, 1x (no +1 row/column)" },
        { { 10, 20, 110, 70 }, 1280, 960, 1, 20, 40, 200, 100, "2x scale" },
        { { -50, -50, 100, 100 }, 640, 480, 1, 0, 0, 100, 100, "negative origin clamps" },
        { { 600, 400, 900, 700 }, 640, 480, 1, 600, 400, 40, 80, "overhang clamps to the target" },
        { { 640, 0, 700, 100 }, 640, 480, 0, 0, 0, 0, 0, "entirely right of the target: skipped" },
        { { 50, 50, 50, 90 }, 640, 480, 0, 0, 0, 0, 0, "zero width: skipped" },
        { { 90, 50, 50, 90 }, 640, 480, 0, 0, 0, 0, 0, "inverted: skipped" },
        { { -20, -20, -5, -5 }, 640, 480, 0, 0, 0, 0, 0, "entirely negative: skipped" },
        { { 0, 0, 640, 480 }, 1920, 1080, 1, 0, 0, 1920, 1080, "non-uniform scale covers the target" },
    };
    int i, bad = 0;
    for (i = 0; i < (int)(sizeof C / sizeof C[0]); i++) {
        int x = -1, y = -1, w = -1, h = -1, ok = gpu_clip_rect(C[i].sc, C[i].tw, C[i].th, 640, 480, &x, &y, &w, &h);
        int good = ok == C[i].ok && (!ok || (x == C[i].x && y == C[i].y && w == C[i].w && h == C[i].h));
        if (ok && (x < 0 || y < 0 || x + w > (int)C[i].tw || y + h > (int)C[i].th)) good = 0;   /* never outside the target */
        printf("%s %s\n", good ? "ok  " : "FAIL", C[i].name);
        bad += !good;
    }
    printf("%s\n", bad ? "FAILED" : "all clip checks held");
    return bad != 0;
}
