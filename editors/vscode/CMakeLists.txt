# The test of the TextMate grammar of the VS Code extension.
# The test needs node only. It skips the tokenizer part when vscode-textmate is not installed
# (run "npm install" in this folder). The build and the test never use the network.
find_program(HOT_NODE node)
if(HOT_NODE)
    add_test(NAME VscodeGrammar
             COMMAND ${HOT_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test/grammar.test.js)
    add_test(NAME VscodeExpand
             COMMAND ${HOT_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test/expand.test.js)
    add_test(NAME VscodeOnlySpp
             COMMAND ${HOT_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test/only_spp.test.js)
else()
    message(STATUS "node not found: the test of the VS Code grammar is off")
endif()
