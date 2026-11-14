cmake_minimum_required(VERSION 2.8.12...3.27)

################
## Variables
#########################
IF(LEXBOR_BUILD_SHARED)
    set(EXAMPLES_DEPS_LIB_NAMES ${LEXBOR_LIB_NAME})
ELSEIF(LEXBOR_BUILD_STATIC)
    set(EXAMPLES_DEPS_LIB_NAMES ${LEXBOR_LIB_NAME_STATIC})
ELSE()
    message(FATAL_ERROR "Requires static or dynamic Lexbor library")
ENDIF()

################
## Subs
#########################
FIND_AND_APPEND_SUB_DIRS("lexbor" OFF)

################
## Top-level examples
#########################
IF(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/form_replay.c")
    add_executable(examples_form_replay form_replay.c)
    set_target_properties(examples_form_replay PROPERTIES
                          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
                          OUTPUT_NAME form_replay)
    add_dependencies(examples_form_replay ${EXAMPLES_DEPS_LIB_NAMES})
    target_link_libraries(examples_form_replay ${EXAMPLES_DEPS_LIB_NAMES})

    IF(LEXBOR_BUILD_TESTS)
        add_test(NAME examples_form_replay
                 COMMAND "${CMAKE_BINARY_DIR}/examples/form_replay"
                         "${CMAKE_SOURCE_DIR}/test/files/lexbor/html/form_replay/form_a.html"
                         "${CMAKE_SOURCE_DIR}/test/files/lexbor/html/form_replay/form_b.html"
                         "${CMAKE_SOURCE_DIR}/test/files/lexbor/html/form_replay/actions.txt")
    ENDIF()
ENDIF()
