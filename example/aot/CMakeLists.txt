# ------------------------------------------------------------ AOT compilation (M7)
# aot_demo has the C++ of a script in the binary. It links spp_aot and not spp_runtime, so it has
# no loader, no migration, no dlopen and no Clang. Actor.cpp and Util.cpp are the engine.
spp_add_aot_scripts(TARGET aot_demo_scripts SCRIPTS ${CMAKE_SOURCE_DIR}/example/scripts/aot_demo.spp
                        INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                        DEPENDS ${SPP_HEADERS})
add_executable(aot_demo main.cpp ${CMAKE_SOURCE_DIR}/example/engine/Actor.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Util.cpp)
target_include_directories(aot_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aot_demo PRIVATE aot_demo_scripts)
