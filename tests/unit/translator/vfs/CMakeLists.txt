# The tests of hot::FileSystem and of the imports from a file system of the host.
add_executable(unit_translator_vfs_tests
    vfs_tests.cpp)
target_link_libraries(unit_translator_vfs_tests PRIVATE hotc_lib GTest::gtest_main)
target_include_directories(unit_translator_vfs_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/translator ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(unit_translator_vfs_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
hot_gtest(unit_translator_vfs_tests)
