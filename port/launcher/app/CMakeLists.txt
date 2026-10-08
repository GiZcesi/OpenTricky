# OpenTricky.exe -- the launcher: RmlUi on SDL3 + OpenGL 3.
# A separate program; it contains no code of the game. Its pictures, sounds
# and theme are read from the player's own disc image at run time (port/launcher).
#
#   cmake -S port/launcher/app -B <build dir> -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
#   cmake --build <build dir> -j 8
#   <build dir>/OpenTricky.exe [--iso <disc image>] [--track <0-9>] [--page home|settings]
#
# Dependencies are built from the sources in port/third_party (no system
# package is used): SDL3, FreeType (no zlib/png/harfbuzz/brotli), RmlUi.
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

# ── The launcher's disc assets and themes (port/launcher, C99) ──
add_library(ot_launcher_assets STATIC
    ../ot_image.c
    ../ot_disc.c
    ../ot_theme.c
)
target_include_directories(ot_launcher_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

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
# A release build: the version without "-dev" (port/src/version.h), set by the
# release tools only.
option(OT_RELEASE "Release build (version without -dev)" OFF)
if(OT_RELEASE)
    target_compile_definitions(OpenTricky PRIVATE OT_RELEASE=1)
endif()
target_link_libraries(OpenTricky PRIVATE ot_launcher_assets RmlUi::Core SDL3::SDL3-static shell32 ole32 uuid psapi winhttp)
if(MINGW)
    target_link_options(OpenTricky PRIVATE -static -static-libgcc -static-libstdc++ -s)
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
# full (RmlUi MIT, SDL zlib, FreeType FTL, the fonts' OFL).
set(OT_LICENCES "")
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/licenses-head.txt OT_PART)
string(APPEND OT_LICENCES "${OT_PART}")
foreach(part
        "RmlUi (MIT)|${OT_THIRD_PARTY}/rmlui/LICENSE.txt"
        "SDL (zlib)|${OT_THIRD_PARTY}/sdl3/LICENSE.txt"
        "FreeType (FreeType License, FTL)|${OT_THIRD_PARTY}/freetype/docs/FTL.TXT"
        "Inter (SIL Open Font License 1.1)|${CMAKE_CURRENT_SOURCE_DIR}/ui/fonts/OFL-Inter.txt"
        "Bebas Neue (SIL Open Font License 1.1)|${CMAKE_CURRENT_SOURCE_DIR}/ui/fonts/OFL-BebasNeue.txt"
        "Exo 2 (SIL Open Font License 1.1)|${CMAKE_CURRENT_SOURCE_DIR}/ui/fonts/OFL-Exo2.txt")
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
