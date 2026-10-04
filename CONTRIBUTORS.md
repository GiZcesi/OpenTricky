# Contributors

OpenTricky continues [SSX Tricky PC](https://github.com/MatiasRiveraC/SSX-Tricky-PC). The people who made SSX Tricky PC are listed after the OpenTricky section.

## OpenTricky

- **GiZcesi**: project lead of OpenTricky. Sets the goals (Xbox fidelity first, then performance, then optional extras), decides what to fix and add, and tests every build on real PCs.

## SSX Tricky PC

### MatiasRiveraC: project lead

- Started and directs the port.
- Decides what the port should do: the launcher, the display and graphics options, the controls, and what to fix next.
- Tests every build and plays it on their own PC, against the original running on xemu.

### sp00nznet: xboxrecomp

[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) is the static recompiler and Xbox runtime this port is built on. It's MIT licensed and vendored in `xboxrecomp/` with changes for this port. SSX Tricky PC kept its commit history (up to upstream commit `32da238`); OpenTricky starts a new history, so see the two upstream repositories for who wrote what.

### Third-party work used

- [xemu](https://xemu.app): `xboxrecomp/src/nv2a/nv2a_psh.c` is a port of its register-combiner shader generator, and parts of `xboxrecomp/src/nv2a/` and `xboxrecomp/src/apu/` are adapted from its GPU and audio emulation (LGPL-2.1-or-later, see each file's header). Its GPU emulation was the reference for the renderer.
- [SMAA](https://www.iryoku.com/smaa/) by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez (MIT): `xboxrecomp/src/d3d/smaa/`.
- [Capstone](https://www.capstone-engine.org/) disassembly, and [Ghidra](https://ghidra-sre.org/) for reverse engineering.
