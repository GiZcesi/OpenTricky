# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   POSIX  -> backed by OpenGL 3.3 core via SDL2 + libepoxy (d3d8_gl.c)

if(WIN32)
    add_library(xbox_d3d8 STATIC
        d3d8_device.c
        d3d8_post.c
        d3d8_softshadow.c
        d3d8_gpuprof.c
        d3d8_resources.c
        d3d8_shaders.c
        d3d8_states.c
        d3d8_statecache.c
        d3d8_combiners.c
        d3d8_nv2a.c
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
