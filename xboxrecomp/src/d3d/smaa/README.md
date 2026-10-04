# SMAA (third party)

Unmodified copies from https://github.com/iryoku/smaa, commit
`71c806a838bdd7d517df19192a20f0c61b3ca29d` (2013-11-06), MIT licence (LICENSE.txt;
the copyright notice is also kept at the top of each file).

- `SMAA.hlsl`           -- the shader, embedded with `#embed` by `../d3d8_post.c`
                           (compiled at run time with `SMAA_CUSTOM_SL`).
- `Textures/AreaTex.h`  -> `AreaTex.h`   (160x560 RG8, bilinear)
- `Textures/SearchTex.h`-> `SearchTex.h` (64x16 R8, point)

Used by the post-processing chain (`XBOX_SMAA`). Do not edit these
files: configuration happens through defines in `d3d8_post.c`.
