add_library(json
  dist/jsoncpp.cpp
  dist/json/json.h
  dist/json/json-forwards.h)

set_target_properties(json
  PROPERTIES FOLDER "extern")

target_compile_definitions(json
  PUBLIC $<$<BOOL:${BUILD_SHARED_LIBS}>:JSON_DLL>
  PRIVATE $<$<BOOL:${BUILD_SHARED_LIBS}>:JSON_DLL_BUILD>)

target_include_directories(json SYSTEM PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/extern/jsoncpp/dist>)
