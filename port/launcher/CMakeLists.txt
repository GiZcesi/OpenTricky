# The OpenTricky launcher's disc assets and themes: a static library with no
# dependency on the game (portable C99), plus a host preview tool.
#
#   cmake -S port/launcher -B <build dir> -G "MinGW Makefiles"
#   cmake --build <build dir>
#   <build dir>/ot_assets_preview <disc image> <output folder> [texture pack folder]
cmake_minimum_required(VERSION 3.16)
project(ot_launcher_assets C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(ot_launcher_assets STATIC
    ot_image.c
    ot_disc.c
    ot_theme.c
)
target_include_directories(ot_launcher_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT MSVC)
    target_compile_options(ot_launcher_assets PRIVATE -Wall -Wextra)
    target_link_libraries(ot_launcher_assets PUBLIC m)
endif()

add_executable(ot_assets_preview tools/ot_assets_preview.c tools/png_write.c)
target_link_libraries(ot_assets_preview PRIVATE ot_launcher_assets)
