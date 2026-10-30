# Hand-written generations of the Player class. The tests load them as libraries.
# handwritten_player_gen1: generation 1.
# handwritten_player_gen2: generation 2 (migration).
# handwritten_player_gen4_base: generation 4 (rejected: other shell).
# The tests also use a library that does not exist (gen3_missing.so) on purpose.
foreach(g gen1 gen2 gen4_base)
    add_library(${g} SHARED handwritten_player_${g}.cpp)
    target_include_directories(${g} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/engine)
    set_target_properties(${g} PROPERTIES PREFIX "")
endforeach()
# stress_generation: the class Pulse with the generation number of the load (reload stress test).
add_library(stress_generation SHARED stress_generation.cpp)
target_include_directories(stress_generation PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/engine)
set_target_properties(stress_generation PROPERTIES PREFIX "")
