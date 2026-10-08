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

# The game's icon and manifest (src/ssx_recomp.rc.in) need the resource
# compiler (windres with MinGW, rc.exe with MSVC).
enable_language(RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# The PS2 game's own buttons (XBOX_BUTTON_ICONS=ps2, assets/btnicons/ps2/):
# built in by default; OFF leaves their pictures out of the executable (a
# saved "ps2" then reads as auto).
option(SSX_PS2_BUTTONS "Build the PS2 original button style (its pictures in the executable)" ON)
if(SSX_PS2_BUTTONS)
    add_compile_definitions(SSX_PS2_BUTTONS_READY=1)
endif()

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
# HD texture index (port/tools/hdtex_index.py), built into the exe.
set(SSX_HDTEX_INDEX "${CMAKE_CURRENT_SOURCE_DIR}/assets/hdtex_index.txt")
# Button icon drawings (CC0), built into the exe (nv2a_btnicons.c).
set(SSX_BTNICONS "${CMAKE_CURRENT_SOURCE_DIR}/assets/btnicons/juliocacko")
set(SSX_BTNICONS_DRAWN "${CMAKE_CURRENT_SOURCE_DIR}/assets/btnicons/drawn")
set(SSX_BTNICONS_PADS "${CMAKE_CURRENT_SOURCE_DIR}/assets/btnicons/pads")
file(GLOB SSX_BTNICON_FILES "${SSX_BTNICONS}/*.png" "${SSX_BTNICONS_DRAWN}/*.png" "${SSX_BTNICONS_PADS}/*.png")
# The PS2 button pictures (assets/btnicons/ps2/): resource
# PS2_<NAME> for ps2_<name>.png, with SSX_PS2_BUTTONS only.
set(SSX_BTNICONS_PS2_RC "")
if(SSX_PS2_BUTTONS)
    file(GLOB SSX_BTNICONS_PS2 "${CMAKE_CURRENT_SOURCE_DIR}/assets/btnicons/ps2/ps2_*.png")
    foreach(f IN LISTS SSX_BTNICONS_PS2)
        get_filename_component(n "${f}" NAME_WE)
        string(TOUPPER "${n}" n)
        string(APPEND SSX_BTNICONS_PS2_RC "${n} RCDATA \"${f}\"\n")
    endforeach()
    list(APPEND SSX_BTNICON_FILES ${SSX_BTNICONS_PS2})
endif()
# The version resource (src/version_info.rc, numbers in src/version.h).
set(SSX_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
configure_file(src/ssx_recomp.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ssx_recomp.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/ssx_recomp.rc" PROPERTIES
    OBJECT_DEPENDS "${SSX_APP_ICON};${SSX_HDTEX_INDEX};${SSX_BTNICON_FILES};${SSX_SRC_DIR}/version.h;${SSX_SRC_DIR}/version_info.rc")

set(GAME_SOURCES
    src/main.c
    src/launcher.c
    src/settings.c
    src/controls.c
    src/hostui.c
    src/crashreport.c
    "${CMAKE_CURRENT_BINARY_DIR}/ssx_recomp.rc"
    src/recomp_manual.c
    src/pass_tags.c
    src/fps_cap.c
    src/aspect.c
    src/drawdist.c
    src/pumplag.c
    src/strmwalk.c
    src/chantfix.c
    src/ctlscheme.c
    src/ps2legend.c
    src/fullfade.c
    src/hud_anchor.c
    src/ticktrace.c
    src/perf_hooks.c
    src/netplay/np_cmdlog.c
    src/netplay/np_ghost.c
    src/netplay/np_racebench.c
    src/netplay/np_net.c
    src/xapi_input_hle.c
    src/recomp/recomp_probe.c
)

# Build identity written into crash reports (crashreport.c): -DSSX_BUILD_ID=...
# (a release name) wins; otherwise the commit at configure time, "+" when the
# tree had local changes. Only crashreport.c is recompiled when it changes.
if(NOT SSX_BUILD_ID)
    set(SSX_BUILD_ID "unknown")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=10 HEAD
                        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                        OUTPUT_VARIABLE _ssx_rev OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE _ssx_rev_rc ERROR_QUIET)
        if(_ssx_rev_rc EQUAL 0 AND _ssx_rev)
            execute_process(COMMAND "${GIT_EXECUTABLE}" diff --quiet HEAD --
                            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                            RESULT_VARIABLE _ssx_dirty ERROR_QUIET)
            set(SSX_BUILD_ID "${_ssx_rev}")
            if(NOT _ssx_dirty EQUAL 0)
                set(SSX_BUILD_ID "${_ssx_rev}+")
            endif()
        endif()
    endif()
endif()
message(STATUS "Build identity for crash reports: ${SSX_BUILD_ID}")
set_source_files_properties(src/crashreport.c PROPERTIES
    COMPILE_DEFINITIONS "SSX_BUILD_ID=\"${SSX_BUILD_ID}\"")

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

# A release build: the version without "-dev" (src/version.h). Only the release
# tools turn it on; every other build says "-dev".
option(OT_RELEASE "Release build (version without -dev)" OFF)
if(OT_RELEASE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE OT_RELEASE=1)
endif()

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
    ws2_32      # netplay over UDP (XBOX_NET)
    # Launcher: controls dialog, file/folder pickers, shell
    comctl32
    ole32
    shell32
    uuid
    d3dcompiler
)

# Host tests of the settings registry and settings.ini (no game):
#   cmake -DSSX_BUILD_TESTS=ON ... ; cmake --build ... --target settings_test
option(SSX_BUILD_TESTS "Build the host tests (settings_test)" OFF)
if(SSX_BUILD_TESTS)
    add_executable(settings_test tests/settings_test.c src/settings.c)
    target_include_directories(settings_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(WIN32)
        target_link_libraries(settings_test PRIVATE shell32 ole32 uuid)
    endif()
endif()

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
    # The generated files are very large.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()
