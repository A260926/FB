add_executable(task_baseline baseline.cpp)
target_include_directories(task_baseline PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}")
target_link_libraries(task_baseline PRIVATE opentimelineio)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/audio_render_test.cpp")
  # These files form part of the production library, not a test-only replacement.
  target_sources(opentimelineio PRIVATE
    "${PROJECT_SOURCE_DIR}/src/opentimelineio/audioRender.cpp"
    "${PROJECT_SOURCE_DIR}/src/opentimelineio/pcmWave.cpp")
  set_source_files_properties(
    "${PROJECT_SOURCE_DIR}/src/opentimelineio/audioRender.cpp"
    "${PROJECT_SOURCE_DIR}/src/opentimelineio/pcmWave.cpp"
    TARGET_DIRECTORY opentimelineio PROPERTIES COMPILE_OPTIONS "-include;${PROJECT_SOURCE_DIR}/src/opentimelineio/audioRender.h;-include;${PROJECT_SOURCE_DIR}/src/opentimelineio/pcmWave.h")
  set_source_files_properties(driver.c PROPERTIES LANGUAGE CXX)
  add_executable(task_target driver.c audio_render_test.cpp)
  target_include_directories(task_target PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}")
  target_link_libraries(task_target PRIVATE opentimelineio)
  set_target_properties(task_target PROPERTIES OUTPUT_NAME target RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_executable(audio_render "${PROJECT_SOURCE_DIR}/examples/audio_render.cpp")
  target_link_libraries(audio_render PRIVATE opentimelineio)
  target_include_directories(audio_render PRIVATE "${PROJECT_SOURCE_DIR}/src")
  set_target_properties(audio_render PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

add_executable(task_fixture io_fixture.cpp)
target_link_libraries(task_fixture PRIVATE opentimelineio)
target_include_directories(task_fixture PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(task_fixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
