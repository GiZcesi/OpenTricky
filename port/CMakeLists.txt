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

project(ssx_tricky C CXX)

# The game's icon and manifest (src/ssx_recomp.rc.in) need the resource
# compiler (windres with MinGW, rc.exe with MSVC).
enable_language(RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── PlayStation controllers (src/pad_sdl.c), experimental ──────────────────
# SDL3 from port/third_party/sdl3 (the launcher's copy, zlib licence), built
# with the launcher's options and linked statically: no DLL to ship. Only
# pad_sdl.c uses it, and only when the player turns the option on
# (XBOX_PS_PADS=1). OFF: no SDL3 in the executable, XInput only. Added
# before the definitions below so that SDL builds as the launcher's does.
option(SSX_PS_PADS "Build the PlayStation controllers support (SDL3, experimental)" ON)
if(SSX_PS_PADS)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
    set(SDL_TRAY OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdl3 ${CMAKE_BINARY_DIR}/sdl3 EXCLUDE_FROM_ALL)
    # No debug information for SDL in the game: a few MB less, and the game's
    # own code keeps its symbols for the crash reports.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(SDL3-static PRIVATE -g0)
    endif()
    message(STATUS "PlayStation controllers: SDL3 (static, experimental option)")
else()
    message(STATUS "PlayStation controllers: not built (SSX_PS_PADS=OFF), XInput only")
endif()

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Floating point: no contraction of a*b+c into one fused multiply-add. GCC's
# default in GNU C mode is -ffp-contract=fast, which rounds once instead of
# twice wherever the target has FMA: the game's simulation would then depend
# on the -march it was built for. Without -march there is no FMA and this
# changes no instruction; it keeps it so if a -march is ever added. Set before
# the runtime below so it covers the game and the runtime alike (C only: not
# the resource compiler).
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-ffp-contract=off>)
endif()

# Link-time optimisation (GCC only; OFF by default, the game's code is the same
# either way). It changes no floating-point result: no -ffast-math or other
# value-changing option comes with it, and -ffp-contract=off above holds for
# every function at link time too.
#   -DSSX_LTO=ON                link-time optimisation of the game and the
#                               runtime as one program (-flto)
#   -DSSX_LTO_JOBS=8            parallel link-time jobs (default: auto, every
#                               thread of the machine)
set(SSX_LTO OFF CACHE BOOL "Link-time optimisation (GCC)")
set(SSX_LTO_JOBS "auto" CACHE STRING "Parallel link-time optimisation jobs (a number, or auto)")
if(SSX_LTO)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "SSX_LTO: GCC only")
    endif()
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-flto=${SSX_LTO_JOBS}>
                        $<$<COMPILE_LANGUAGE:C,CXX>:-fno-fat-lto-objects>)
    add_link_options(-flto=${SSX_LTO_JOBS} -ffp-contract=off)
    # The code (and the late debug information) is made at link time, and the
    # game links as C++, whose flags have neither -g nor the
    # C flags' path maps: keep them there too, or a crash report's addresses
    # lose their lines and the executable names the folder it was built in.
    if(CMAKE_C_FLAGS_RELEASE MATCHES "(^| )-g( |$)")
        add_link_options($<$<CONFIG:Release>:-g>)
    endif()
    string(REGEX MATCHALL "-f(file|debug)-prefix-map=[^ ]+" _ssx_lto_path_maps "${CMAKE_C_FLAGS}")
    if(_ssx_lto_path_maps)
        add_link_options(${_ssx_lto_path_maps})
    endif()
    # The runtime's static libraries hold LTO objects: archive them with GCC's
    # wrappers (they load the LTO plugin).
    if(CMAKE_C_COMPILER_AR AND CMAKE_C_COMPILER_RANLIB)
        set(CMAKE_AR "${CMAKE_C_COMPILER_AR}")
        set(CMAKE_RANLIB "${CMAKE_C_COMPILER_RANLIB}")
    endif()
    message(STATUS "Link-time optimisation (SSX_LTO): ON, -flto=${SSX_LTO_JOBS}")
endif()

# The PS2 button style (XBOX_BUTTON_ICONS=ps2, assets/btnicons/ps2/):
# built in by default; OFF leaves its pictures out of the executable (a
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
# or Python are missing.
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
    src/frame_clock.c
    src/aspect.c
    src/drawdist.c
    src/terrain_seams.c
    src/pumplag.c
    src/strmwalk.c
    src/chantfix.c
    src/riderlod.c
    src/shadowvb.c
    src/weather.c
    src/ctlscheme.c
    src/ps2legend.c
    src/extras_off.c
    src/fullfade.c
    src/hud_anchor.c
    src/perf_hooks.c
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

# Only pad_sdl.c knows about SDL, in a library of its own: SDL's headers and
# definitions never reach the other sources, and turning SSX_PS_PADS on or
# off rebuilds nothing else. (SDL's two GameInput sources are C++, empty with
# MinGW: the game is then linked by g++, with the static runtimes below.)
add_library(ssx_pad_sdl STATIC src/pad_sdl.c)
if(SSX_PS_PADS)
    target_compile_definitions(ssx_pad_sdl PRIVATE SSX_HAVE_SDL3)
    target_link_libraries(ssx_pad_sdl PRIVATE SDL3::SDL3-static)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE ssx_pad_sdl)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # no C / C++ runtime DLL beside the game
    target_link_options(${PROJECT_NAME} PRIVATE -static-libstdc++ -static-libgcc)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE dwmapi)

# Screenshots (src/hostui.c): the PNG writer of miniz 3.1.2, MIT
# (third_party/miniz, unmodified, also in the launcher), deflate only.
add_library(ssx_miniz STATIC third_party/miniz/miniz.c)
target_include_directories(ssx_miniz PUBLIC third_party/miniz)
target_compile_definitions(ssx_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_INFLATE_APIS
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_link_libraries(${PROJECT_NAME} PRIVATE ssx_miniz)

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
    # Tick -> ns / us conversion of the APU and NV2A clocks (no overflow)
    add_executable(clock_test tests/clock_test.c)
    target_include_directories(clock_test PRIVATE
        ${XBOXRECOMP_DIR}/src/nv2a)
    if(WIN32)
        # The kernel's waits on NT timeouts of any length: the real
        # kernel_sync.c and kernel_thread.c, on a simulated clock
        add_executable(wait_test tests/wait_test.c)
        target_include_directories(wait_test PRIVATE
            ${XBOXRECOMP_DIR}/src/kernel ${XBOXRECOMP_DIR}/src)
        # What NtQueryDirectoryFile writes into the caller's buffer: the
        # real kernel_file.c, a made-up disc directory and a real host one
        add_executable(dir_query_test tests/dir_query_test.c)
        target_include_directories(dir_query_test PRIVATE
            ${XBOXRECOMP_DIR}/src/kernel ${XBOXRECOMP_DIR}/src)
    endif()
endif()

# Host test of the PlayStation controllers and of how controls.c reads them,
# with SDL's virtual pads and a stand-in XInput (no controller needed):
#   pad_sdl_test off (XBOX_PS_PADS unset), pad_sdl_test on (XBOX_PS_PADS=1)
if(SSX_BUILD_TESTS AND SSX_PS_PADS)
    add_executable(pad_sdl_test tests/pad_sdl_test.c src/pad_sdl.c src/controls.c src/settings.c)
    target_include_directories(pad_sdl_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(pad_sdl_test PRIVATE SSX_HAVE_SDL3 XInputGetState=pad_test_XInputGetState
                               XInputSetState=pad_test_XInputSetState)
    target_link_libraries(pad_sdl_test PRIVATE SDL3::SDL3-static shell32 ole32 uuid)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(pad_sdl_test PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endif()

# The extras (extras.cmake): not in the stable build, OFF by default; not in
# the public tree, where this line does nothing.
include(${CMAKE_CURRENT_SOURCE_DIR}/extras.cmake OPTIONAL)
# The developer tools (devtools.cmake): never in a public build, OFF by
# default; not in the public tree either.
include(${CMAKE_CURRENT_SOURCE_DIR}/devtools.cmake OPTIONAL)

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
    # The generated files are very large.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()
