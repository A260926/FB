set (BEA_SOURCES
  BeaEngine.c
  vsib/vsib_decode.c
  vsib/vsib_footprint.c
)

if (optBUILD_DLL)
  add_library (${BEA_TARGET} SHARED ${BEA_SOURCES})
  set_target_properties (${BEA_TARGET} PROPERTIES COMPILE_FLAGS "-DBUILD_BEA_ENGINE_DLL")
else ()
  add_library (${BEA_TARGET} STATIC ${BEA_SOURCES})
  set_target_properties (${BEA_TARGET} PROPERTIES COMPILE_FLAGS "-DBEA_ENGINE_STATIC")
endif ()
