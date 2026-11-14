#!/bin/sh
#
# check-predication.sh
#
# End-to-end, CPU-only checks for glsl120 predicated SM3 translation.
# No GPU, Python, or extra libraries: everything is compiled with the system
# C99 compiler and driven through MOJOSHADER_parse().
#
#  1. Builds the existing baseline and the new predication test binary.
#  2. Confirms the non-predicated output is byte-identical to the baseline.
#  3. Confirms the previously-failing predicated fixture now translates.
#  4. Runs the C99 register/texture model suite (it prints its own case and
#     assertion counts, including rejection of unsupported opcodes and illegal
#     predicates).
#  5. Re-runs the original baseline and the predicated sample at the end.

set -eu

cd "$(dirname "$0")/.."
mkdir -p build

printf '%s\n' '#define MOJOSHADER_VERSION -1' \
              '#define MOJOSHADER_CHANGESET "fixed-source"' \
              > build/mojoshader_version.h

SOURCES="mojoshader.c mojoshader_common.c \
 profiles/mojoshader_profile_common.c profiles/mojoshader_profile_glsl.c \
 profiles/mojoshader_profile_d3d.c profiles/mojoshader_profile_bytecode.c \
 profiles/mojoshader_profile_arb1.c profiles/mojoshader_profile_metal.c \
 profiles/mojoshader_profile_spirv.c profiles/mojoshader_profile_hlsl.c"

CFLAGS="-std=c99 -O0 -Wall -Wextra -I. -Ibuild"

echo "== building baseline =="
cc $CFLAGS checks/baseline.c $SOURCES -lm -o build/baseline

echo "== building predication suite =="
cc $CFLAGS checks/check_predication.c $SOURCES -lm -o build/check_predication

echo "== non-predicated output must match the existing baseline =="
# The exact GLSL the plain (non-predicated) fixture has always produced.
cat > build/baseline_expected.txt <<'EXPECTED'
#version 120
attribute vec4 vs_v0;
#define vs_o0 gl_Position

void main()
{
	vs_o0 = vs_v0;
}
EXPECTED

./build/baseline > build/baseline_plain.txt
# Keep only the GLSL body: from the "#version" line through main()'s closing
#  brace, dropping the "PLAIN errors=0" header, trailing blanks and the OK line.
awk '/^#version 120/ { body = 1 }
     body { print }
     body && /^}/ { exit }' build/baseline_plain.txt > build/baseline_got.txt
if diff -u build/baseline_expected.txt build/baseline_got.txt; then
    echo "baseline GLSL unchanged"
else
    echo "ERROR: non-predicated output drifted from the baseline" >&2
    exit 1
fi

echo "== predicated fixture must now translate (it used to fail) =="
./build/baseline --predicated > build/baseline_predicated.txt
if grep -q 'errors=0' build/baseline_predicated.txt \
   && grep -q 'if (vs_p0' build/baseline_predicated.txt; then
    echo "predicated fixture translated with per-channel guards"
else
    echo "ERROR: predicated fixture did not translate as expected" >&2
    cat build/baseline_predicated.txt >&2
    exit 1
fi

echo "== register/texture execution model + rejection cases =="
./build/check_predication

echo "== re-running original baseline and predicated sample =="
./build/baseline
./build/baseline --predicated >/dev/null
echo "predicated sample exit status: $?"

echo "all predication checks passed"
