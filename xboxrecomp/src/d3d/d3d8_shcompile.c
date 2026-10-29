/**
 * D3DCompile through the shader disk cache (d3d8_shcache.c).
 *
 * Every HLSL compile of the D3D layer goes through here: the pump's
 * generated shaders (combiner pixel shaders, vertex programs, d3d8_nv2a.c)
 * and the layer's own ones -- fixed-function shaders, post-process and SMAA,
 * soft shadows, render scale, the scaling blit, the point geometry shader,
 * the API-side combiners and vertex shaders, the bloom and its light mask,
 * the speed blur, the temporal inputs. A shader seen by an earlier
 * session is read back from the disk instead of compiled again: same
 * bytecode, none of the compile time (the fixed-function pixel shader alone
 * takes over a second, before the first frame).
 *
 * The key is the one of the pump's cache: HLSL source, name, entry point,
 * profile, flags, cache format and the d3dcompiler_47.dll build. Macros,
 * when there are any, are part of the hashed text (name and value of each,
 * before the source); without macros the key is unchanged, so the pump's
 * entries written by earlier builds stay valid.
 *
 * XBOX_SHADER_CACHE=0: D3DCompile with the same arguments as before, no file
 * touched.
 */
#include "d3d8_internal.h"
#include "d3d8_shcache.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef D3DCOMPILER_DLL_A
#define D3DCOMPILER_DLL_A "d3dcompiler_47.dll"
#endif

/* Milliseconds since the process was created, for one-shot log lines. The
 * precise clock is looked up at run time (Windows 8 and later); without it,
 * the plain one (a few ms coarser). */
double d3d8_ms_since_start(void)
{
    typedef VOID (WINAPI *PGetTime)(LPFILETIME);
    static PGetTime get_time;
    FILETIME created, ex, kern, user, now;
    ULARGE_INTEGER a, b;
    if (!get_time) {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        PGetTime p = k ? (PGetTime)(void *)GetProcAddress(k, "GetSystemTimePreciseAsFileTime") : NULL;
        get_time = p ? p : GetSystemTimeAsFileTime;
    }
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ex, &kern, &user)) return 0.0;
    get_time(&now);
    a.LowPart = created.dwLowDateTime; a.HighPart = created.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;     b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (double)(b.QuadPart - a.QuadPart) / 10000.0 : 0.0;
}

static double shc_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER q;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* The compiler build: the version of d3dcompiler_47.dll loaded (its PE time
 * stamp and image size). */
static uint64_t shc_compiler_id(void)
{
    uint64_t id = (uint64_t)D3D_COMPILER_VERSION << 56;
    HMODULE m = GetModuleHandleA(D3DCOMPILER_DLL_A);
    if (m) {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)m;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)m + dos->e_lfanew);
        id ^= (uint64_t)nt->FileHeader.TimeDateStamp ^ ((uint64_t)nt->OptionalHeader.SizeOfImage << 32);
    }
    return id;
}

/* Opened once, by whichever thread compiles first (device creation, the
 * pump, or the API path). */
static volatile LONG s_state;           /* 0 not yet, 1 opening, 2 done */
static int s_on;

static int shc_on(void)
{
    if (InterlockedCompareExchange(&s_state, 0, 0) != 2) {
        if (InterlockedCompareExchange(&s_state, 1, 0) == 0) {
            int on = shc_env_on();
            if (on) on = shc_open(NULL, shc_compiler_id(), 0, 1);
            fprintf(stderr, "[SHC] XBOX_SHADER_CACHE=%d\n", on);
            s_on = on;
            InterlockedExchange(&s_state, 2);
        } else {
            while (InterlockedCompareExchange(&s_state, 0, 0) != 2) Sleep(0);
        }
    }
    return s_on;
}

static int has_macros(const D3D_SHADER_MACRO *defs)
{
    return defs && defs[0].Name;
}

/* The key of a compile. With macros, the hashed text is each macro's name
 * and value (each followed by a 0 byte), a 0 byte, then the source: HLSL
 * text never holds a 0 byte, so it cannot match a compile without macros. */
static int shc_key(const char *src, size_t len, const char *name, const D3D_SHADER_MACRO *defs,
                   const char *entry, const char *profile, UINT flags, ShcId *id)
{
    const D3D_SHADER_MACRO *d;
    size_t n = 1;
    char *buf, *p;

    if (!has_macros(defs)) {
        *id = shc_id(src, len, name, profile, entry, (uint32_t)flags);
        return 1;
    }
    for (d = defs; d->Name; d++)
        n += strlen(d->Name) + 1 + (d->Definition ? strlen(d->Definition) : 0) + 1;
    buf = (char *)malloc(n + len);
    if (!buf) return 0;
    p = buf;
    for (d = defs; d->Name; d++) {
        size_t a = strlen(d->Name) + 1, b = d->Definition ? strlen(d->Definition) : 0;
        memcpy(p, d->Name, a); p += a;
        if (b) memcpy(p, d->Definition, b);
        p += b;
        *p++ = '\0';
    }
    *p++ = '\0';
    memcpy(p, src, len);
    *id = shc_id(buf, n + len, name, profile, entry, (uint32_t)flags);
    free(buf);
    return 1;
}

HRESULT d3d8_shader_compile(const char *src, SIZE_T len, const char *name,
                            const D3D_SHADER_MACRO *defs, const char *entry, const char *profile,
                            UINT flags, ID3DBlob **code, ID3DBlob **err, int *hit)
{
    ShcId id;
    void *bin;
    size_t n = 0;
    HRESULT hr;

    if (hit) *hit = 0;
    if (!shc_on() || !shc_key(src, (size_t)len, name, defs, entry, profile, flags, &id))
        return D3DCompile(src, len, name, defs, NULL, entry, profile, flags, 0, code, err);
    bin = shc_get(id, &n);
    if (bin) {
        hr = D3DCreateBlob(n, code);
        if (SUCCEEDED(hr)) {
            memcpy(ID3D10Blob_GetBufferPointer(*code), bin, n);
            shc_free(bin);
            if (hit) *hit = 1;
            return hr;
        }
        shc_free(bin);
    }
    hr = D3DCompile(src, len, name, defs, NULL, entry, profile, flags, 0, code, err);
    if (SUCCEEDED(hr) && *code)
        shc_put(id, ID3D10Blob_GetBufferPointer(*code), ID3D10Blob_GetBufferSize(*code));
    return hr;
}

/* The same, plus one log line: where the bytecode came from and the time it
 * took (a few dozen such shaders per session; capped all the same). */
HRESULT d3d8_shader_compile_logged(const char *src, SIZE_T len, const char *name,
                                   const D3D_SHADER_MACRO *defs, const char *entry,
                                   const char *profile, UINT flags,
                                   ID3DBlob **code, ID3DBlob **err)
{
    static volatile LONG lines;
    double t0 = shc_now_ms();
    int hit = 0;
    HRESULT hr = d3d8_shader_compile(src, len, name, defs, entry, profile, flags, code, err, &hit);
    LONG k = InterlockedIncrement(&lines);
    if (k <= 256)
        fprintf(stderr, "[SHC] %s/%s %s%s%s: %s, %.1f ms (t+%.0f ms)\n",
                name ? name : "?", entry, profile,
                has_macros(defs) ? " " : "", has_macros(defs) ? defs[0].Name : "",
                FAILED(hr) ? "failed" : hit ? "from the cache" : "compiled",
                shc_now_ms() - t0, d3d8_ms_since_start());
    else if (k == 257)
        fprintf(stderr, "[SHC] (no more lines for the layer's shaders)\n");
    return hr;
}
