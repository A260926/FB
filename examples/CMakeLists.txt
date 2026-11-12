project(DSPatchExplicitDelayExample)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

add_executable(
    ${PROJECT_NAME}
    explicit_delay.cpp
)

target_link_libraries(
    ${PROJECT_NAME}
    DSPatch
)
