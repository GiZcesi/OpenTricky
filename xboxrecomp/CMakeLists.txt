cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.1.0 LANGUAGES C)

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Libraries:
#   xbox_kernel    - Xbox kernel replacement layer (147 imports → Win32)
#   xbox_d3d8      - D3D8 → D3D11 graphics compatibility
#   xbox_dsound    - DirectSound → software mixer compatibility
#   xbox_input     - Xbox gamepad → XInput compatibility
#   xbox_apu       - MCPX APU audio emulation (extracted from xemu)
#   xbox_nv2a      - NV2A GPU emulation (extracted from xemu)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions for all targets
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# A call to an undeclared function is a hard error in the runtime, not a
# warning. C still accepts it as an implicit `int f()` taking any arguments,
# and on Win64 that is not a style problem: missing arguments are read from
# whatever sits in the caller's stack slots, and a pointer return is cut to 32
# bits. xbox_HeapFree called the seven-argument xbox_heap_owner_of with four,
# and the garbage `frames` pointer it wrote through zeroed the guest esp and
# hung DirectSound teardown -- one warning line in a full rebuild, invisible
# in an incremental one.
#
# Scoped to these hand-written libraries only. The generated code still has
# implicit declarations of its own, which are the recompiler's to fix.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Werror=implicit-function-declaration)
endif()

# Option to build examples

# ── Runtime Libraries ─────────────────────────────────────────

# Platform: Win32/NT type vocabulary + POSIX implementation of generic
# Win32 host primitives. Carries no Xbox semantics; provides the foundation
# the kernel HLE and the graphics/audio/input layers build on.
add_subdirectory(src/platform)

# Core: Xbox kernel replacement (memory layout, file I/O, threading, sync)
add_subdirectory(src/kernel)

# Graphics: D3D8 → D3D11 compatibility layer
add_subdirectory(src/d3d)

# Audio: DirectSound → software mixer compatibility
add_subdirectory(src/audio)

# Audio: MCPX APU hardware emulation (voice processor, DSP)
add_subdirectory(src/apu)

# GPU: NV2A hardware emulation (register handlers, MMIO, push buffer)
add_subdirectory(src/nv2a)

# Input: Xbox gamepad → XInput
add_subdirectory(src/input)

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
add_library(xboxrecomp INTERFACE)
target_link_libraries(xboxrecomp INTERFACE
    xbox_kernel
    xbox_d3d8
    xbox_dsound
    xbox_input
    xbox_apu
    xbox_nv2a
)

