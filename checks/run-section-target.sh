#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f tests/section_optimality.c ]; then
    echo 'missing tests/section_optimality.c' >&2
    exit 1
fi
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
# All scalar library sources from libfaac/meson.build; no Meson or SIMD SDK.
# gnu11 supplies the existing math constants on platforms which require it.
${CC:-cc} -std=gnu11 -O2 -UNDEBUG -DPACKAGE_VERSION='"2.2.0"' \
    -DMAX_CHANNELS=8 -DFAAC_SBR_DECIMATION=1 \
    -Ichecks -Ilibfaac -Iinclude \
    checks/section_main.c tests/section_optimality.c \
    libfaac/bitstream.c libfaac/blockswitch.c libfaac/channels.c \
    libfaac/cpu_compute.c libfaac/faac.c libfaac/filtbank.c libfaac/fft.c \
    libfaac/frame.c libfaac/huff2.c libfaac/huffdata.c libfaac/quantize.c \
    libfaac/ratecontrol.c libfaac/sbr.c libfaac/sbr_bitstream.c \
    libfaac/sbr_tables.c libfaac/sbr_analysis.c libfaac/resample.c \
    libfaac/stereo.c libfaac/tns.c libfaac/util.c -lm -o "$build/section-target"
"$build/section-target" >"$build/output" 2>&1 || {
    cat "$build/output"
    exit 1
}
cat "$build/output"
# Detect a callback exiting the process early, even with exit(0).
awk '
    /^SECTION_RUN / { runs++ }
    /^SECTION_PASS / { passes++ }
    /^FAAC_SECTION_OPTIMAL_OK cases=12$/ { summaries++; last=NR }
    END { if (runs != 12 || passes != 12 || summaries != 1 || last != NR) exit 1 }
' "$build/output"
# Keep the real pre-existing huffbook/writebooks witness as a separate regression.
# This mode accepts a genuine improvement instead of requiring the old 244 bits.
sh checks/baseline.sh --require-optimal
echo 'FAAC_SECTION_LEGACY_OK'
