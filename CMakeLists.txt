# Minimum version and project name
cmake_minimum_required(VERSION 3.10)
project(ray_tracer)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(include)

# Define source files
file(GLOB SOURCES "src/*.cpp")

# Add the executable
add_executable(ray_tracer ${SOURCES})

# Build-type-aware compile options
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(ray_tracer PRIVATE -O2 -march=native)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(ray_tracer PRIVATE -O2 -g -march=native)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(ray_tracer PRIVATE -O0 -g)
else()
    target_compile_options(ray_tracer PRIVATE -O2)
endif()

# Optional sanitizer flags (ASAN + UBSAN combined)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TESTING "Enable CTest" OFF)

if(ENABLE_ASAN)
    target_compile_options(ray_tracer PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(ray_tracer PRIVATE -fsanitize=address)
endif()

if(ENABLE_UBSAN)
    target_compile_options(ray_tracer PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
    target_link_options(ray_tracer PRIVATE -fsanitize=undefined)
endif()

if(ENABLE_TESTING)
    enable_testing()

    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    file(GLOB TEST_SOURCES "tests/*.cpp")
    add_executable(ray_tracer_tests ${TEST_SOURCES})
    target_link_libraries(ray_tracer_tests PRIVATE Catch2::Catch2WithMain)

    if(ENABLE_ASAN)
        target_compile_options(ray_tracer_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(ray_tracer_tests PRIVATE -fsanitize=address)
    endif()

    if(ENABLE_UBSAN)
        target_compile_options(ray_tracer_tests PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
        target_link_options(ray_tracer_tests PRIVATE -fsanitize=undefined)
    endif()

    include(CTest)
    include(Catch)
    catch_discover_tests(ray_tracer_tests PROPERTIES TIMEOUT 120)
endif()