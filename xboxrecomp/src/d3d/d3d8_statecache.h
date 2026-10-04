/*
 * d3d8_statecache.h -- miroir des liaisons d'état du contexte D3D11.
 *
 * Inclus par d3d8_internal.h après <d3d11.h> : les macros COBJMACROS des appels
 * d'état ci-dessous sont redéfinies pour passer par d3d8_statecache.c, qui
 * garde un miroir de ce qui est lié et, avec XBOX_FIX_PUMP_STATE=1, saute un
 * appel qui relierait exactement la même chose. Tous les fichiers d3d8_*.c
 * passent ainsi par le miroir : il reste exact sans invalidation à la main.
 *
 * Jamais filtrés (passent tels quels) : textures (SRV) et cibles de rendu, que
 * le runtime délie de lui-même en cas de conflit lecture / écriture.
 * ClearState vide le miroir. Un objet lié est retenu par le contexte : son
 * adresse ne peut pas être réutilisée tant qu'il est lié, la comparaison de
 * pointeurs est donc sûre. Autre contexte que l'immédiat : passe sans miroir.
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
#undef ID3D11DeviceContext_ClearState
#define ID3D11DeviceContext_ClearState(T) sc_ClearState(T)
#undef ID3D11DeviceContext1_ClearState
#define ID3D11DeviceContext1_ClearState(T) sc_ClearState1(T)

#endif
