# WindowsUtf8Manifest.cmake
# Embeds the UTF-8 active-code-page manifest in a Windows executable.
#
# See cmake/windows/orc-utf8.manifest for why this is needed. Only executables
# need it: a DLL (stage plugins included) inherits the code page of the process
# that loads it.
#
# No-op on every non-Windows platform, where narrow paths are already
# byte-transparent UTF-8.

set(ORC_WINDOWS_UTF8_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/windows/orc-utf8.manifest"
    CACHE INTERNAL "Path to the UTF-8 code page application manifest")

function(orc_apply_utf8_manifest target)
    if(NOT WIN32)
        return()
    endif()

    if(MSVC)
        # CMake passes a .manifest source to the linker as /MANIFESTINPUT and
        # embeds the merged result, so this composes with the default manifest
        # the linker generates rather than replacing it.
        target_sources(${target} PRIVATE "${ORC_WINDOWS_UTF8_MANIFEST}")
    elseif(MINGW)
        # windres has no manifest input, so go through a resource script.
        # 1 = CREATEPROCESS_MANIFEST_RESOURCE_ID, 24 = RT_MANIFEST; the numeric
        # form avoids needing winuser.h. Forward slashes are intentional:
        # backslashes are escapes in an .rc string literal.
        set(_manifest_rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_utf8_manifest.rc")
        file(TO_CMAKE_PATH "${ORC_WINDOWS_UTF8_MANIFEST}" _manifest_path)
        file(WRITE "${_manifest_rc}" "1 24 \"${_manifest_path}\"\n")
        target_sources(${target} PRIVATE "${_manifest_rc}")
    else()
        message(WARNING
            "Unknown Windows toolchain: no UTF-8 manifest embedded in ${target}. "
            "File paths containing non-ASCII characters will fail to open.")
    endif()
endfunction()
