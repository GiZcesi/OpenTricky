/*
 * d3d8_statecache.h -- mirror of the D3D11 context's state bindings.
 *
 * Included by d3d8_internal.h after <d3d11.h>: the COBJMACROS macros of the
 * state calls below are redefined to go through d3d8_statecache.c, which
 * keeps a mirror of what is bound and, with XBOX_FIX_PUMP_STATE=1, skips a
 * call that would bind exactly the same thing again. Every d3d8_*.c file
 * goes through the mirror this way: it stays exact without hand-written
 * invalidation.
 *
 * Never filtered (passed as is): render targets. Pixel-shader textures
 * (SRV), which the runtime unbinds by itself on a read / write conflict, are
 * filtered only with XBOX_FIX_PUMP_SRV: every render-target change,
 * ClearState, Present and buffer resize forgets them, and the mirror holds a
 * reference on each view it records.
 * ClearState empties the mirror, and so does sc_ForgetAll after code that
 * binds through the context directly: the tuning panel, and every Present
 * (an overlay hooked on it does too). A bound object is held by the
 * context: its address cannot be reused while it is bound, so comparing
 * pointers is safe.
 * A context other than the immediate one: passes without the mirror.
 */
#ifndef D3D8_STATECACHE_H
#define D3D8_STATECACHE_H

void sc_OMSetBlendState(ID3D11DeviceContext *c, ID3D11BlendState *s, const FLOAT f[4], UINT mask);
void sc_OMSetDepthStencilState(ID3D11DeviceContext *c, ID3D11DepthStencilState *s, UINT ref);
void sc_RSSetState(ID3D11DeviceContext *c, ID3D11RasterizerState *s);
void sc_RSSetScissorRects(ID3D11DeviceContext *c, UINT n, const D3D11_RECT *r);
void sc_IASetInputLayout(ID3D11DeviceContext *c, ID3D11InputLayout *il);
void sc_IASetPrimitiveTopology(ID3D11DeviceContext *c, D3D11_PRIMITIVE_TOPOLOGY t);
void sc_IASetVertexBuffers(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b,
                           const UINT *strides, const UINT *offs);
void sc_IASetIndexBuffer(ID3D11DeviceContext *c, ID3D11Buffer *b, DXGI_FORMAT f, UINT off);
void sc_VSSetShader(ID3D11DeviceContext *c, ID3D11VertexShader *s, ID3D11ClassInstance *const *ci, UINT nci);
void sc_PSSetShader(ID3D11DeviceContext *c, ID3D11PixelShader *s, ID3D11ClassInstance *const *ci, UINT nci);
void sc_PSSetSamplers(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11SamplerState *const *s);
void sc_VSSetConstantBuffers(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b);
void sc_PSSetConstantBuffers(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b);
void sc_VSSetConstantBuffers1(ID3D11DeviceContext1 *c, UINT start, UINT n, ID3D11Buffer *const *b,
                              const UINT *first, const UINT *num);
void sc_PSSetConstantBuffers1(ID3D11DeviceContext1 *c, UINT start, UINT n, ID3D11Buffer *const *b,
                              const UINT *first, const UINT *num);
void sc_ClearState(ID3D11DeviceContext *c);
void sc_ClearState1(ID3D11DeviceContext1 *c);
/* Pixel-shader textures (XBOX_FIX_PUMP_SRV) and what invalidates them. */
void sc_PSSetShaderResources(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v);
void sc_OMSetRenderTargets(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv);
void sc_OMSetRenderTargetsAndUnorderedAccessViews(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv,
                                                  ID3D11DepthStencilView *dsv, UINT uav_start, UINT nuav,
                                                  ID3D11UnorderedAccessView *const *uav, const UINT *counts);
void sc_ForgetTextures(void);
int  sc_PSSRVBound(UINT slot, ID3D11ShaderResourceView *v);   /* the bind of v there would be skipped */
void sc_ForgetAll(void);                /* after direct context calls (the tuning panel, Present) */

#undef ID3D11DeviceContext_OMSetBlendState
#define ID3D11DeviceContext_OMSetBlendState(T, a, b, c) sc_OMSetBlendState(T, a, b, c)
#undef ID3D11DeviceContext_OMSetDepthStencilState
#define ID3D11DeviceContext_OMSetDepthStencilState(T, a, b) sc_OMSetDepthStencilState(T, a, b)
#undef ID3D11DeviceContext_RSSetState
#define ID3D11DeviceContext_RSSetState(T, a) sc_RSSetState(T, a)
#undef ID3D11DeviceContext_RSSetScissorRects
#define ID3D11DeviceContext_RSSetScissorRects(T, a, b) sc_RSSetScissorRects(T, a, b)
#undef ID3D11DeviceContext_IASetInputLayout
#define ID3D11DeviceContext_IASetInputLayout(T, a) sc_IASetInputLayout(T, a)
#undef ID3D11DeviceContext_IASetPrimitiveTopology
#define ID3D11DeviceContext_IASetPrimitiveTopology(T, a) sc_IASetPrimitiveTopology(T, a)
#undef ID3D11DeviceContext_IASetVertexBuffers
#define ID3D11DeviceContext_IASetVertexBuffers(T, a, b, c, d, e) sc_IASetVertexBuffers(T, a, b, c, d, e)
#undef ID3D11DeviceContext_IASetIndexBuffer
#define ID3D11DeviceContext_IASetIndexBuffer(T, a, b, c) sc_IASetIndexBuffer(T, a, b, c)
#undef ID3D11DeviceContext_VSSetShader
#define ID3D11DeviceContext_VSSetShader(T, a, b, c) sc_VSSetShader(T, a, b, c)
#undef ID3D11DeviceContext_PSSetShader
#define ID3D11DeviceContext_PSSetShader(T, a, b, c) sc_PSSetShader(T, a, b, c)
#undef ID3D11DeviceContext_PSSetSamplers
#define ID3D11DeviceContext_PSSetSamplers(T, a, b, c) sc_PSSetSamplers(T, a, b, c)
#undef ID3D11DeviceContext_VSSetConstantBuffers
#define ID3D11DeviceContext_VSSetConstantBuffers(T, a, b, c) sc_VSSetConstantBuffers(T, a, b, c)
#undef ID3D11DeviceContext_PSSetConstantBuffers
#define ID3D11DeviceContext_PSSetConstantBuffers(T, a, b, c) sc_PSSetConstantBuffers(T, a, b, c)
#undef ID3D11DeviceContext1_VSSetConstantBuffers1
#define ID3D11DeviceContext1_VSSetConstantBuffers1(T, a, b, c, d, e) sc_VSSetConstantBuffers1(T, a, b, c, d, e)
#undef ID3D11DeviceContext1_PSSetConstantBuffers1
#define ID3D11DeviceContext1_PSSetConstantBuffers1(T, a, b, c, d, e) sc_PSSetConstantBuffers1(T, a, b, c, d, e)
#undef ID3D11DeviceContext_PSSetShaderResources
#define ID3D11DeviceContext_PSSetShaderResources(T, a, b, c) sc_PSSetShaderResources(T, a, b, c)
#undef ID3D11DeviceContext_OMSetRenderTargets
#define ID3D11DeviceContext_OMSetRenderTargets(T, a, b, c) sc_OMSetRenderTargets(T, a, b, c)
#undef ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews
#define ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(T, a, b, c, d, e, f, g) \
    sc_OMSetRenderTargetsAndUnorderedAccessViews(T, a, b, c, d, e, f, g)
#undef ID3D11DeviceContext_ClearState
#define ID3D11DeviceContext_ClearState(T) sc_ClearState(T)
#undef ID3D11DeviceContext1_ClearState
#define ID3D11DeviceContext1_ClearState(T) sc_ClearState1(T)

#endif
