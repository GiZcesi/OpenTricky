# NV2A GPU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Register handlers: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC, PFIFO, PGRAPH
# MMIO interception via VEH (Vectored Exception Handling)
# Push buffer parsing and command dispatch

# OT_HEADLESS (macOS bring-up): the Direct3D 11 translator and the files that
# hang off it (HD textures, button icons) are replaced by the null backend in
# ../d3d (d3d8_null.c). The push-buffer ring walker, nv2a_live_pb.c, stays.
set(NV2A_SOURCES
    nv2a_core.c
    nv2a_vsh_cpu.c
    nv2a_psh.c
    nv2a_par.c
    nv2a_pb_replay.c
    nv2a_mmio_hook.c
    nv2a_pb_test.c
    nv2a_live_pb.c      # the push-buffer ring walker: unchanged, drives the null backend too
)
if(NOT OT_HEADLESS)
    list(APPEND NV2A_SOURCES nv2a_pgraph_d3d11.c nv2a_hdtex.c nv2a_btnicons.c)
elseif(OT_GPU)
    # macOS GPU renderer: the real PGRAPH translator (hdtex and button icons stay off: d3d8_null.c)
    list(APPEND NV2A_SOURCES nv2a_pgraph_d3d11.c nv2a_hdtex.c nv2a_btnicons.c)
endif()
add_library(xbox_nv2a STATIC ${NV2A_SOURCES})

target_include_directories(xbox_nv2a PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_nv2a PRIVATE
        /W3
        /wd4244
        /wd4146
        /wd4267
    )
    target_compile_definitions(xbox_nv2a PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(xbox_nv2a PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_link_libraries(xbox_nv2a PRIVATE platform)
if(OT_GPU)
    find_package(SDL3 CONFIG REQUIRED)       # the button pictures are decoded with SDL's PNG loader (nv2a_btnicons.c)
    target_link_libraries(xbox_nv2a PRIVATE SDL3::SDL3)
endif()
# nv2a_live_pb.c creates the host D3D8 device the translator draws into
target_link_libraries(xbox_nv2a PUBLIC xbox_d3d8)
if(WIN32)
    target_link_libraries(xbox_nv2a PRIVATE kernel32)
endif()
