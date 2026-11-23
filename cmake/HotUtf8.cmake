# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# hot_utf8_manifest(<target>)
#
# On Windows the executable gets a manifest that sets the process code page to UTF-8. Then
# std::filesystem, std::string paths and argv use UTF-8. Other platforms: no effect.
# - MSVC and clang-cl: the linker embeds a ".manifest" source file.
# - MinGW: the linker does not read a manifest, so a resource script holds it (windres).
set(HOT_UTF8_DIR ${CMAKE_CURRENT_LIST_DIR})
if(MINGW)
    enable_language(RC)
endif()

function(hot_utf8_manifest target)
    if(MSVC)
        target_sources(${target} PRIVATE "${HOT_UTF8_DIR}/hot_utf8.manifest")
    elseif(MINGW)
        target_sources(${target} PRIVATE "${HOT_UTF8_DIR}/hot_utf8.rc")
        target_include_directories(${target} PRIVATE "${HOT_UTF8_DIR}")
        set_source_files_properties("${HOT_UTF8_DIR}/hot_utf8.rc" PROPERTIES OBJECT_DEPENDS "${HOT_UTF8_DIR}/hot_utf8.manifest")
    endif()
endfunction()
