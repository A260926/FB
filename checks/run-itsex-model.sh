#!/bin/sh
# Fixed execution scaffold. Tests and generator are deliberately not supplied.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
for file in test-dev/test_itsex_model.c test-dev/itsex_model_generator.c; do
    test -s "$file" || { echo "MISSING_TARGET $file" >&2; exit 2; }
done
build=$(mktemp -d "${TMPDIR:-/tmp}/itsex-model.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
# Compile test translation units first; comment-only files still fail at linkage.
${CC:-cc} -std=c99 -O2 -UNDEBUG -I. -Iinclude -Isrc -c test-dev/test_itsex_model.c -o "$build/model.o"
${CC:-cc} -std=c99 -O2 -UNDEBUG -I. -Iinclude -Isrc -c test-dev/itsex_model_generator.c -o "$build/generator.o"
sh checks/baseline.sh
cmake -S . -B "$build/lib" -DBUILD_SHARED=OFF -DBUILD_STATIC=ON \
    -DLIBXMP_DISABLE_DEPACKERS=ON -DLIBXMP_DISABLE_PROWIZARD=ON \
    -DWITH_UNIT_TESTS=OFF -DLIBXMP_DOCS=OFF -DCMAKE_C_COMPILER="${CC:-cc}"
cmake --build "$build/lib" --target xmp_static -j 2
${CC:-cc} -std=c99 -O2 -UNDEBUG -I. -Iinclude -Isrc checks/itsex_model_driver.c \
    "$build/model.o" "$build/generator.o" "$build/lib/libxmp.a" -lm -o "$build/model"
if "$build/model" > "$build/normal.log"; then
    cat "$build/normal.log"
else
    status=$?; cat "$build/normal.log"; exit "$status"
fi
grep -Fx 'ITSEX_MODEL_CASES_OK cases=16' "$build/normal.log" >/dev/null
for variant in reset-d2 drop-cache-low; do
    mkdir -p "$build/$variant"
    cp -R src "$build/$variant/src"
    patch -s -d "$build/$variant" -p1 < "checks/mutations/$variant.patch"
    ${CC:-cc} -std=c99 -O2 -UNDEBUG -I. -Iinclude -Isrc \
        -c "$build/$variant/src/loaders/itsex.c" -o "$build/$variant/itsex.o"
    # Explicit object precedes archive, so all decoder symbols come from the changed source.
    ${CC:-cc} -std=c99 -O2 -UNDEBUG -I. -Iinclude -Isrc checks/itsex_model_driver.c \
        "$build/model.o" "$build/generator.o" "$build/$variant/itsex.o" \
        "$build/lib/libxmp.a" -lm -o "$build/$variant/model"
    echo "MUTANT_BUILD_OK $variant"
    case "$variant" in
        reset-d2) group=core-8-215-tail ;;
        drop-cache-low) group=core-8-214-short ;;
    esac
    if "$build/$variant/model" --expect-pcm-mismatch "$group" > "$build/$variant/result.log"; then
        cat "$build/$variant/result.log"
    else
        status=$?; cat "$build/$variant/result.log"; exit "$status"
    fi
    grep -Fx "ITSEX_MODEL_DIFFERENCE_OK case=$group" "$build/$variant/result.log" >/dev/null
    echo "MUTANT_PCM_DETECTED $variant"
done
printf '%s\n' 'ITSEX_MODEL_CONTRACT_OK cases=16 mutants=2'
