# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    # MinGW's plain "xinput" import library is XInput 1.3 (XINPUT1_3.dll),
    # which Windows 10/11 only have with the old DirectX runtime installed.
    # XInput 1.4 ships with Windows 8 and later. MSVC's xinput.lib is 1.4.
    if(MINGW)
        target_link_libraries(xbox_input PUBLIC platform xinput1_4)
    else()
        target_link_libraries(xbox_input PUBLIC platform xinput)
    endif()
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
