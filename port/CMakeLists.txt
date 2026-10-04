cmake_minimum_required(VERSION 3.20)

# ============================================================================
# SSX Tricky (Xbox) -- native PC port
#
#   cmake -S port -B port/build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
#   cmake --build port/build -j
#
# The translated game code (src/recomp/gen/, src/recomp/recomp_funcs.h) is not
# in the repository: it is generated from your own copy of the game. See
# docs/building.md.
# ============================================================================

project(ssx_tricky C)

# The launcher's icon and manifest (src/ssx_recomp.rc.in) need the resource
# compiler (windres with MinGW, rc.exe with MSVC).
enable_language(RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp runtime (vendored at the repository root) ───────────────────
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Translated game code ───────────────────────────────────────────────────
# CONFIGURE_DEPENDS so regenerating picks up added or removed files without a
# manual re-configure.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")
if(NOT RECOMP_GEN_SOURCES)
    message(FATAL_ERROR
        "port/src/recomp/gen/ is empty: the translated game code has not been "
        "generated yet. It is made from your own copy of the game and is never "
        "stored in this repository -- see docs/building.md.")
endif()

# ── Icon ───────────────────────────────────────────────────────
# Made at build time from the player's own game files, in the build directory
# only (tools/make_icon.py: the Garibaldi track badge by default);
# src/ssx_recomp.ico, an original icon drawn for this fork, when the game files
# or Python are missing. Nothing of the game goes into the repository.
# (Renamed from SSX_ICON_PICK so a build directory configured before keeps no stale choice.)
set(SSX_APP_ICON_PICK "garibaldi" CACHE STRING "Icon from the game files: garibaldi, flocon, medal or logo")
set(SSX_APP_ICON "${CMAKE_CURRENT_BINARY_DIR}/ssx_app.ico")
set(SSX_ICON_FALLBACK "${CMAKE_CURRENT_SOURCE_DIR}/src/ssx_recomp.ico")
set(SSX_GAME_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../game_files")
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(SSX_ICON_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_icon.py" "${SSX_ICON_FALLBACK}")
    foreach(f data/textures/hudgame.xsh data/textures/loading.xsh)
        if(EXISTS "${SSX_GAME_FILES}/${f}")
            list(APPEND SSX_ICON_DEPENDS "${SSX_GAME_FILES}/${f}")
        endif()
    endforeach()
    add_custom_command(OUTPUT "${SSX_APP_ICON}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_icon.py"
                --game-files "${SSX_GAME_FILES}" --fallback "${SSX_ICON_FALLBACK}"
                --out "${SSX_APP_ICON}" --pick "${SSX_APP_ICON_PICK}"
        DEPENDS ${SSX_ICON_DEPENDS}
        COMMENT "Icon from the game files (build directory only)")
else()
    configure_file("${SSX_ICON_FALLBACK}" "${SSX_APP_ICON}" COPYONLY)
endif()
set(SSX_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/src/ssx_recomp.manifest")
configure_file(src/ssx_recomp.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ssx_recomp.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/ssx_recomp.rc" PROPERTIES
    OBJECT_DEPENDS "${SSX_APP_ICON}")

set(GAME_SOURCES
    src/main.c
    src/launcher.c
    src/discart.c
    src/controls.c
    src/hostui.c
    "${CMAKE_CURRENT_BINARY_DIR}/ssx_recomp.rc"
    src/recomp_manual.c
    src/pass_tags.c
    src/fps_cap.c
    src/aspect.c
    src/drawdist.c
    src/ticktrace.c
    src/perf_hooks.c
    src/netplay/np_cmdlog.c
    src/netplay/np_ghost.c
    src/xapi_input_hle.c
    src/recomp/recomp_probe.c
)

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Name the executable after the title in the XBE certificate ("SSX Tricky"),
# falling back to that name when no XBE is present.
include(${XBOXRECOMP_DIR}/cmake/XbeTitleName.cmake)
xbe_title_name(GAME_TITLE "${CMAKE_CURRENT_SOURCE_DIR}/../game_files/default.xbe"
               FALLBACK "SSX Tricky")
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${GAME_TITLE}")
message(STATUS "Executable will be named: ${GAME_TITLE}.exe")

target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # XInput 1.4 ships with Windows
    winmm
    # Launcher: controls dialog, file/folder pickers, shell
    comctl32
    ole32
    shell32
    uuid
    d3dcompiler
)

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
    # The generated files are very large.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()
