#!/bin/sh
# check_dash.sh - build and run the dash contract regression tests.
#
#   sh regress/check_dash.sh              build the library + tests, run them
#   sh regress/check_dash.sh --mutations  prove the tests catch real bugs:
#                                         on a temporary copy of the sources,
#                                         break (1) negative offset
#                                         normalization, (2) the odd-length
#                                         dash period, (3) the MOVE_TO phase
#                                         restart, and require the suite to
#                                         reject each one. The working tree
#                                         is never touched.
set -eu
cd "$(dirname "$0")/.."

CC=${CC:-cc}
CFLAGS="-std=c11 -O1 -DPLUTOVG_BUILD_STATIC -Iinclude -Isource"

if [ "${1:-}" != "--mutations" ]; then
    mkdir -p .build
    # shellcheck disable=SC2086
    $CC $CFLAGS regress/dash_contract.c source/*.c -lm -o .build/dash_contract
    .build/dash_contract
    exit $?
fi

# ---------------------------------------------------------------------------
# Mutation mode: everything happens in a temporary directory.

tmp=$(mktemp -d "${TMPDIR:-/tmp}/dash-mutations.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

mkdir -p "$tmp/base/source" "$tmp/base/include" "$tmp/base/regress"
cp source/*.c source/*.h "$tmp/base/source/"
cp include/*.h "$tmp/base/include/"
cp regress/dash_contract.c "$tmp/base/regress/"

build_and_run() { # $1 = tree root, $2 = output binary
    # shellcheck disable=SC2086
    $CC -std=c11 -O1 -DPLUTOVG_BUILD_STATIC -I"$1/include" -I"$1/source" \
        "$1/regress/dash_contract.c" "$1"/source/*.c -lm -o "$2"
}

# Sanity: the unmutated copy must pass, otherwise mutation results are moot.
build_and_run "$tmp/base" "$tmp/base_dash_contract"
if ! "$tmp/base_dash_contract" > "$tmp/base.log" 2>&1; then
    echo "mutation setup broken: unmutated copy failed:" >&2
    cat "$tmp/base.log" >&2
    exit 2
fi
echo "baseline (unmutated copy): PASS"

require_pattern() { # $1 = pattern, $2 = file, $3 = mutation name
    if ! grep -q "$1" "$2"; then
        echo "mutation '$3' cannot be applied: pattern not found in $2" >&2
        echo "(the library source changed; update check_dash.sh)" >&2
        exit 2
    fi
}

run_mutation() { # $1 = name, $2 = sed script
    name=$1
    script=$2
    dir="$tmp/mut_$name"
    cp -R "$tmp/base" "$dir"
    target="$dir/source/plutovg-path.c"
    sed -i.bak -e "$script" "$target"
    rm -f "$target.bak"
    build_and_run "$dir" "$dir/dash_contract"
    if "$dir/dash_contract" > "$dir/run.log" 2>&1; then
        echo "MISSED: mutation '$name' was NOT detected by the tests"
        return 1
    fi
    echo "detected: mutation '$name' rejected; first failure:"
    grep -m1 '^FAIL' "$dir/run.log" | sed 's/^/    /'
    return 0
}

path_c="$tmp/base/source/plutovg-path.c"

# 1. Negative offset normalization: drop the wrap of negative start_phase
#    into [0, dash_sum).
require_pattern 'if(dasher\.start_phase < 0\.f)' "$path_c" negative-offset-normalization
m1='/if(dasher\.start_phase < 0\.f)/,+1d'

# 2. Odd period: drop the doubling of dash_sum for odd-length arrays.
require_pattern 'if(ndashes % 2 == 1)' "$path_c" odd-period
m2='/if(ndashes % 2 == 1)/,+1d'

# 3. MOVE_TO phase restart: drop the per-subpath reset of phase/index/toggle.
require_pattern 'dasher->phase = dasher->start_phase;' "$path_c" moveto-phase-restart

rc=0
run_mutation negative-offset-normalization "$m1" || rc=1
run_mutation odd-period "$m2" || rc=1
# The third mutation deletes three separate lines; run it inline.
name=moveto-phase-restart
dir="$tmp/mut_$name"
cp -R "$tmp/base" "$dir"
target="$dir/source/plutovg-path.c"
sed -i.bak \
    -e '/dasher->phase = dasher->start_phase;/d' \
    -e '/dasher->index = dasher->start_index;/d' \
    -e '/dasher->toggle = dasher->start_toggle;/d' \
    "$target"
rm -f "$target.bak"
build_and_run "$dir" "$dir/dash_contract"
if "$dir/dash_contract" > "$dir/run.log" 2>&1; then
    echo "MISSED: mutation '$name' was NOT detected by the tests"
    rc=1
else
    echo "detected: mutation '$name' rejected; first failure:"
    grep -m1 '^FAIL' "$dir/run.log" | sed 's/^/    /'
fi

if [ $rc -eq 0 ]; then
    echo "PASS mutations: all 3 mutations were detected"
else
    echo "FAIL mutations: at least one mutation went undetected" >&2
fi
exit $rc
