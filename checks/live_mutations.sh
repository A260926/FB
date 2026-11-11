#!/usr/bin/env bash
#
# live_mutations.sh - mutation-testing harness for the Live contract.
#
# Three independent temporary copies of the source tree are made; each
# receives exactly one source-level defect, must still COMPILE, and
# must then be rejected by the unchanged audio assertions in
# checks/live_contract.cpp (which is copied in unmodified).  A compile
# error, a missing file, or a mere source-string search is never
# accepted as detection: the mutated library has to produce wrong
# audio or a wrong delay at runtime, and the contract binary has to
# exit non-zero while printing the named DETECTED marker.
#
#   mutation  target file              defect                     expected marker
#   pitch     src/rubberband-c.cpp     C wrapper ignores scale    LIVE_PITCH_DETECTED
#   relation  src/finer/R3LiveShifter  mid/side decode L-R -> L+R LIVE_RELATION_DETECTED
#   latency   src/finer/R3LiveShifter  start delay + one block    LIVE_LATENCY_DETECTED
#
# The original working tree is never edited; everything happens under
# $TMPDIR.  If any mutated copy compiles but is not caught, this
# script fails.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)

work=${TMPDIR:-/tmp}/live-mutations.$$
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

cxx=${CXX:-c++}
flags=(-std=c++17 -O2
       -DALREADY_CONFIGURED -DUSE_BQRESAMPLER -DUSE_BUILTIN_FFT
       -DNO_THREADING -DNO_TIMING -DNO_THREAD_CHECKS -DLACK_SINCOS)

# Anchor files whose integrity proves the real tree was not touched.
guard_files=(src/rubberband-c.cpp src/finer/R3LiveShifter.cpp)
before=$(for f in "${guard_files[@]}"; do shasum -a 256 "$root/$f"; done)

# Copy only what the single-file build needs; no .git, no scratch.
make_copy() {
    local dst=$1
    mkdir -p "$dst/checks"
    cp -R "$root/src" "$dst/src"
    cp -R "$root/single" "$dst/single"
    cp -R "$root/rubberband" "$dst/rubberband"
    cp "$root/checks/live_contract.cpp" "$dst/checks/live_contract.cpp"
}

# apply_mutation <name> <dst>
apply_mutation() {
    local name=$1 dst=$2
    case "$name" in
        pitch)
            # Make the LIVE C wrapper discard the requested ratio.
            # Anchored on the whole function so the stretcher wrapper
            # of the same name further up the file is untouched.
            perl -0777 -i -pe '
                s/void rubberband_live_set_pitch_scale\(RubberBandLiveState state, double scale\)\s*\{\s*state->m_s->setPitchScale\(scale\);\s*\}/
                 void rubberband_live_set_pitch_scale(RubberBandLiveState state, double scale)\n{\n    (void)scale; \/* MUTATION pitch: wrapper ignores requested scale *\/\n}/s
            ' "$dst/src/rubberband-c.cpp"
            ;;
        relation)
            # Mid/side decode: right channel subtraction becomes
            # addition, so both outputs collapse onto m + s.
            perl -0777 -i -pe '
                s/float r = m - s;/float r = m + s; \/* MUTATION relation *\//
            ' "$dst/src/finer/R3LiveShifter.cpp"
            ;;
        latency)
            # Report a start delay one processing block too large.
            perl -0777 -i -pe '
                s/    int total = outDelay;\n/    int total = outDelay + int(getBlockSize()); \/* MUTATION latency: over-report one block *\/\n/
            ' "$dst/src/finer/R3LiveShifter.cpp"
            ;;
        *) echo "unknown mutation $name" >&2; exit 2 ;;
    esac
    # Exactly one injected site, or this copy is invalid.
    local target hits
    if [ "$name" = pitch ]; then
        target=$dst/src/rubberband-c.cpp
    else
        target=$dst/src/finer/R3LiveShifter.cpp
    fi
    hits=$(grep -c "MUTATION $name" "$target")
    if [ "$hits" -ne 1 ]; then
        echo "FAIL: $name mutation applied $hits times (expected 1)" >&2
        exit 1
    fi
}

# run_mutation <name> <expected-marker>
run_mutation() {
    local name=$1 marker=$2
    local dst="$work/mut-$name"
    echo "== mutation: $name =="
    make_copy "$dst"
    apply_mutation "$name" "$dst"

    # The defect must not break the build; a compile failure is an
    # error of this harness, not a detection.
    if ! "$cxx" "${flags[@]}" -I"$dst" \
            "$dst/checks/live_contract.cpp" \
            "$dst/single/RubberBandSingle.cpp" -pthread \
            -o "$dst/contract" 2> "$work/$name.compile.log"; then
        echo "FAIL: $name copy did not compile" >&2
        cat "$work/$name.compile.log" >&2
        exit 1
    fi
    echo "  compiled ok"

    # The unchanged audio assertions must reject it at runtime.
    set +e
    "$dst/contract" >"$work/$name.run.out" 2>"$work/$name.run.err"
    local rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        echo "FAIL: $name mutation was NOT detected (contract exited 0)" >&2
        exit 1
    fi
    if ! grep -q "$marker" "$work/$name.run.err"; then
        echo "FAIL: $name run failed but $marker missing" >&2
        cat "$work/$name.run.err" >&2
        exit 1
    fi
    if grep -q 'LIVE_CONTRACT_OK' "$work/$name.run.out"; then
        echo "FAIL: $name run printed LIVE_CONTRACT_OK despite defect" >&2
        exit 1
    fi

    # Surface the named marker and the first explanatory line.
    grep "$marker" "$work/$name.run.err" | head -1
    grep '^\s*\[' "$work/$name.run.err" | head -2 | sed 's/^/  /'
    echo "  DETECTED (exit $rc)"
}

run_mutation pitch    LIVE_PITCH_DETECTED
run_mutation relation LIVE_RELATION_DETECTED
run_mutation latency  LIVE_LATENCY_DETECTED

after=$(for f in "${guard_files[@]}"; do shasum -a 256 "$root/$f"; done)
if [ "$before" != "$after" ]; then
    echo "FAIL: original working tree was modified" >&2
    exit 1
fi

echo "LIVE_MUTATIONS_OK: 3/3 defects compiled and were caught by named markers"
