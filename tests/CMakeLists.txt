cmake_minimum_required(VERSION 3.1.3)

# The EditableMesh regression cases use the task driver instead of GTest,
# so they are always built. The driver is C, but links as C++ here.
configure_file(../task_support/driver.c ${CMAKE_CURRENT_BINARY_DIR}/task_driver.cpp COPYONLY)

add_executable(FastBVH_EditableMeshCases
  ${CMAKE_CURRENT_BINARY_DIR}/task_driver.cpp
  editable_mesh_cases.cpp
  ../src/EditableMesh.cpp)

target_link_libraries(FastBVH_EditableMeshCases PRIVATE FastBVH)

target_include_directories(FastBVH_EditableMeshCases PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../task_support)

target_compile_options(FastBVH_EditableMeshCases PRIVATE ${cxxflags})

set_target_properties(FastBVH_EditableMeshCases PROPERTIES
  OUTPUT_NAME editable_mesh_cases
  RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

add_test(NAME FastBVH_EditableMeshCases COMMAND $<TARGET_FILE:FastBVH_EditableMeshCases>)

find_package(GTest)

if(NOT GTest_FOUND)
  return()
endif(NOT GTest_FOUND)

if(NOT GTest_FOUND)
  return()
endif(NOT GTest_FOUND)

add_executable(FastBVH_UnitTests
  BuildStrategy.cxx)

target_link_libraries(FastBVH_UnitTests PRIVATE FastBVH GTest::GTest GTest::Main)

target_compile_options(FastBVH_UnitTests PRIVATE ${cxxflags})

set_target_properties(FastBVH_UnitTests PROPERTIES
  OUTPUT_NAME run_tests
  RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

add_test(NAME FastBVH_UnitTests COMMAND $<TARGET_FILE:FastBVH_UnitTests>)

enable_testing()
