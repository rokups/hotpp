# Golden-output tests. The expected files are in the folder expected/.
add_executable(golden_translation_tests golden_translation_tests.cpp)
target_link_libraries(golden_translation_tests PRIVATE hotc_lib GTest::gtest_main)
target_include_directories(golden_translation_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/translator ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(golden_translation_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
hot_gtest(golden_translation_tests)
