# ------------------------------------------------------------ Shipping mode (M7)
# ship_demo has the C++ of a script in the binary. It links spp_ship and not spp_runtime, so it has
# no loader, no migration, no dlopen and no Clang. Actor.cpp and Util.cpp are the engine.
spp_add_shipped_scripts(TARGET ship_demo_scripts SCRIPTS ${CMAKE_SOURCE_DIR}/example/scripts/ship_demo.spp
                        INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                        DEPENDS ${SPP_HEADERS})
add_executable(ship_demo main.cpp ${CMAKE_SOURCE_DIR}/example/engine/Actor.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Util.cpp)
target_include_directories(ship_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ship_demo PRIVATE ship_demo_scripts)
