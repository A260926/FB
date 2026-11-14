project(Examples)

set(SOURCES
	ExampleContributions.cpp
	ExampleFunctionSymbols.cpp
	ExampleFunctionVariables.cpp
	ExampleIPI.cpp
	ExampleLines.cpp
	ExampleMain.cpp
	ExampleMemoryMappedFile.cpp
	ExampleMemoryMappedFile.h
	ExamplePDBSize.cpp
	Examples_PCH.cpp
	Examples_PCH.h
	ExampleSymbols.cpp
	ExampleTimedScope.cpp
	ExampleTimedScope.h
	ExampleTypes.cpp
	ExampleTypeTable.cpp
	ExampleTypeTable.h
)

source_group(src FILES
    ${SOURCES}
)

add_executable(Examples
    ${SOURCES}
)

target_link_libraries(Examples
  PUBLIC
    raw_pdb
)

target_precompile_headers(Examples
  PUBLIC
    Examples_PCH.h
)

# Standalone, offline address-provenance example.  It deliberately has its own main() and does
# not load the target image or contact a symbol server; it only reads a PDB file from disk.
add_executable(ExampleAddressProvenance
    ExampleAddressProvenance.cpp
)

target_link_libraries(ExampleAddressProvenance
  PUBLIC
    raw_pdb
)