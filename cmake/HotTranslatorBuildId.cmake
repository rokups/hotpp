# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Writes the build id of the translator (hotc::translator_build_id) to OUT. Script mode:
#   cmake -DLIST=<file with one source path in each line> -DROOT=<source dir> -DOUT=<header> -P this
# The id is the SHA-256 of the relative path and the SHA-256 of each source. The header changes
# only when the id changes, so an unchanged id does not compile build_id.cpp again.
file(STRINGS "${LIST}" _files)
list(SORT _files)
set(_all "")
foreach(_f IN LISTS _files)
    file(SHA256 "${_f}" _h)
    file(RELATIVE_PATH _rel "${ROOT}" "${_f}")
    string(APPEND _all "${_rel} ${_h}\n")
endforeach()
string(SHA256 _id "${_all}")
set(_text "#define HOT_TRANSLATOR_SOURCE_ID \"${_id}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" _old)
    if(_old STREQUAL _text)
        return()
    endif()
endif()
file(WRITE "${OUT}" "${_text}")
