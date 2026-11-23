# Copyright (C) 2015-2025 Jonathan Müller and foonathan/memory contributors
# SPDX-License-Identifier: Zlib

# AddressSanitizer integration for the stack allocator.
#
# When FOONATHAN_MEMORY_ASAN is ON the library and every consumer are compiled
# and linked with AddressSanitizer, and the generated config_impl.hpp enables
# the poison lifecycle in the public memory_stack templates.  The option is OFF
# by default, in which case no sanitizer flag or symbol is used anywhere.

option(FOONATHAN_MEMORY_ASAN
       "give memory_stack sub-allocations an AddressSanitizer poison lifecycle"
       OFF)

if(FOONATHAN_MEMORY_ASAN)
    if(MSVC)
        set(FOONATHAN_MEMORY_ASAN_COMPILE_FLAG "/fsanitize=address")
        set(FOONATHAN_MEMORY_ASAN_LINK_FLAG "")
    else()
        set(FOONATHAN_MEMORY_ASAN_COMPILE_FLAG "-fsanitize=address")
        set(FOONATHAN_MEMORY_ASAN_LINK_FLAG "-fsanitize=address")
    endif()

    # A single compile-and-link probe: it requires the compiler driver to accept
    # the sanitizer flag, the runtime to be available for linking and the
    # compiler-provided sanitizer/asan_interface.h header to be present.
    include(CheckCXXSourceCompiles)
    set(_cmake_required_flags_backup "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${FOONATHAN_MEMORY_ASAN_COMPILE_FLAG}")
    check_cxx_source_compiles(
        "#include <sanitizer/asan_interface.h>
         int main()
         {
             char buffer[16];
             __asan_poison_memory_region(buffer, sizeof(buffer));
             __asan_unpoison_memory_region(buffer, sizeof(buffer));
             return __asan_address_is_poisoned(buffer) != 0;
         }"
        FOONATHAN_MEMORY_HAS_ADDRESS_SANITIZER)
    set(CMAKE_REQUIRED_FLAGS "${_cmake_required_flags_backup}")

    if(NOT FOONATHAN_MEMORY_HAS_ADDRESS_SANITIZER)
        message(FATAL_ERROR
            "FOONATHAN_MEMORY_ASAN=ON requires a C++ compiler with working "
            "AddressSanitizer support (flag '${FOONATHAN_MEMORY_ASAN_COMPILE_FLAG}', "
            "linkable runtime and sanitizer/asan_interface.h). "
            "Disable FOONATHAN_MEMORY_ASAN or use a compiler with AddressSanitizer.")
    endif()
endif()
