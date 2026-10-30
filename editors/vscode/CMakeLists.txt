# The VS Code extension. This file has two parts: the package and the install rules, and the
# test of the TextMate grammar.

# The package. The tool vsce makes hotpp-<version>.vsix in the build dir. The cache variable
# HOT_VSCE_COMMAND is the command of vsce as a CMake list. It is empty by default. Then
# the command is "npx --no-install vsce", and the build never uses the network.
set(HOT_VSCE_COMMAND "" CACHE STRING "The command of vsce (a list). Empty: npx --no-install vsce")
set(hot_vsce "")
if(HOT_VSCE_COMMAND)
    set(hot_vsce_candidate ${HOT_VSCE_COMMAND})
else()
    find_program(HOT_NPX npx)
    if(HOT_NPX)
        set(hot_vsce_candidate ${HOT_NPX} --no-install vsce)
    else()
        set(hot_vsce_candidate "")
    endif()
endif()
if(hot_vsce_candidate)
    execute_process(COMMAND ${hot_vsce_candidate} --version
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    RESULT_VARIABLE hot_vsce_result OUTPUT_QUIET ERROR_QUIET)
    if(hot_vsce_result EQUAL 0)
        set(hot_vsce ${hot_vsce_candidate})
    endif()
endif()

if(hot_vsce)
    set(HOT_VSIX_FILE ${CMAKE_BINARY_DIR}/hotpp-${PROJECT_VERSION}.vsix)
    file(GLOB_RECURSE hot_vsix_inputs CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/src/*.js ${CMAKE_CURRENT_SOURCE_DIR}/syntaxes/*.json)
    list(APPEND hot_vsix_inputs ${CMAKE_CURRENT_SOURCE_DIR}/package.json
         ${CMAKE_CURRENT_SOURCE_DIR}/language-configuration.json)
    add_custom_command(
        OUTPUT ${HOT_VSIX_FILE}
        COMMAND ${hot_vsce} package --no-dependencies --skip-license --allow-missing-repository
                --out ${HOT_VSIX_FILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        DEPENDS ${hot_vsix_inputs}
        COMMENT "vsce package: ${HOT_VSIX_FILE}"
        VERBATIM)
    # The extension is part of the SDK, so the SDK build (HOT_INSTALL) always makes it.
    if(HOT_INSTALL)
        add_custom_target(hot_vsix ALL DEPENDS ${HOT_VSIX_FILE})
    else()
        add_custom_target(hot_vsix DEPENDS ${HOT_VSIX_FILE})
    endif()
    if(HOT_INSTALL)
        install(FILES ${HOT_VSIX_FILE} DESTINATION ${CMAKE_INSTALL_DATADIR}/hot/vscode)
    endif()
else()
    message(STATUS "vsce not found: the install copies the extension folder")
    if(HOT_INSTALL)
        install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
                DESTINATION ${CMAKE_INSTALL_DATADIR}/hot/vscode/hotpp
                PATTERN node_modules EXCLUDE
                PATTERN test EXCLUDE
                PATTERN package-lock.json EXCLUDE
                PATTERN CMakeLists.txt EXCLUDE
                PATTERN .gitignore EXCLUDE
                PATTERN .vscodeignore EXCLUDE)
    endif()
endif()

# The test of the TextMate grammar of the VS Code extension.
# The test needs node only. It skips the tokenizer part when vscode-textmate is not installed
# (run "npm install" in this folder). The build and the test never use the network.
if(HOT_BUILD_TESTS)
    find_program(HOT_NODE node)
    if(HOT_NODE)
        add_test(NAME VscodeGrammar
                 COMMAND ${HOT_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test/grammar.test.js)
        add_test(NAME VscodeExpand
                 COMMAND ${HOT_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test/expand.test.js)
        add_test(NAME VscodeOnlyHot
                 COMMAND ${HOT_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test/only_hot.test.js)
    else()
        message(STATUS "node not found: the test of the VS Code grammar is off")
    endif()
endif()
