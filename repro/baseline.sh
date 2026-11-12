#!/usr/bin/env bash
#
# repro/baseline.sh
#
# Baseline gate: the unmodified library build machinery must produce the
# static library in both precisions before the mbcompress checks run.
# NO_LIBSNDFILE keeps the baseline independent of any audio device or
# third-party library availability.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

base="$root/repro/build-baseline"

# Some bundled library Makefiles (openlpc, dr_wav) assume the default
# build prefix and lack rules to create or stage files under a custom
# $(LIBDIR); prepare those directories and sources up front.
stage_openlpc() {
    mkdir -p "$1"/lib/{openlpc,dr_wav,spa,fft,kissfft,inih}
    cp "$root/lib/openlpc/openlpc.c" \
       "$root/lib/openlpc/openlpc.h" \
       "$root/lib/openlpc/ftol.h" "$1/lib/openlpc/"
}

echo "[baseline] float build"
make -s clean INTERMEDIATES_PREFIX="$base/float" >/dev/null 2>&1 || true
stage_openlpc "$base/float"
cp "$root/config.def.mk" "$base/float/config.mk"
# generate the amalgamated header first to avoid a parallel
# build race on the generated soundpipe.h
make -s INTERMEDIATES_PREFIX="$base/float" NO_LIBSNDFILE=1 \
     "$base/float/h/soundpipe.h"
make -s -j4 INTERMEDIATES_PREFIX="$base/float" NO_LIBSNDFILE=1
test -f "$base/float/libsoundpipe.a"
test -f "$base/float/h/soundpipe.h"

echo "[baseline] USE_DOUBLE build"
make -s clean INTERMEDIATES_PREFIX="$base/double" >/dev/null 2>&1 || true
stage_openlpc "$base/double"
cp "$root/config.def.mk" "$base/double/config.mk"
make -s INTERMEDIATES_PREFIX="$base/double" NO_LIBSNDFILE=1 USE_DOUBLE=1 \
     "$base/double/h/soundpipe.h"
make -s -j4 INTERMEDIATES_PREFIX="$base/double" NO_LIBSNDFILE=1 USE_DOUBLE=1
test -f "$base/double/libsoundpipe.a"
test -f "$base/double/h/soundpipe.h"

echo "BASELINE_OK"
