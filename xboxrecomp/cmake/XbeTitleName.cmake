# Read a title's name out of its XBE certificate, at configure time.
#
# An XBE carries the game's real name in its certificate: the certificate's
# virtual address sits at header+0x118, rebased through the image base at
# header+0x104, and the name is 40 UTF-16 characters at certificate+0x0C.
#
# This is worth doing rather than hardcoding a name, because the recompiler is
# a template: whatever game it is pointed at should produce an executable
# called after that game, not after the template.  A build that emits
# "your_game_recomp.exe" tells the person running it nothing.
#
#   xbe_title_name(<out-var> <path-to-xbe> [FALLBACK <name>])
#
# Sets <out-var> to the title name with characters that are awkward in
# filenames removed.  Falls back quietly if the file is missing or does not
# look like an XBE, so a checkout without game data still configures.

function(xbe_title_name OUT_VAR XBE_PATH)
    cmake_parse_arguments(ARG "" "FALLBACK" "" ${ARGN})
    set(_fallback "${ARG_FALLBACK}")
    if(NOT _fallback)
        set(_fallback "xbox_recomp")
    endif()

    if(NOT EXISTS "${XBE_PATH}")
        set(${OUT_VAR} "${_fallback}" PARENT_SCOPE)
        return()
    endif()

    # Header: magic "XBEH", image base at 0x104, certificate VA at 0x118.
    file(READ "${XBE_PATH}" _hdr HEX LIMIT 288)
    string(SUBSTRING "${_hdr}" 0 8 _magic)
    if(NOT _magic STREQUAL "58424548")          # "XBEH"
        set(${OUT_VAR} "${_fallback}" PARENT_SCOPE)
        return()
    endif()

    # Little-endian dword at byte offset N -> hex chars [2N, 2N+8)
    function(_le32 OUT HEXSTR BYTEOFF)
        math(EXPR _c "${BYTEOFF} * 2")
        string(SUBSTRING "${HEXSTR}" ${_c} 8 _q)
        string(SUBSTRING "${_q}" 0 2 _b0)
        string(SUBSTRING "${_q}" 2 2 _b1)
        string(SUBSTRING "${_q}" 4 2 _b2)
        string(SUBSTRING "${_q}" 6 2 _b3)
        set(${OUT} "0x${_b3}${_b2}${_b1}${_b0}" PARENT_SCOPE)
    endfunction()

    _le32(_base "${_hdr}" 260)                  # 0x104
    _le32(_certva "${_hdr}" 280)                # 0x118
    math(EXPR _certoff "${_certva} - ${_base}")
    if(_certoff LESS 0)
        set(${OUT_VAR} "${_fallback}" PARENT_SCOPE)
        return()
    endif()

    # Re-read far enough to cover the certificate's name field.
    math(EXPR _need "${_certoff} + 92")
    file(READ "${XBE_PATH}" _cert HEX LIMIT ${_need})
    math(EXPR _nameoff "(${_certoff} + 12) * 2")

    set(_name "")
    foreach(_i RANGE 39)
        math(EXPR _o "${_nameoff} + ${_i} * 4")
        string(SUBSTRING "${_cert}" ${_o} 2 _lo)
        if(_lo STREQUAL "00")
            break()
        endif()
        # ASCII range only; anything else becomes a space and is trimmed below.
        string(TOUPPER "${_lo}" _lo)
        set(_ch "")
        foreach(_pair "20; " "21;!" "26;&" "27;'" "28;(" "29;)" "2C;," "2D;-"
                      "2E;." "30;0" "31;1" "32;2" "33;3" "34;4" "35;5" "36;6"
                      "37;7" "38;8" "39;9" "3A;-" "41;A" "42;B" "43;C" "44;D"
                      "45;E" "46;F" "47;G" "48;H" "49;I" "4A;J" "4B;K" "4C;L"
                      "4D;M" "4E;N" "4F;O" "50;P" "51;Q" "52;R" "53;S" "54;T"
                      "55;U" "56;V" "57;W" "58;X" "59;Y" "5A;Z" "5F;_"
                      "61;a" "62;b" "63;c" "64;d" "65;e" "66;f" "67;g" "68;h"
                      "69;i" "6A;j" "6B;k" "6C;l" "6D;m" "6E;n" "6F;o" "70;p"
                      "71;q" "72;r" "73;s" "74;t" "75;u" "76;v" "77;w" "78;x"
                      "79;y" "7A;z")
            string(REPLACE ";" "|" _p "${_pair}")
            string(SUBSTRING "${_p}" 0 2 _code)
            if(_code STREQUAL _lo)
                string(SUBSTRING "${_p}" 3 1 _ch)
                break()
            endif()
        endforeach()
        set(_name "${_name}${_ch}")
    endforeach()

    string(STRIP "${_name}" _name)
    if(_name STREQUAL "")
        set(_name "${_fallback}")
    endif()
    set(${OUT_VAR} "${_name}" PARENT_SCOPE)
endfunction()
