# ------------------------------------------------------------ Dear ImGui example
# imgui_app draws a Dear ImGui user interface from a script. It needs SDL3.
# Without SDL3 the target does not exist and the rest of the build is unchanged.
find_package(SDL3 QUIET)
if(SDL3_FOUND AND NOT CMAKE_VERSION VERSION_LESS 3.24)
    CPMAddPackage(NAME imgui GITHUB_REPOSITORY ocornut/imgui GIT_TAG v1.92.9b DOWNLOAD_ONLY YES)
    add_library(imgui_sdl3 STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(imgui_sdl3 PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui_sdl3 PUBLIC SDL3::SDL3)

    # The host builds ClangFilesOptions itself. It does not use make_default_backend().
    add_executable(imgui_app main.cpp UiApp.cpp)
    target_include_directories(imgui_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Whole archive: a script can call any ImGui function. The linker must keep all objects.
    target_link_libraries(imgui_app PRIVATE spp_runtime spp_clang_backend "$<LINK_LIBRARY:WHOLE_ARCHIVE,imgui_sdl3>")
    set_target_properties(imgui_app PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(imgui_app PRIVATE
        IMGUI_APP_CXX="${CMAKE_CXX_COMPILER}"
        IMGUI_APP_CXXFLAGS="${SPP_CXXFLAGS_JOINED}"
        IMGUI_APP_INCLUDES="${CMAKE_SOURCE_DIR}/include|${CMAKE_SOURCE_DIR}/example/engine|${CMAKE_SOURCE_DIR}/example/imgui|${imgui_SOURCE_DIR}"
        IMGUI_APP_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
        IMGUI_APP_WORK_DIR="${SPP_SCRATCH_DIR}/imgui"
        IMGUI_APP_DEFAULT_SCRIPT="${CMAKE_SOURCE_DIR}/example/imgui/demo.spp")
    # The language server reads this file. The engine dir is in IMGUI_APP_INCLUDES.
    spp_script_host(NAME imgui_app TARGET imgui_app
        SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/example/imgui
        INCLUDES ${CMAKE_SOURCE_DIR}/example/engine)
else()
    message(STATUS "SDL3 or CMake 3.24 not found: the target imgui_app is not built")
endif()
