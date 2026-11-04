# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The test of the tree-sitter grammar. It generates the parser, parses the example scripts and
# compiles the highlight query. It needs node and the tree-sitter tool. It skips (the code 77)
# when one is missing. The test never uses the network.
if(HOT_BUILD_TESTS)
    find_program(HOT_NODE node)
    find_program(HOT_TREE_SITTER tree-sitter)
    if(HOT_NODE AND HOT_TREE_SITTER)
        add_test(NAME TreeSitterGrammar
                 COMMAND ${HOT_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test/check.js
                         ${CMAKE_CURRENT_BINARY_DIR}/work ${HOT_TREE_SITTER})
        # The script exits with 77 when the tool does not run. CTest then shows "Skipped".
        set_tests_properties(TreeSitterGrammar PROPERTIES SKIP_RETURN_CODE 77)
    else()
        message(STATUS "node or tree-sitter not found: the test of the tree-sitter grammar is off")
    endif()
endif()
