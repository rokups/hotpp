# Hand-written generations of the Player class. The tests load them as libraries.
# handwritten_player_gen1: generation 1.
# handwritten_player_gen2: generation 2 (migration).
# handwritten_player_gen4_base: generation 4 (rejected: other shell).
# The tests also use a library that does not exist (gen3_missing.so) on purpose.
foreach(g gen1 gen2 gen4_base)
    add_library(${g} SHARED handwritten_player_${g}.cpp)
    target_include_directories(${g} PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine)
    set_target_properties(${g} PROPERTIES PREFIX "")
endforeach()
