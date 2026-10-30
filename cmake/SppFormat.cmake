# The format library of the string interpolation "${...}". The choice is a build-time choice.
#
#   C++17 (the standard is less than 20): fmt. std::format needs C++20.
#   C++20 and later:                      std::format, unless SPP_USE_FMT is ON.
#
# SPP_USE_FMT=ON selects fmt at any standard. No runtime switch exists.
# The cache variable SPP_FMT_PROVIDER says where fmt comes from:
#   AUTO   (default) find_package(fmt) first, then CPM.cmake fetches fmt.
#   SYSTEM find_package(fmt REQUIRED) only. The build never fetches.
#   CPM    CPM.cmake always fetches fmt.
#   NONE   The host project supplies the target fmt::fmt-header-only or fmt::fmt. The build
#          only links to it and stops with an error if the target does not exist.
# A target fmt::fmt-header-only or fmt::fmt that exists already is always used. The build then
# does not search and does not fetch, with any value of SPP_FMT_PROVIDER.
option(SPP_USE_FMT "Use fmt, not std::format, for string interpolation (C++20 and later)" OFF)
set(SPP_FMT_PROVIDER AUTO CACHE STRING "Where fmt comes from: AUTO, SYSTEM, CPM or NONE")
set_property(CACHE SPP_FMT_PROVIDER PROPERTY STRINGS AUTO SYSTEM CPM NONE)

set(_SPP_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(SPP_FMT_VERSION 11.2.0)

# spp_format_library(<out> <cxx standard>): set <out> to "fmt" or "std".
function(spp_format_library out std)
    if(std STREQUAL "" OR std MATCHES "[^0-9]")
        set(std 17)
    endif()
    if(SPP_USE_FMT OR std LESS 20)
        set(${out} fmt PARENT_SCOPE)
    else()
        set(${out} std PARENT_SCOPE)
    endif()
endfunction()

# spp_effective_cxx_standard(<out> <target>): set <out> to the C++ standard of the host: 17, 20
# or 23. The value is the property CXX_STANDARD of the target, else CMAKE_CXX_STANDARD, else
# the default of S++ (20, set by the root CMakeLists.txt), else 17.
# A value less than 17 gives 17. A value above 23 gives 23. The value 21 or 22 gives 20.
# The runtime library and the backends compile the generated code with this standard
# (HOT_CXX_STANDARD in include/hot/config.h).
function(spp_effective_cxx_standard out target)
    set(std "")
    if(TARGET ${target})
        get_target_property(std ${target} CXX_STANDARD)
    endif()
    if(NOT std OR std MATCHES "NOTFOUND")
        set(std "${CMAKE_CXX_STANDARD}")
    endif()
    if(std STREQUAL "")
        # S++ chose its own default (root CMakeLists.txt) and the parent project has none.
        get_property(std GLOBAL PROPERTY SPP_DEFAULT_CXX_STANDARD)
    endif()
    if(std STREQUAL "" OR std MATCHES "[^0-9]" OR std LESS 17)
        set(std 17)
    elseif(std GREATER 23)
        set(std 23)
    elseif(std EQUAL 21 OR std EQUAL 22)
        set(std 20)
    endif()
    set(${out} ${std} PARENT_SCOPE)
endfunction()

# spp_provide_fmt(): make the target spp_fmt. It links to the fmt target that the provider gives.
# The function does the work one time. The global property SPP_FMT_INCLUDE_DIR has the include
# directory of fmt, or an empty text when the compiler finds the directory by itself.
function(spp_provide_fmt)
    if(TARGET spp_fmt)
        return()
    endif()
    if(NOT SPP_FMT_PROVIDER MATCHES "^(AUTO|SYSTEM|CPM|NONE)$")
        message(FATAL_ERROR "SPP_FMT_PROVIDER must be AUTO, SYSTEM, CPM or NONE. It is: ${SPP_FMT_PROVIDER}")
    endif()

    set(source "an existing target")
    if(NOT TARGET fmt::fmt-header-only AND NOT TARGET fmt::fmt)
        if(SPP_FMT_PROVIDER STREQUAL "NONE")
            message(FATAL_ERROR "SPP_FMT_PROVIDER is NONE, but the target fmt::fmt-header-only or "
                                "fmt::fmt does not exist. Create the target before you add spp.")
        elseif(SPP_FMT_PROVIDER STREQUAL "SYSTEM")
            find_package(fmt REQUIRED)
            set(source "an installed package")
        elseif(SPP_FMT_PROVIDER STREQUAL "AUTO")
            find_package(fmt QUIET)
            set(source "an installed package")
        endif()
        if(NOT TARGET fmt::fmt-header-only AND NOT TARGET fmt::fmt)
            # CPM and AUTO without an installed package. CPM does not search again.
            if(NOT COMMAND CPMAddPackage)
                include(${_SPP_CMAKE_DIR}/CPM.cmake)
            endif()
            set(CPM_USE_LOCAL_PACKAGES OFF)
            CPMAddPackage(
                NAME fmt
                GITHUB_REPOSITORY fmtlib/fmt
                GIT_TAG ${SPP_FMT_VERSION}
                VERSION ${SPP_FMT_VERSION}
                OPTIONS "FMT_INSTALL OFF" "FMT_DOC OFF" "FMT_TEST OFF"
            )
            set(source "CPM.cmake")
        endif()
    endif()

    if(TARGET fmt::fmt-header-only)
        set(real fmt::fmt-header-only)
    elseif(TARGET fmt::fmt)
        set(real fmt::fmt)
    else()
        message(FATAL_ERROR "SPP: fmt is not available. Check SPP_FMT_PROVIDER (now ${SPP_FMT_PROVIDER}).")
    endif()
    # An imported target is local to the directory. Make it visible everywhere.
    get_target_property(imported ${real} IMPORTED)
    if(imported)
        set_target_properties(${real} PROPERTIES IMPORTED_GLOBAL TRUE)
    endif()
    add_library(spp_fmt INTERFACE)
    target_link_libraries(spp_fmt INTERFACE ${real})

    set(dir "")
    if(source STREQUAL "CPM.cmake" AND fmt_SOURCE_DIR)
        set(dir "${fmt_SOURCE_DIR}/include")
    else()
        get_target_property(dirs ${real} INTERFACE_INCLUDE_DIRECTORIES)
        if(dirs)
            list(GET dirs 0 dir)
        endif()
    endif()
    # A default include directory needs no -I. The option -I of such a directory can break the
    # search order of the standard headers. A generator expression cannot be used here.
    if(dir MATCHES "\\$<" OR dir STREQUAL "/usr/include" OR dir STREQUAL "/usr/local/include")
        set(dir "")
    endif()
    set_property(GLOBAL PROPERTY SPP_FMT_INCLUDE_DIR "${dir}")
    message(STATUS "fmt for string interpolation: ${real} from ${source}")
endfunction()
