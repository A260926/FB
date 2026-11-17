cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
project(MemPDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT only applies to the MSVC ABI toolchain (cl / clang-cl).
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Whole-program / link-time optimization (cross-module inlining) for Release.
include(CheckIPOSupported)
check_ipo_supported(RESULT MEMPDB_IPO_OK OUTPUT MEMPDB_IPO_MSG)

find_package(Threads REQUIRED)
if(NOT WIN32)
    find_package(CURL REQUIRED)
endif()

# MSVC-style frontends: cl.exe, clang-cl.exe
# GNU-style frontends: g++ / clang++ (Windows MinGW, Linux, ...)
set(MEMPDB_MSVC_FRONTEND FALSE)
if(MSVC OR (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    set(MEMPDB_MSVC_FRONTEND TRUE)
endif()

function(mempdb_apply_warnings target)
    cmake_parse_arguments(MEMPDB_W "WX" "" "" ${ARGN})
    if(MEMPDB_MSVC_FRONTEND)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(MEMPDB_W_WX)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(MEMPDB_W_WX)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(mempdb_apply_release_opt target)
    if(MEMPDB_MSVC_FRONTEND)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:/O2>)
    else()
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:-O2>)
    endif()
endfunction()

add_library(MemPDB STATIC
    src/MSFReader.cpp
    src/Downloader.cpp
    src/PDBParser.cpp
    src/SymbolServer.cpp
    src/PDB.cpp
)

target_include_directories(MemPDB
    PUBLIC  include
    PRIVATE src
)

target_link_libraries(MemPDB PRIVATE Threads::Threads)
if(WIN32)
    # Works for MSVC (winhttp.lib) and MinGW/Clang (-lwinhttp).
    target_link_libraries(MemPDB PRIVATE winhttp)
else()
    target_link_libraries(MemPDB PRIVATE CURL::libcurl)
endif()

mempdb_apply_warnings(MemPDB WX)
mempdb_apply_release_opt(MemPDB)
if(MEMPDB_IPO_OK)
    set_property(TARGET MemPDB PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

add_executable(example example/main.cpp)
target_link_libraries(example PRIVATE MemPDB)
mempdb_apply_warnings(example WX)
mempdb_apply_release_opt(example)
if(MEMPDB_IPO_OK)
    set_property(TARGET example PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# --- pdb2json: PDB -> JSON exporter ----------------------------------------
add_executable(pdb2json tools/pdb2json/pdb2json.cpp)
target_link_libraries(pdb2json PRIVATE MemPDB)
mempdb_apply_warnings(pdb2json)
mempdb_apply_release_opt(pdb2json)
if(MEMPDB_IPO_OK)
    set_property(TARGET pdb2json PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# --- Test & benchmark suite -------------------------------------------------

# Fixture shared library built with full debug info. On MSVC this also emits a
# private PDB used for size/argument ground-truth tests. On Linux/GCC/Clang the
# .so still builds; those PDB-backed checks are skipped unless a fixture.pdb is
# supplied on the command line.
add_library(mempdb_fixture SHARED tests/fixture.cpp)
if(MEMPDB_MSVC_FRONTEND)
    target_compile_options(mempdb_fixture PRIVATE /Zi /Od)
    target_link_options(mempdb_fixture PRIVATE /DEBUG /OPT:NOREF /OPT:NOICF)
else()
    target_compile_options(mempdb_fixture PRIVATE -O0 -g)
    if(NOT WIN32)
        set_target_properties(mempdb_fixture PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
    endif()
endif()
set_property(TARGET mempdb_fixture PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)

add_executable(mempdb_tests tests/test_main.cpp)
target_include_directories(mempdb_tests PRIVATE tests)
target_link_libraries(mempdb_tests PRIVATE MemPDB)
if(WIN32)
    target_link_libraries(mempdb_tests PRIVATE psapi)
endif()
add_dependencies(mempdb_tests mempdb_fixture)
mempdb_apply_warnings(mempdb_tests)
mempdb_apply_release_opt(mempdb_tests)
if(MEMPDB_IPO_OK)
    set_property(TARGET mempdb_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
