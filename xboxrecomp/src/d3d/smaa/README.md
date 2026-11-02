# SMAA (third party)

Copies from https://github.com/iryoku/smaa, commit
`71c806a838bdd7d517df19192a20f0c61b3ca29d` (2013-11-06), MIT licence (LICENSE.txt;
the copyright notice is also kept at the top of each file). The textures are
unmodified; `SMAA.hlsl` has four lines changed, nothing else:
`SMAAMovc(bool4, inout float4, float4)` is renamed `SMAAMovc4` and
`SMAADecodeDiagBilinearAccess(float4)` is renamed `SMAADecodeDiagBilinearAccess4`,
at their definitions and at their one call each. Overloads chosen between by an
implicit conversion are not implemented in Wine 9.0's HLSL compiler
(vkd3d-shader 1.10, Proton 9), which refused all of SMAA; with one name per
overload, D3DCompile gives the same bytecode as before.

- `SMAA.hlsl`           -- the shader, embedded with `#embed` by `../d3d8_post.c`
                           (compiled at run time with `SMAA_CUSTOM_SL`).
- `Textures/AreaTex.h`  -> `AreaTex.h`   (160x560 RG8, bilinear)
- `Textures/SearchTex.h`-> `SearchTex.h` (64x16 R8, point)

Used by the post-processing chain (`XBOX_SMAA`). Do not edit these
files further: configuration happens through defines in `d3d8_post.c` (`mad()` too,
for the same compiler).
