#!/usr/bin/env bash
#
# checks/run_mbcompress.sh
#
# Builds sp_mbcompress through the normal soundpipe build (generated
# soundpipe.h + static library) in both float and USE_DOUBLE precision,
# then runs the independent double-precision reference checks in
# checks/mbcompress.c. No Python is involved.
#
# Success prints:  MB_COMPRESS_OK precisions=2 cases=6
# Any missing or failing named case, in either precision, exits nonzero.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
work="${MBC_BUILD_DIR:-$here/build}"
expected_cases=6

cd "$root"

# 1. baseline gate first
bash "$root/repro/baseline.sh"

# 2. compute() must not allocate or free memory
if sed -n '/^int sp_mbcompress_compute/,/^}/p' modules/mbcompress.c \
        | grep -Eq 'malloc|calloc|realloc|[^a-z]free\('; then
    echo "FAIL: sp_mbcompress_compute allocates or frees memory" >&2
    exit 1
fi

mkdir -p "$work"
ran_precisions=0

for prec in float double; do
    bdir="$work/$prec"
    extra=""
    precflags=""
    if [ "$prec" = double ]; then
        extra="USE_DOUBLE=1"
        # the generated header defines USE_DOUBLE itself; SPFLOAT only
        # changes through this compiler flag, matching the Makefile
        precflags="-DSPFLOAT=double"
    fi

    echo "[mbcompress] $prec precision build"
    rm -rf "$bdir"
    # some bundled lib Makefiles lack mkdir/staging rules for custom
    # build prefixes; prepare those directories and sources up front
    mkdir -p "$bdir"/lib/{openlpc,dr_wav,spa,fft,kissfft,inih}
    cp "$root/lib/openlpc/openlpc.c" \
       "$root/lib/openlpc/openlpc.h" \
       "$root/lib/openlpc/ftol.h" "$bdir/lib/openlpc/"
    cp "$root/config.def.mk" "$bdir/config.mk"
    # generate the amalgamated header first to avoid a parallel
    # build race on the generated soundpipe.h
    # shellcheck disable=SC2086
    make -s INTERMEDIATES_PREFIX="$bdir" NO_LIBSNDFILE=1 $extra \
         "$bdir/h/soundpipe.h"
    # shellcheck disable=SC2086
    make -s -j4 INTERMEDIATES_PREFIX="$bdir" NO_LIBSNDFILE=1 $extra

    # the module must be reachable through the normally generated header
    if ! grep -q sp_mbcompress_init "$bdir/h/soundpipe.h"; then
        echo "FAIL: sp_mbcompress missing from generated soundpipe.h" >&2
        exit 1
    fi

    # shellcheck disable=SC2086
    cc -std=c99 -Wall -Wextra -Werror $precflags -I "$bdir/h" \
        "$here/mbcompress.c" "$bdir/libsoundpipe.a" -lm \
        -o "$bdir/mbcompress_check"

    # the native C example builds and runs against the same artifacts
    # shellcheck disable=SC2086
    cc -std=c99 -Wall -Wextra -Werror $precflags -I "$bdir/h" \
        "$root/examples/ex_mbcompress.c" "$bdir/libsoundpipe.a" -lm \
        -o "$bdir/ex_mbcompress"
    "$bdir/ex_mbcompress" > "$bdir/example.out"

    log="$bdir/check.log"
    "$bdir/mbcompress_check" | tee "$log"

    ncases=$(grep -c '^CASE ' "$log" || true)
    nfails=$(grep -c '^FAIL:' "$log" || true)
    if [ "$ncases" -ne "$expected_cases" ] || [ "$nfails" -ne 0 ]; then
        echo "FAIL: $prec ran $ncases cases (want $expected_cases), " \
             "$nfails failure(s)" >&2
        exit 1
    fi
    ran_precisions=$((ran_precisions + 1))
done

if [ "$ran_precisions" -ne 2 ]; then
    echo "FAIL: expected 2 precision builds, ran $ran_precisions" >&2
    exit 1
fi

echo "MB_COMPRESS_OK precisions=2 cases=6"
