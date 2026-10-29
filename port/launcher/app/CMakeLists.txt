# OpenTricky.exe -- the launcher: RmlUi on SDL3 + OpenGL 3.
# A separate program; it contains no code of the game. It builds on its own,
# with the two commands below.
#
#   cmake -S port/launcher/app -B <build dir> -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
#   cmake --build <build dir> -j 8
#   <build dir>/OpenTricky.exe [--iso <disc image>] [--track <0-9>] [--page home|settings]
#
# Dependencies are built from the sources in port/third_party (no system
# package is used): SDL3, FreeType (no zlib/png/harfbuzz/brotli), RmlUi, miniz.
cmake_minimum_required(VERSION 3.20)
project(OpenTricky C CXX RC)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(OT_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)
# The settings registry and the version are shared with the game (port/src).
set(OT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

# Never pick up an installed copy of a dependency (MSYS2 has SDL3 and
# FreeType): only the vendored sources below.
set(CMAKE_DISABLE_FIND_PACKAGE_Freetype TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_SDL3 TRUE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ── SDL3 (static) ──
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
add_subdirectory(${OT_THIRD_PARTY}/sdl3 ${CMAKE_BINARY_DIR}/sdl3 EXCLUDE_FROM_ALL)

# ── FreeType (static, no optional dependency) ──
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
add_subdirectory(${OT_THIRD_PARTY}/freetype ${CMAKE_BINARY_DIR}/freetype EXCLUDE_FROM_ALL)
add_library(Freetype::Freetype ALIAS freetype)

# ── RmlUi (static core + debugger off) ──
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(${OT_THIRD_PARTY}/rmlui ${CMAKE_BINARY_DIR}/rmlui EXCLUDE_FROM_ALL)

# ── miniz (static): the zip reader and inflate of the updates only (no
# deflate, so no zip writing either) ──
add_library(ot_miniz STATIC ${OT_THIRD_PARTY}/miniz/miniz.c)
target_include_directories(ot_miniz PUBLIC ${OT_THIRD_PARTY}/miniz)
target_compile_definitions(ot_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_DEFLATE_APIS
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# ── The launcher's disc assets and themes (port/launcher, C99) ──
add_library(ot_launcher_assets STATIC
    ../ot_image.c
    ../ot_disc.c
    ../ot_theme.c
)
target_include_directories(ot_launcher_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ── The release signature: Ed25519 (Monocypher 4.0.2, C99) ──
add_library(ot_sign STATIC
    ot_sign.c
    ${OT_THIRD_PARTY}/monocypher/monocypher.c
    ${OT_THIRD_PARTY}/monocypher/optional/monocypher-ed25519.c
)
target_include_directories(ot_sign PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${OT_THIRD_PARTY}/monocypher)

# ── OpenTricky.exe ──
set(RMLUI_BACKENDS ${OT_THIRD_PARTY}/rmlui/Backends)
add_executable(OpenTricky WIN32
    main.cpp
    ot_render.cpp
    ot_util.cpp
    ot_update.cpp
    ot_install.cpp
    ${OT_SRC}/settings.c
    OpenTricky.rc
    ${RMLUI_BACKENDS}/RmlUi_Platform_SDL.cpp
    ${RMLUI_BACKENDS}/RmlUi_Renderer_GL3.cpp
)
target_include_directories(OpenTricky PRIVATE ${RMLUI_BACKENDS} ${CMAKE_CURRENT_SOURCE_DIR} ${OT_SRC}
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
# The icon (OpenTricky.ico, made by make_icon.py) and the version resource
# are linked by OpenTricky.rc; windres finds opentricky_version.rc in OT_SRC.
set_source_files_properties(OpenTricky.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/OpenTricky.ico;${OT_SRC}/opentricky_version.rc;${OT_SRC}/version_info.rc;${OT_SRC}/version.h")
target_compile_definitions(OpenTricky PRIVATE RMLUI_SDL_VERSION_MAJOR=3)
# Smoother edges of the shapes (slanted PLAY and title boxes, rounded corners,
# borders): 4 samples instead of the GL3 renderer's 2.
target_compile_definitions(OpenTricky PRIVATE RMLUI_NUM_MSAA_SAMPLES=8)
# A release build: the version without "-dev" (port/src/version.h), set by the
# release tools only.
option(OT_RELEASE "Release build (version without -dev)" OFF)
if(OT_RELEASE)
    target_compile_definitions(OpenTricky PRIVATE OT_RELEASE=1)
endif()
# Pages of the private test builds (online_pages.cmake): not in the public
# tree, where this line does nothing. Such a build may link another miniz.
set(OT_MINIZ_LIB ot_miniz)
include(${CMAKE_CURRENT_SOURCE_DIR}/online_pages.cmake OPTIONAL)
# The extras' settings (port/extras.cmake): off by default; not in the public tree.
include(${OT_SRC}/../extras.cmake OPTIONAL)
target_link_libraries(OpenTricky PRIVATE ot_launcher_assets ${OT_MINIZ_LIB} ot_sign RmlUi::Core SDL3::SDL3-static shell32 ole32 uuid psapi winhttp)
if(MINGW)
    target_link_options(OpenTricky PRIVATE -static -static-libgcc -static-libstdc++ -s)
endif()

# Host test of the zip reader of the updates (port/launcher/tests), off by
# default: -DOT_BUILD_TESTS=ON, then the ot_unzip_test target.
option(OT_BUILD_TESTS "Build the launcher's host tests (ot_unzip_test)" OFF)
if(OT_BUILD_TESTS)
    add_executable(ot_unzip_test ../tests/unzip_test.cpp ot_install.cpp ot_update.cpp ot_util.cpp)
    target_include_directories(ot_unzip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ot_unzip_test PRIVATE ot_miniz ot_sign SDL3::SDL3-static shell32 ole32 uuid psapi winhttp)
    if(MINGW)
        target_link_options(ot_unzip_test PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()

# The installed version's "What's new" block (whatsnew.txt), built into the
# exe so the home page shows it without a network.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/whatsnew.txt ${CMAKE_CURRENT_SOURCE_DIR}/licenses-head.txt)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/whatsnew.txt OT_WHATSNEW)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/ot_whatsnew.inc
    CONTENT "R\"OTNOTES(${OT_WHATSNEW})OTNOTES\"
" @ONLY)

# THIRD-PARTY-LICENSES.txt beside the exe: our summary, then each licence in
# full (RmlUi MIT with robin_hood and itlib, and khrplatform.h of its OpenGL
# loader; SDL zlib with yuv2rgb, hidapi, stb_image and its own miniz 1.15;
# FreeType FTL, its LICENSE.TXT and the notices of its BDF and PCF drivers, of
# fthash.c and of the zlib of its gzip module; miniz MIT, Monocypher, and what
# the toolchain puts in: the MinGW-w64 runtime, winpthreads, and the GCC
# Runtime Library Exception of libgcc, libstdc++ and GCC's headers; the fonts'
# OFL), then those of the game's code (xboxrecomp MIT, SMAA MIT, bcdec, the
# parts adapted from xemu), and last OpenTricky's own licence (the GNU GPL 3.0
# of the repository's LICENSE, the GPL of the GCC runtime too), each read from
# the repository's own copy. A notice that is only a passage of a third-party
# file, or the toolchain's own file of the GCC exception, is read from its copy
# in port/third_party/notices (port/third_party/VERSIONS.txt).
set(OT_LICENCES "")
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/licenses-head.txt OT_PART)
string(APPEND OT_LICENCES "${OT_PART}")
foreach(part
        "RmlUi (MIT)|${OT_THIRD_PARTY}/rmlui/LICENSE.txt"
        "robin_hood and itlib (MIT), in RmlUi|${OT_THIRD_PARTY}/rmlui/Include/RmlUi/Core/Containers/LICENSE.txt"
        "khrplatform.h (Khronos, MIT-style), in RmlUi's OpenGL loader|${OT_THIRD_PARTY}/notices/khrplatform.txt"
        "SDL (zlib)|${OT_THIRD_PARTY}/sdl3/LICENSE.txt"
        "yuv2rgb (BSD 3-clause), in SDL|${OT_THIRD_PARTY}/sdl3/src/video/yuv2rgb/LICENSE"
        "hidapi (the original HIDAPI licence, of the three it offers), in SDL|${OT_THIRD_PARTY}/sdl3/src/hidapi/LICENSE-orig.txt"
        "stb_image (MIT, or Unlicense), in SDL|${OT_THIRD_PARTY}/notices/sdl-stb-image.txt"
        "miniz 1.15 (Unlicense, public domain), in SDL|${OT_THIRD_PARTY}/notices/sdl-miniz.txt"
        "FreeType (FreeType License, FTL)|${OT_THIRD_PARTY}/freetype/docs/FTL.TXT"
        "FreeType's LICENSE.TXT (the FTL, and its parts under other licences)|${OT_THIRD_PARTY}/freetype/LICENSE.TXT"
        "BDF driver (X11-style), in FreeType|${OT_THIRD_PARTY}/notices/freetype-bdf.txt"
        "PCF driver (X11-style), in FreeType|${OT_THIRD_PARTY}/notices/freetype-pcf.txt"
        "pcfutil.c of the PCF driver (X11, The Open Group), in FreeType|${OT_THIRD_PARTY}/notices/freetype-pcfutil.txt"
        "fthash.c (X11-style), in FreeType|${OT_THIRD_PARTY}/notices/freetype-fthash.txt"
        "zlib (zlib licence), in FreeType's gzip module|${OT_THIRD_PARTY}/notices/freetype-zlib.txt"
        "miniz (MIT)|${OT_THIRD_PARTY}/miniz/LICENSE"
        "Monocypher (BSD 2-clause, or CC0)|${OT_THIRD_PARTY}/monocypher/LICENCE.md"
        "MinGW-w64 runtime (ZPL 2.1 and others), in the launcher and the game|${OT_THIRD_PARTY}/mingw-w64/crt/COPYING.MinGW-w64-runtime.txt"
        "winpthreads (MIT and BSD 3-clause)|${OT_THIRD_PARTY}/mingw-w64/winpthreads/COPYING"
        "GCC runtime: libgcc, libstdc++ and GCC's headers (GNU GPL 3.0 or later, with this exception)|${OT_THIRD_PARTY}/notices/gcc-runtime-exception.txt"
        "Inter (SIL Open Font License 1.1)|${CMAKE_CURRENT_SOURCE_DIR}/ui/fonts/OFL-Inter.txt"
        "Exo 2 (SIL Open Font License 1.1)|${CMAKE_CURRENT_SOURCE_DIR}/ui/fonts/OFL-Exo2.txt"
        "xboxrecomp (MIT), in the game|${CMAKE_CURRENT_SOURCE_DIR}/../../../xboxrecomp/LICENSE"
        "SMAA (MIT), in the game|${CMAKE_CURRENT_SOURCE_DIR}/../../../xboxrecomp/src/d3d/smaa/LICENSE.txt")
    string(REPLACE "|" ";" part "${part}")
    list(GET part 0 title)
    list(GET part 1 file)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${file})
    file(READ ${file} OT_PART)
    string(APPEND OT_LICENCES "
==============================================================================
${title}
==============================================================================

${OT_PART}
")
endforeach()

# The other third-party code compiled into the game, read from its own files:
# bcdec, whose licence is the comment that ends bcdec.h; and the parts adapted
# from xemu, the files of xboxrecomp/src whose header says LGPL (found here, so
# a new one is listed by itself): their names, their copyright lines and the
# permission notice of their headers.
set(OT_XBR ${CMAKE_CURRENT_SOURCE_DIR}/../../../xboxrecomp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OT_XBR}/src/nv2a/bcdec/bcdec.h ${OT_XBR}/src/apu/apu_core.c)
file(READ ${OT_XBR}/src/nv2a/bcdec/bcdec.h OT_PART)
string(FIND "${OT_PART}" "/* LICENSE:\n" OT_AT REVERSE)
if(OT_AT LESS 0)
    message(FATAL_ERROR "bcdec.h: its licence comment (/* LICENSE:) is not found")
endif()
string(SUBSTRING "${OT_PART}" ${OT_AT} -1 OT_PART)
string(REGEX REPLACE "^/\\* LICENSE:\n+" "" OT_PART "${OT_PART}")
string(REGEX REPLACE "\n*\\*/[ \t\r\n]*$" "\n" OT_PART "${OT_PART}")
string(APPEND OT_LICENCES "
==============================================================================
bcdec (MIT, or Unlicense), in the game
==============================================================================

${OT_PART}")

file(GLOB_RECURSE OT_SRC LIST_DIRECTORIES false ${OT_XBR}/src/*.c ${OT_XBR}/src/*.h)
list(SORT OT_SRC)
set(OT_LGPL_FILES "")
set(OT_LGPL_COPY "")
foreach(f ${OT_SRC})
    file(STRINGS ${f} OT_HIT REGEX "Lesser General Public|LGPL" LIMIT_COUNT 1)
    if(OT_HIT)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${f})
        file(RELATIVE_PATH OT_REL ${OT_XBR}/.. ${f})
        string(APPEND OT_LGPL_FILES "  ${OT_REL}\n")
        file(STRINGS ${f} OT_C REGEX "^ \\* Copyright \\(c\\) ")
        list(APPEND OT_LGPL_COPY ${OT_C})
    endif()
endforeach()
if(OT_LGPL_FILES STREQUAL "")
    message(FATAL_ERROR "no file of xboxrecomp/src says LGPL: the xemu notice would be empty")
endif()
list(REMOVE_DUPLICATES OT_LGPL_COPY)
list(SORT OT_LGPL_COPY)
string(REPLACE ";" "\n" OT_LGPL_COPY "${OT_LGPL_COPY}")
string(REGEX REPLACE "(^|\n) \\* " "\\1" OT_LGPL_COPY "${OT_LGPL_COPY}")
file(READ ${OT_XBR}/src/apu/apu_core.c OT_PART)
set(OT_TAIL "<http://www.gnu.org/licenses/>.")
string(LENGTH "${OT_TAIL}" OT_LEN)
string(FIND "${OT_PART}" "This library is free software" OT_AT)
string(FIND "${OT_PART}" "${OT_TAIL}" OT_END)
if(OT_AT LESS 0 OR OT_END LESS OT_AT)
    message(FATAL_ERROR "apu_core.c: the LGPL permission notice of its header is not found")
endif()
math(EXPR OT_LEN "${OT_END} + ${OT_LEN} - ${OT_AT}")
string(SUBSTRING "${OT_PART}" ${OT_AT} ${OT_LEN} OT_PART)
string(REGEX REPLACE "\n \\*[ ]?" "\n" OT_PART "${OT_PART}")
string(APPEND OT_LICENCES "
==============================================================================
Parts adapted from xemu (GNU LGPL), in the game
==============================================================================

The game's code adapted from xemu (https://xemu.app), in these files of its
source, stays under the GNU Lesser General Public License, as their headers
say (the notice below):
${OT_LGPL_FILES}
${OT_LGPL_COPY}

${OT_PART}
")
# ... and the licence itself, the text published by the FSF, kept in the
# repository as xboxrecomp/LICENSE-LGPL-2.1.txt.
set(OT_LGPL ${OT_XBR}/LICENSE-LGPL-2.1.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OT_LGPL})
file(READ ${OT_LGPL} OT_PART)
string(APPEND OT_LICENCES "
==============================================================================
GNU Lesser General Public License, version 2.1 (the parts adapted from xemu)
==============================================================================

${OT_PART}")
# Last, OpenTricky's own licence: the GNU GPL 3.0 of the LICENSE at the root
# of the repository (licenses-head.txt says that its full text is at the end
# of the file).
set(OT_GPL ${CMAKE_CURRENT_SOURCE_DIR}/../../../LICENSE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OT_GPL})
file(READ ${OT_GPL} OT_PART)
string(APPEND OT_LICENCES "
==============================================================================
GNU General Public License, version 3 (OpenTricky's own code)
==============================================================================

${OT_PART}")
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/THIRD-PARTY-LICENSES.txt CONTENT "${OT_LICENCES}" @ONLY)

# The interface files (markup, style sheet, fonts) beside the exe, copied at
# every build (they change without the exe being relinked), with the button
# pictures of the Button style preview (port/assets/btnicons) and the licences.
set(OT_ICONS ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/btnicons)
add_custom_target(OpenTricky_ui ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/ui $<TARGET_FILE_DIR:OpenTricky>/ui
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${OT_ICONS}/juliocacko $<TARGET_FILE_DIR:OpenTricky>/ui/icons/jc
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${OT_ICONS}/ps2 $<TARGET_FILE_DIR:OpenTricky>/ui/icons/ps2
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_BINARY_DIR}/THIRD-PARTY-LICENSES.txt
            $<TARGET_FILE_DIR:OpenTricky>/THIRD-PARTY-LICENSES.txt)
add_dependencies(OpenTricky OpenTricky_ui)
