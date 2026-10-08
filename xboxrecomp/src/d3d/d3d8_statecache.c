/*
 * d3d8_statecache.c -- mirror of the D3D11 context's state bindings.
 * See d3d8_statecache.h.
 */
#include "d3d8_internal.h"
#include "../kernel/xbox_perf.h"
#include <string.h>
#include <stdlib.h>

/* Real calls, through the vtable (the macros are redefined). */
#define VT(c) ((c)->lpVtbl)

static ID3D11DeviceContext *s_ctx;       /* the immediate context, seen on the first call */

static struct {
    int valid;                          /* 0 = nothing known (start, ClearState) */
    ID3D11BlendState *bs; FLOAT bf[4]; UINT mask; int bs_ok;
    ID3D11DepthStencilState *ds; UINT ref; int ds_ok;
    ID3D11RasterizerState *rs; int rs_ok;
    D3D11_RECT sc; int sc_ok;           /* a single rectangle tracked */
    ID3D11InputLayout *il; int il_ok;
    D3D11_PRIMITIVE_TOPOLOGY topo; int topo_ok;
    ID3D11Buffer *vb[16]; UINT vst[16], voff[16]; unsigned vb_ok;     /* one bit per slot */
    ID3D11Buffer *ib; DXGI_FORMAT ibf; UINT iboff; int ib_ok;
    ID3D11VertexShader *vs; int vs_ok;
    ID3D11PixelShader *ps; int ps_ok;
    ID3D11SamplerState *smp[16]; unsigned smp_ok;
    ID3D11Buffer *vcb[4]; UINT vfirst[4], vnum[4]; unsigned vcb_ok;   /* num 0 = bound without offset */
    ID3D11Buffer *pcb[4]; UINT pfirst[4], pnum[4]; unsigned pcb_ok;
} S;

int d3d8_pump_state_on(void);           /* d3d8_nv2a.c (bascule + alternance) */

static int mine(ID3D11DeviceContext *c)
{
    if (!s_ctx) s_ctx = d3d8_GetD3D11Context();
    return c == s_ctx;
}

/* Filter: only when the switch is on; the mirror itself is always kept up
 * to date. */
static int skip_ok(void) { return d3d8_pump_state_on(); }

static void count(int skipped)
{
    if (g_perf_on) perf_count(skipped ? PC_D3DSKIP : PC_D3DSET, 1);
}

void sc_OMSetBlendState(ID3D11DeviceContext *c, ID3D11BlendState *s, const FLOAT f[4], UINT mask)
{
    static const FLOAT one[4] = { 1, 1, 1, 1 };
    const FLOAT *bf = f ? f : one;
    if (mine(c)) {
        if (S.bs_ok && S.bs == s && S.mask == mask && !memcmp(S.bf, bf, sizeof S.bf) && skip_ok()) { count(1); return; }
        S.bs = s; S.mask = mask; memcpy(S.bf, bf, sizeof S.bf); S.bs_ok = 1;
    }
    count(0);
    VT(c)->OMSetBlendState(c, s, f, mask);
}

void sc_OMSetDepthStencilState(ID3D11DeviceContext *c, ID3D11DepthStencilState *s, UINT ref)
{
    if (mine(c)) {
        if (S.ds_ok && S.ds == s && S.ref == ref && skip_ok()) { count(1); return; }
        S.ds = s; S.ref = ref; S.ds_ok = 1;
    }
    count(0);
    VT(c)->OMSetDepthStencilState(c, s, ref);
}

void sc_RSSetState(ID3D11DeviceContext *c, ID3D11RasterizerState *s)
{
    if (mine(c)) {
        if (S.rs_ok && S.rs == s && skip_ok()) { count(1); return; }
        S.rs = s; S.rs_ok = 1;
    }
    count(0);
    VT(c)->RSSetState(c, s);
}

void sc_RSSetScissorRects(ID3D11DeviceContext *c, UINT n, const D3D11_RECT *r)
{
    if (mine(c)) {
        if (n == 1 && r) {
            if (S.sc_ok && !memcmp(&S.sc, r, sizeof S.sc) && skip_ok()) { count(1); return; }
            S.sc = *r; S.sc_ok = 1;
        } else
            S.sc_ok = 0;
    }
    count(0);
    VT(c)->RSSetScissorRects(c, n, r);
}

void sc_IASetInputLayout(ID3D11DeviceContext *c, ID3D11InputLayout *il)
{
    if (mine(c)) {
        if (S.il_ok && S.il == il && skip_ok()) { count(1); return; }
        S.il = il; S.il_ok = 1;
    }
    count(0);
    VT(c)->IASetInputLayout(c, il);
}

void sc_IASetPrimitiveTopology(ID3D11DeviceContext *c, D3D11_PRIMITIVE_TOPOLOGY t)
{
    if (mine(c)) {
        if (S.topo_ok && S.topo == t && skip_ok()) { count(1); return; }
        S.topo = t; S.topo_ok = 1;
    }
    count(0);
    VT(c)->IASetPrimitiveTopology(c, t);
}

void sc_IASetVertexBuffers(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b,
                           const UINT *strides, const UINT *offs)
{
    if (mine(c)) {
        UINT i;
        int same = start + n <= 16 && b && strides && offs;
        for (i = 0; same && i < n; i++)
            same = (S.vb_ok >> (start + i) & 1u) && S.vb[start + i] == b[i]
                   && S.vst[start + i] == strides[i] && S.voff[start + i] == offs[i];
        if (same && n && skip_ok()) { count(1); return; }
        for (i = 0; i < n && start + i < 16; i++) {
            if (b && strides && offs) {
                S.vb[start + i] = b[i]; S.vst[start + i] = strides[i]; S.voff[start + i] = offs[i];
                S.vb_ok |= 1u << (start + i);
            } else
                S.vb_ok &= ~(1u << (start + i));
        }
    }
    count(0);
    VT(c)->IASetVertexBuffers(c, start, n, b, strides, offs);
}

void sc_IASetIndexBuffer(ID3D11DeviceContext *c, ID3D11Buffer *b, DXGI_FORMAT f, UINT off)
{
    if (mine(c)) {
        if (S.ib_ok && S.ib == b && S.ibf == f && S.iboff == off && skip_ok()) { count(1); return; }
        S.ib = b; S.ibf = f; S.iboff = off; S.ib_ok = 1;
    }
    count(0);
    VT(c)->IASetIndexBuffer(c, b, f, off);
}

void sc_VSSetShader(ID3D11DeviceContext *c, ID3D11VertexShader *s, ID3D11ClassInstance *const *ci, UINT nci)
{
    if (mine(c)) {
        if (!nci && S.vs_ok && S.vs == s && skip_ok()) { count(1); return; }
        S.vs = s; S.vs_ok = !nci;
    }
    count(0);
    VT(c)->VSSetShader(c, s, ci, nci);
}

void sc_PSSetShader(ID3D11DeviceContext *c, ID3D11PixelShader *s, ID3D11ClassInstance *const *ci, UINT nci)
{
    if (mine(c)) {
        if (!nci && S.ps_ok && S.ps == s && skip_ok()) { count(1); return; }
        S.ps = s; S.ps_ok = !nci;
    }
    count(0);
    VT(c)->PSSetShader(c, s, ci, nci);
}

void sc_PSSetSamplers(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11SamplerState *const *s)
{
    if (mine(c)) {
        UINT i;
        int same = start + n <= 16 && s;
        for (i = 0; same && i < n; i++)
            same = (S.smp_ok >> (start + i) & 1u) && S.smp[start + i] == s[i];
        if (same && n && skip_ok()) { count(1); return; }
        for (i = 0; i < n && start + i < 16; i++) {
            if (s) { S.smp[start + i] = s[i]; S.smp_ok |= 1u << (start + i); }
            else S.smp_ok &= ~(1u << (start + i));
        }
    }
    count(0);
    VT(c)->PSSetSamplers(c, start, n, s);
}

/* Constants: num = 0 records a binding without offset (the classic call). */
static int cb_same(ID3D11Buffer **sb, UINT *sf, UINT *sn, unsigned ok, UINT start, UINT n,
                   ID3D11Buffer *const *b, const UINT *first, const UINT *num)
{
    UINT i;
    if (start + n > 4 || !b || !n) return 0;
    for (i = 0; i < n; i++) {
        UINT f = first ? first[i] : 0, m = num ? num[i] : 0;
        if (!(ok >> (start + i) & 1u) || sb[start + i] != b[i] || sf[start + i] != f || sn[start + i] != m)
            return 0;
    }
    return 1;
}

static void cb_note(ID3D11Buffer **sb, UINT *sf, UINT *sn, unsigned *ok, UINT start, UINT n,
                    ID3D11Buffer *const *b, const UINT *first, const UINT *num)
{
    UINT i;
    for (i = 0; i < n; i++) {
        if (start + i >= 4) { continue; }
        if (b) {
            sb[start + i] = b[i]; sf[start + i] = first ? first[i] : 0; sn[start + i] = num ? num[i] : 0;
            *ok |= 1u << (start + i);
        } else
            *ok &= ~(1u << (start + i));
    }
    if (start + n > 4) *ok = 0;         /* beyond what is tracked: forget everything */
}

void sc_VSSetConstantBuffers(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b)
{
    if (mine(c)) {
        if (cb_same(S.vcb, S.vfirst, S.vnum, S.vcb_ok, start, n, b, NULL, NULL) && skip_ok()) { count(1); return; }
        cb_note(S.vcb, S.vfirst, S.vnum, &S.vcb_ok, start, n, b, NULL, NULL);
    }
    count(0);
    VT(c)->VSSetConstantBuffers(c, start, n, b);
}

void sc_PSSetConstantBuffers(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b)
{
    if (mine(c)) {
        if (cb_same(S.pcb, S.pfirst, S.pnum, S.pcb_ok, start, n, b, NULL, NULL) && skip_ok()) { count(1); return; }
        cb_note(S.pcb, S.pfirst, S.pnum, &S.pcb_ok, start, n, b, NULL, NULL);
    }
    count(0);
    VT(c)->PSSetConstantBuffers(c, start, n, b);
}

void sc_VSSetConstantBuffers1(ID3D11DeviceContext1 *c, UINT start, UINT n, ID3D11Buffer *const *b,
                              const UINT *first, const UINT *num)
{
    if (mine((ID3D11DeviceContext *)c)) {
        if (cb_same(S.vcb, S.vfirst, S.vnum, S.vcb_ok, start, n, b, first, num) && skip_ok()) { count(1); return; }
        cb_note(S.vcb, S.vfirst, S.vnum, &S.vcb_ok, start, n, b, first, num);
    }
    count(0);
    VT(c)->VSSetConstantBuffers1(c, start, n, b, first, num);
}

void sc_PSSetConstantBuffers1(ID3D11DeviceContext1 *c, UINT start, UINT n, ID3D11Buffer *const *b,
                              const UINT *first, const UINT *num)
{
    if (mine((ID3D11DeviceContext *)c)) {
        if (cb_same(S.pcb, S.pfirst, S.pnum, S.pcb_ok, start, n, b, first, num) && skip_ok()) { count(1); return; }
        cb_note(S.pcb, S.pfirst, S.pnum, &S.pcb_ok, start, n, b, first, num);
    }
    count(0);
    VT(c)->PSSetConstantBuffers1(c, start, n, b, first, num);
}

void sc_ClearState(ID3D11DeviceContext *c)
{
    if (mine(c)) memset(&S, 0, sizeof S);
    VT(c)->ClearState(c);
}

void sc_ClearState1(ID3D11DeviceContext1 *c)
{
    if (mine((ID3D11DeviceContext *)c)) memset(&S, 0, sizeof S);
    VT(c)->ClearState(c);
}
