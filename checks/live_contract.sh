#!/usr/bin/env bash
#
# live_contract.sh - build and run the Rubber Band Live regression
# contract.  The compile line mirrors checks/run.sh (single-file
# build with the built-in FFT and BQ resampler, no third-party
# libraries); only the sources under checks/ are added.
#
#   bash checks/live_contract.sh
#
# On success the contract binary prints LIVE_CONTRACT_OK and this
# script exits 0.  Any regression prints a named LIVE_*_DETECTED
# marker and the script exits non-zero.
#
# The pre-existing checks/baseline.cpp short-window/unity sanity test
# is retained and is built and run here as well.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)

# mktemp is unavailable in some sandboxes; $TMPDIR is the writable
# scratch directory handed to the session, fall back to /tmp.
work=${TMPDIR:-/tmp}/live-contract.$$
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

cxx=${CXX:-c++}
flags=(-std=c++17 -O2
       -DALREADY_CONFIGURED -DUSE_BQRESAMPLER -DUSE_BUILTIN_FFT
       -DNO_THREADING -DNO_TIMING -DNO_THREAD_CHECKS -DLACK_SINCOS
       -I"$root")

echo "== building baseline (checks/baseline.cpp) =="
"$cxx" "${flags[@]}" "$root/checks/baseline.cpp" \
       "$root/single/RubberBandSingle.cpp" -pthread -o "$work/baseline"
"$work/baseline"

echo "== building live contract (checks/live_contract.cpp) =="
"$cxx" "${flags[@]}" "$root/checks/live_contract.cpp" \
       "$root/single/RubberBandSingle.cpp" -pthread -o "$work/live_contract"
"$work/live_contract"
