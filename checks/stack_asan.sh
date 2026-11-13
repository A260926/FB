#!/usr/bin/env bash
# Copyright (C) 2015-2025 Jonathan Müller and foonathan/memory contributors
# SPDX-License-Identifier: Zlib
#
# Builds and runs the FOONATHAN_MEMORY_ASAN checks for memory_stack:
#  - with the option enabled, in Release and Debug, the ASan-aware behavior and the
#    out-of-bounds child processes,
#  - with the option disabled, the original behavior (and no ASan symbols),
#  - a configure check that an enabled build without AddressSanitizer fails loudly.
set -eu

cd "$(dirname "$0")/.."
ROOT=$(pwd)
BUILD_ROOT="$ROOT/build-checks-asan"
CXX=${CXX:-c++}

rm -rf "$BUILD_ROOT"
mkdir -p "$BUILD_ROOT"

CMAKE_COMMON=(
    -DFOONATHAN_MEMORY_BUILD_TESTS=OFF
    -DFOONATHAN_MEMORY_BUILD_EXAMPLES=OFF
    -DFOONATHAN_MEMORY_BUILD_TOOLS=OFF
    -DFOONATHAN_MEMORY_EXTERN_TEMPLATE=ON
)

# ---------------------------------------------------------------------------
# header only helper: compile checks/stack_asan.cpp against a build directory
# ---------------------------------------------------------------------------
build_and_run_check()
{
    local build_dir=$1
    local output=$2
    local extra_cxx=${3:-}
    local lib
    lib=$(find "$build_dir/src" -maxdepth 1 -name 'libfoonathan_memory-*.a' | head -n 1)
    if [ -z "$lib" ]; then
        echo "error: no library found in $build_dir/src" >&2
        exit 1
    fi
    # shellcheck disable=SC2086
    "$CXX" -std=c++11 -g -fsanitize=address -fno-omit-frame-pointer \
        -Iinclude -I"$build_dir/src" $extra_cxx \
        checks/stack_asan.cpp "$lib" -o "$output"
    "$output"
}

# ---------------------------------------------------------------------------
# enabled with AddressSanitizer (Release and Debug)
# ---------------------------------------------------------------------------
for config in Release Debug; do
    build_dir="$BUILD_ROOT/asan-$config"
    cmake -S . -B "$build_dir" \
        -DCMAKE_BUILD_TYPE="$config" \
        -DFOONATHAN_MEMORY_ASAN=ON \
        "${CMAKE_COMMON[@]}" \
        -DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer' \
        > "$BUILD_ROOT/configure-$config.log" 2>&1
    cmake --build "$build_dir" --target foonathan_memory -j2 \
        > "$BUILD_ROOT/build-$config.log" 2>&1
    echo "== ASan enabled ($config build) =="
    build_and_run_check "$build_dir" "$BUILD_ROOT/stack_asan-$config"
done

# The option must not require a separate sanitizer flag for direct compiles when
# the consumer links through CMake: verify that a plain compile of a user of the
# public headers fails loudly instead of silently building a broken binary.
echo "== ASan enabled, user compiled without AddressSanitizer must fail =="
if "$CXX" -std=c++11 -Iinclude -I"$BUILD_ROOT/asan-Release/src" \
    -c checks/stack_asan.cpp -o "$BUILD_ROOT/without-asan.o" \
    > "$BUILD_ROOT/without-asan.log" 2>&1; then
    echo "error: compiling a user without AddressSanitizer unexpectedly succeeded" >&2
    exit 1
fi
if ! grep -q "not compiled with AddressSanitizer" "$BUILD_ROOT/without-asan.log"; then
    echo "error: the missing AddressSanitizer was not reported clearly" >&2
    cat "$BUILD_ROOT/without-asan.log" >&2
    exit 1
fi
echo "ASAN_STACK misconfiguration mode=enabled-without-asan reported=1"

# The library itself also has to reject the configuration when the compiler has no
# sanitizer runtime. A compiler wrapper that rejects AddressSanitizer simulates such
# a toolchain without depending on the host's sanitizer support.
echo "== ASan enabled, configure-time capability probe =="
probe_dir="$BUILD_ROOT/probe"
mkdir -p "$probe_dir"
cat > "$probe_dir/no-asan-cxx" <<EOF
#!/usr/bin/env bash
for arg in "\$@"; do
    case "\$arg" in
        -fsanitize=address*) exit 1 ;;
    esac
done
exec "$CXX" "\$@"
EOF
chmod +x "$probe_dir/no-asan-cxx"
if cmake -S . -B "$probe_dir/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFOONATHAN_MEMORY_ASAN=ON \
    -DCMAKE_CXX_COMPILER="$probe_dir/no-asan-cxx" \
    "${CMAKE_COMMON[@]}" \
    > "$probe_dir/configure.log" 2>&1; then
    echo "error: CMake accepted an enabled ASan build without sanitizer support" >&2
    exit 1
fi
if ! grep -q "FOONATHAN_MEMORY_ASAN is enabled" "$probe_dir/configure.log"; then
    echo "error: the missing AddressSanitizer support was not reported clearly" >&2
    cat "$probe_dir/configure.log" >&2
    exit 1
fi
echo "ASAN_STACK configure-probe mode=enabled missing-support=reported"
echo "ASAN_STACK configure-probe mode=enabled capability=ok"

# ---------------------------------------------------------------------------
# disabled: original behavior holds and no AddressSanitizer symbols are used
# The disabled library and user are built *without* AddressSanitizer, so a
# successful build and link proves the feature introduces no sanitizer dependency.
# ---------------------------------------------------------------------------
echo "== ASan option disabled (original behavior) =="
build_dir="$BUILD_ROOT/disabled-Release"
cmake -S . -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFOONATHAN_MEMORY_ASAN=OFF \
    "${CMAKE_COMMON[@]}" \
    > "$BUILD_ROOT/configure-disabled.log" 2>&1
cmake --build "$build_dir" --target foonathan_memory -j2 \
    > "$BUILD_ROOT/build-disabled.log" 2>&1

lib=$(find "$build_dir/src" -maxdepth 1 -name 'libfoonathan_memory-*.a' | head -n 1)
"$CXX" -std=c++11 -g -Iinclude -I"$build_dir/src" \
    checks/stack_asan.cpp "$lib" -o "$BUILD_ROOT/stack_asan-disabled"
"$BUILD_ROOT/stack_asan-disabled"

if nm -u "$lib" 2>/dev/null | grep -q "__asan_"; then
    echo "error: the disabled build has undefined AddressSanitizer symbols" >&2
    nm -u "$lib" | grep "__asan_" >&2
    exit 1
fi
if grep -q "#define FOONATHAN_MEMORY_ASAN 1" "$build_dir/src/config_impl.hpp"; then
    echo "error: the disabled build enables FOONATHAN_MEMORY_ASAN" >&2
    exit 1
fi
# The disabled check itself verifies the original behavior (same-address reuse,
# accessible unwound memory); it is built and linked without any sanitizer flags.
echo "ASAN_STACK disabled mode=off asan-symbols=0 baseline=ok"

echo "== ASan checks passed =="
