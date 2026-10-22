# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   POSIX  -> backed by OpenGL 3.3 core via SDL2 + libepoxy (d3d8_gl.c)

# OT_HEADLESS (macOS bring-up): a null backend that accepts every call and draws
# nothing -- see d3d8_null.c. Selected on any host, so it can be built on Windows too.
option(OT_HEADLESS "Build the null D3D8/PGRAPH backend (no renderer)" OFF)

# OT_GPU (with OT_HEADLESS, macOS): the real PGRAPH translator (nv2a/nv2a_pgraph_d3d11.c) drawing through
# d3d8_gpu.c, a GPU backend on SDL3's GPU API (Metal). Without it the null backend draws nothing.
option(OT_GPU "macOS GPU renderer (SDL3 GPU API) instead of the null backend" OFF)

if(OT_HEADLESS)
    add_library(xbox_d3d8 STATIC d3d8_null.c)
    target_link_libraries(xbox_d3d8 PUBLIC platform)
    if(OT_GPU)
        find_package(SDL3 CONFIG REQUIRED)
        target_sources(xbox_d3d8 PRIVATE d3d8_gpu.c d3d8_gpu_vsh.c d3d8_gpu_smaa.c d3d8_gpu_smaa_msl.c d3d8_gpu_shadow.c d3d8_gpu_out.c)
        target_compile_definitions(xbox_d3d8 PUBLIC OT_GPU=1)
        target_link_libraries(xbox_d3d8 PUBLIC SDL3::SDL3)
    endif()
elseif(WIN32)
    add_library(xbox_d3d8 STATIC
        d3d8_device.c
        d3d8_post.c
        d3d8_softshadow.c
        d3d8_gpuprof.c
        d3d8_sync.c
        d3d8_resources.c
        d3d8_shaders.c
        d3d8_states.c
        d3d8_statecache.c
        d3d8_combiners.c
        d3d8_nv2a.c
        d3d8_shcache.c
        d3d8_nv2a_vsh.c
        d3d8_vsh.c
    )
    target_link_libraries(xbox_d3d8 PUBLIC platform d3d11 dxgi dxguid d3dcompiler windowscodecs ole32)
else()
    add_library(xbox_d3d8 STATIC
        d3d8_gl.c
    )
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    target_include_directories(xbox_d3d8 PRIVATE ${EPOXY_INCLUDE_DIRS})
    target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2 ${EPOXY_LIBRARIES})
endif()

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

# Host test of the shader disk cache (no game, no D3D):
#   cmake -DXBOX_BUILD_TESTS=ON ... ; cmake --build ... --target shcache_test
option(XBOX_BUILD_TESTS "Build the host tests of the D3D layer (shcache_test)" OFF)
if(XBOX_BUILD_TESTS)
    add_executable(shcache_test tests/shcache_test.c d3d8_shcache.c)
    target_include_directories(shcache_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(shcache_test PRIVATE Threads::Threads)
    endif()
endif()
