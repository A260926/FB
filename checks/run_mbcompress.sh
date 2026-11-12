#!/usr/bin/env bash
#
# Build and verification for sp_mbcompress.
#
# 1. Runs the existing baseline checks.
# 2. Generates soundpipe.h and a static library through the normal
#    Makefile, once per precision (float and USE_DOUBLE).
# 3. Builds and runs checks/mbcompress.c against each library.
# 4. Requires six named cases to pass in both precisions; any missing
#    or failing case makes the script exit non-zero.
set -euo pipefail

cd "$(dirname "$0")/.."
root=$(pwd)

echo "== baseline =="
bash repro/baseline.sh

work=$(mktemp -d "${TMPDIR:-/tmp}/mbcompress.XXXXXX")
trap 'rm -rf "$work"' EXIT

cat > "$work/checks.mk" <<'EOF'
MODULES = base butlp buthp mbcompress
CFLAGS += -DNO_LIBSNDFILE -Ilib/faust
EOF

cases="unity_allpass crossover_points band_selectivity stereo_link dry_wet timing_params_reset"
precisions=0

for prec in float double; do
    prefix="$work/$prec"
    echo "== precision: $prec =="
    if [ "$prec" = double ]; then
        # soundpipe.h defaults SPFLOAT to float and itself defines
        # USE_DOUBLE; the consumer still has to set SPFLOAT=double.
        make USE_DOUBLE=1 CONFIG="$work/checks.mk" \
            INTERMEDIATES_PREFIX="$prefix" \
            "$prefix/libsoundpipe.a" -j4 >/dev/null
        cc -std=c99 -Wall -Wextra -UNDEBUG -DSPFLOAT=double \
            -I"$prefix/h" "$root/checks/mbcompress.c" \
            "$prefix/libsoundpipe.a" -lm -o "$prefix/mbcompress_check"
    else
        make CONFIG="$work/checks.mk" \
            INTERMEDIATES_PREFIX="$prefix" \
            "$prefix/libsoundpipe.a" -j4 >/dev/null
        cc -std=c99 -Wall -Wextra -UNDEBUG -DSPFLOAT=float \
            -I"$prefix/h" "$root/checks/mbcompress.c" \
            "$prefix/libsoundpipe.a" -lm -o "$prefix/mbcompress_check"
    fi

    log="$prefix/run.log"
    "$prefix/mbcompress_check" | tee "$log"

    for c in $cases; do
        if ! grep -q "case $c" "$log"; then
            echo "missing named case: $c ($prec)" >&2
            exit 1
        fi
    done
    if ! grep -q "MB_COMPRESS_CASES_DONE count=6" "$log"; then
        echo "case run did not complete ($prec)" >&2
        exit 1
    fi
    precisions=$((precisions + 1))
done

if [ "$precisions" -ne 2 ]; then
    echo "expected 2 precisions, got $precisions" >&2
    exit 1
fi

echo "MB_COMPRESS_OK precisions=2 cases=6"
