# Compares two generated files. The "#line" lines hold the path of the file, so the script replaces
# the directory of each file with the word DIR first. Arguments: -DA=<file> -DB=<file>.
foreach(v A B)
    get_filename_component(dir_${v} "${${v}}" DIRECTORY)
    file(READ "${${v}}" text_${v})
    string(REPLACE "${dir_${v}}" "DIR" text_${v} "${text_${v}}")
endforeach()
if(NOT text_A STREQUAL text_B)
    message(FATAL_ERROR "The generated files differ: ${A} ${B}")
endif()
