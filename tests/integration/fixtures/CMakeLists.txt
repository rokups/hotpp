# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Hand-written generations of the Player class. The tests load them as libraries.
# handwritten_player_gen1: generation 1.
# handwritten_player_gen2: generation 2 (migration).
# handwritten_player_gen4_base: generation 4 (rejected: other shell).
# The tests also use a library that does not exist (gen3_missing.so) on purpose.
# Windows: a fixture DLL links the import library of the test that loads it (HOT_MODULE_BUILD
# makes HOT_EXPORT a dllimport). So the test cannot depend on the fixture: the fixture builds
# after the test, as part of ALL. A second test gets its own copy of the fixture.
function(hot_test_fixture name source host)
    add_library(${name} SHARED ${source})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/engine)
    set_target_properties(${name} PROPERTIES PREFIX "")
    # The host provides the hot:: symbols at run time. The Mach-O linker needs a flag for this.
    hot_allow_undefined_symbols(${name})
    if(WIN32)
        target_compile_definitions(${name} PRIVATE HOT_MODULE_BUILD=1)
        target_link_libraries(${name} PRIVATE ${host})
    endif()
endfunction()

foreach(g gen1 gen2 gen4_base)
    hot_test_fixture(${g} handwritten_player_${g}.cpp integration_reload_tests)
endforeach()
if(WIN32)
    hot_test_fixture(gen4_base_threading handwritten_player_gen4_base.cpp integration_threading_tests)
endif()
# stress_generation: the class Pulse with the generation number of the load (reload stress test).
hot_test_fixture(stress_generation stress_generation.cpp integration_reload_tests)
