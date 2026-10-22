# Link probe (MACOS_PORT_PLAN.md 0.1): the xboxrecomp runtime libraries plus a
# dummy recomp_lookup() linked into one arm64 executable, without the disc image.
# Compiling alone hides missing symbols; this lists them.
#
#   cmake -S macos/link_probe -B build/link_probe -G Ninja -DOT_HEADLESS=ON
#   cmake --build build/link_probe -k 0
cmake_minimum_required(VERSION 3.20)
project(ot_link_probe C)
set(CMAKE_C_STANDARD 11)

set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl" CACHE PATH "Homebrew OpenSSL")
set(OT_HEADLESS ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../xboxrecomp ${CMAKE_BINARY_DIR}/xboxrecomp)

# Runtime libraries only, with every object forced in: lists what the runtime expects from
# the translated game (superseded by ot_port_probe below, so not part of `all`).
add_executable(ot_link_probe EXCLUDE_FROM_ALL probe_main.c)
target_link_libraries(ot_link_probe PRIVATE xboxrecomp)

# Pull in every object of every static library, so unresolved references show up
# even though probe_main.c calls nothing.
if(APPLE)
    target_link_options(ot_link_probe PRIVATE -Wl,-all_load -Wl,-undefined,error)
endif()

# Guest-window test (0.3): the real xbox_MemoryLayoutInit on a blank XBE.
add_executable(ot_layout_test ../layout_test/layout_test.c ../layout_test/stubs.c)
target_include_directories(ot_layout_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../port/src/recomp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../xboxrecomp/src)
target_link_libraries(ot_layout_test PRIVATE xboxrecomp)

# MMIO fault-handler test (0.4), at several optimisation levels so the compiler
# picks different AArch64 addressing forms. Only needs the platform library.
foreach(opt O0 O1 O3)
    add_executable(ot_mmio_test_${opt} ../mmio_test/mmio_test.c)
    target_include_directories(ot_mmio_test_${opt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../xboxrecomp/src)
    target_compile_options(ot_mmio_test_${opt} PRIVATE -${opt} -g)
    target_link_libraries(ot_mmio_test_${opt} PRIVATE platform)
endforeach()

# The whole port (every port/src file) linked against the runtime and a stub for the translated
# game: proves the port + runtime + null backend have no unresolved or duplicate symbols.
file(GLOB PORT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../port/src/*.c
                   ${CMAKE_CURRENT_SOURCE_DIR}/../../port/src/netplay/*.c
                   ${CMAKE_CURRENT_SOURCE_DIR}/../../port/src/recomp/*.c)
list(FILTER PORT_SRC EXCLUDE REGEX "/crashreport\\.c$")
add_executable(ot_port_probe ${PORT_SRC} gen_stub.c)
target_include_directories(ot_port_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../port/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../port/src/recomp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../xboxrecomp/include)
target_compile_options(ot_port_probe PRIVATE -w -include ${CMAKE_CURRENT_SOURCE_DIR}/../../xboxrecomp/src/platform/windows.h)
target_compile_definitions(ot_port_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ot_port_probe PRIVATE xboxrecomp)
find_package(SDL3 CONFIG)
if(SDL3_FOUND)
    target_compile_definitions(ot_port_probe PRIVATE OT_HAVE_WINDOW=1)
    target_link_libraries(ot_port_probe PRIVATE SDL3::SDL3)
endif()
if(APPLE)
    # Everything the translated game defines (sub_XXXXXXXX ...) is unresolved here on purpose.
    target_link_options(ot_port_probe PRIVATE -Wl,-undefined,dynamic_lookup)
endif()

# Optional: a file of empty sub_XXXXXXXX() definitions (OT_SUB_STUBS=<file>), so the probe can run far
# enough to exercise the entry code. Without it those symbols stay unresolved (dynamic lookup).
set(OT_SUB_STUBS "" CACHE FILEPATH "stubs for the translated game's functions")
if(OT_SUB_STUBS)
    target_sources(ot_port_probe PRIVATE ${OT_SUB_STUBS})
endif()

# ISO-backed file layer (Phase 1): the POSIX Nt* file backend serving the disc from an XDVDFS image.
# run: ot_iso_test <image.iso>   (python3 macos/iso_test/make_xdvdfs.py out.iso builds a small one)
add_executable(ot_iso_test ../iso_test/iso_test.c ../layout_test/stubs.c)
target_include_directories(ot_iso_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../xboxrecomp/src ${CMAKE_CURRENT_SOURCE_DIR}/../../port/src/recomp)
target_link_libraries(ot_iso_test PRIVATE xboxrecomp)
