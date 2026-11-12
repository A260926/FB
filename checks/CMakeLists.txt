# Supplied runners only; no target codec implementation.
add_executable(check-baseline "${CMAKE_CURRENT_LIST_DIR}/start_test.cpp")
target_link_libraries(check-baseline PRIVATE charls GTest::gtest_main)
if(EXISTS "${PROJECT_SOURCE_DIR}/test/stream_decoder_contract_test.cpp")
  add_executable(check-stream-contract
    "${CMAKE_CURRENT_LIST_DIR}/contract_main.cpp"
    "${PROJECT_SOURCE_DIR}/test/stream_decoder_contract_test.cpp"
    "${PROJECT_SOURCE_DIR}/test/support.cpp")
  target_include_directories(check-stream-contract PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/test")
  target_link_libraries(check-stream-contract PRIVATE charls GTest::gtest)
endif()
