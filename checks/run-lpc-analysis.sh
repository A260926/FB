#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
if [ ! -s tests/lpc_analysis.cpp ]; then
    echo 'LPC_TARGET_MISSING tests/lpc_analysis.cpp' >&2
    exit 2
fi
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
# Own the main and build command here; do not delegate execution to a new script.
${CXX:-c++} -std=c++11 -O2 -UNDEBUG -Icodec -Ichecks -x c++ \
    checks/lpc_test_main.cpp tests/lpc_analysis.cpp \
    codec/ALACEncoder.cpp codec/ALACDecoder.cpp codec/EndianPortable.c \
    codec/ALACBitUtilities.c codec/ag_enc.c codec/ag_dec.c codec/dp_enc.c \
    codec/dp_dec.c codec/matrix_enc.c codec/matrix_dec.c -o "$build/lpc-analysis"
if "$build/lpc-analysis" >"$build/report" 2>&1; then
    cat "$build/report"
else
    status=$?
    cat "$build/report"
    exit "$status"
fi
# A premature successful exit before the fixed main's summary is not success.
LC_ALL=C grep -Fx 'ALAC_LPC_ANALYSIS_OK cases=12 passed=12 skipped=0 failed=0' "$build/report" >/dev/null
