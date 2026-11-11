#!/usr/bin/env bash
# End-to-end, windowless regression for the Poly2Tri CDT -> interior mesh path.
#
#   devcheck/check-cdt.sh <svg-output-directory>
#
# The normal sources must pass every input/variant.  Then three independent
# temporary source copies are mutated (one defect each), compiled, run with the
# same driver, and required to complete triangulation and be caught by the
# matching assertion.  A compile failure, timeout or signal is never counted as
# a successful detection.

set -u

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
driver="$root/devcheck/cdt-mesh.cpp"
CXX=${CXX:-c++}

artifacts=${1:-}
if [ -z "$artifacts" ]; then
  echo "usage: $0 <svg-output-directory>" >&2
  exit 2
fi
mkdir -p "$artifacts" || exit 2

tmpbase=${TMPDIR:-/tmp}
work=$(mktemp -d "$tmpbase/p2t-check.XXXXXX") || exit 2
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

fail() {
  echo "CHECK_CDT_ERROR: $*" >&2
  exit 1
}

# Compile a source tree (normal or a temporary mutated copy) with the same
# driver and flags used by devcheck/build-run.sh.
compile_tree() {
  local tree=$1
  local bin=$2
  local drv=$3
  "$CXX" -std=c++14 -O0 -DP2T_STATIC_EXPORTS -I"$tree" \
    "$drv" \
    "$tree/poly2tri/common/shapes.cc" \
    "$tree/poly2tri/sweep/advancing_front.cc" \
    "$tree/poly2tri/sweep/cdt.cc" \
    "$tree/poly2tri/sweep/sweep.cc" \
    "$tree/poly2tri/sweep/sweep_context.cc" \
    -o "$bin"
}

# Portable timeout: run a binary for at most 60 seconds.  Returns 124 on
# timeout and an ordinary wait status otherwise (128+N for a signal death).
run_bin() {
  local bin=$1
  local log=$2
  shift 2
  "$bin" "$@" >"$log" 2>&1 &
  local pid=$!
  local ticks=0
  while kill -0 "$pid" 2>/dev/null; do
    sleep 1
    ticks=$((ticks + 1))
    if [ "$ticks" -ge 60 ]; then
      kill -TERM "$pid" 2>/dev/null || true
      sleep 1
      kill -KILL "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null
      return 124
    fi
  done
  wait "$pid"
  return $?
}

require_line() {
  local file=$1
  local line=$2
  grep -qx "$line" "$file"
}

# ---------------------------------------------------------------------------
# 1. Normal sources: every input must pass, and the SVG must be produced.
# ---------------------------------------------------------------------------
normal="$work/normal"
normal_log="$work/normal.log"
echo "== normal sources =="
if ! compile_tree "$root" "$normal" "$driver"; then
  fail "normal source compile failed"
fi
run_bin "$normal" "$normal_log" "$artifacts"
normal_rc=$?
cat "$normal_log"
echo "normal-driver-exit=$normal_rc"
if [ "$normal_rc" -ne 0 ]; then
  fail "normal driver exited $normal_rc (a release-built library must pass every input)"
fi
for name in coverage topology delaunay identity; do
  require_line "$normal_log" "PASS $name" || fail "normal sources did not PASS $name"
done
require_line "$normal_log" "CDT_MESH_DRIVER_OK" || fail "normal sources did not finish cleanly"
if ! find "$artifacts" -name '*.svg' -type f -size +100c | grep -q .; then
  fail "no SVG artifact >= 100 bytes was written to $artifacts"
fi
echo "normal-observations: $(grep -c '^OBS ' "$normal_log") variants"
echo "svgs: $(find "$artifacts" -name '*.svg' -type f | wc -l | tr -d ' ')"

# ---------------------------------------------------------------------------
# 2. Three independent mutated source copies.
# ---------------------------------------------------------------------------
# apply_mutation <tree> <file-relative-to-tree> <sed-expression> <marker>
apply_mutation() {
  local tree=$1
  local file=$2
  local expr=$3
  local marker=$4
  local path="$tree/$file"
  sed -i.bak "$expr" "$path" || return 1
  grep -qF "$marker" "$path" || return 1
  rm -f "$path.bak"
}

# run_mutant <name> <expected-assert-category> <description>
# The tree and sed mutation are prepared by the caller in $mut_tree/$mut_file.
check_mutant() {
  local name=$1
  local expected=$2
  local description=$3
  local tree="$work/$name"
  local bin="$work/$name.bin"
  local log="$work/$name.log"
  local mut_art="$work/$name-artifacts"
  mkdir -p "$mut_art"

  echo "== mutant: $name =="
  echo "   mutation: $description"

  if ! compile_tree "$tree" "$bin" "$driver"; then
    fail "$name did not compile (a compile failure is not a detection)"
  fi
  echo "   compile: ok"

  run_bin "$bin" "$log" "$mut_art"
  local rc=$?
  cat "$log"
  echo "   driver-exit: $rc"

  # Timeout and signal deaths are not acceptable detections.
  if [ "$rc" -eq 124 ]; then
    fail "$name timed out (not a detection)"
  fi
  if [ "$rc" -ge 128 ]; then
    fail "$name died from a signal (exit $rc); not a detection"
  fi
  if [ "$rc" -eq 0 ]; then
    fail "$name completed with exit 0 although the mutated behaviour is wrong"
  fi

  # The mutant must have triangulated and printed observation data before the
  # assertion fired.
  grep -q '^OBS ' "$log" || fail "$name produced no triangulation observations"
  grep -q '^SUMMARY ' "$log" || fail "$name did not complete all input variants"
  grep -q "^ASSERT_FAIL $expected " "$log" ||
    fail "$name was not caught by the expected '$expected' assertion"

  # Report exactly which concrete assertion fired (the first matching detail).
  local caught
  caught=$(grep -m1 "^ASSERT_FAIL $expected " "$log")
  echo "DETECTED $name"
  echo "   caught-by: $expected"
  echo "   assertion: $caught"
  echo "   observation-lines: $(grep -c '^OBS ' "$log")"
  grep -m1 '^SUMMARY ' "$log" | sed 's/^/   /'
  echo "   abnormal-exit-code: $rc"
}

# --- 2a. skip Steiner registration in SweepContext::AddPoint ----------------
mut="$work/skip-steiner"
mkdir -p "$mut"
cp -R "$root/poly2tri" "$mut/poly2tri" || fail "could not copy sources for skip-steiner"
apply_mutation "$mut" "poly2tri/sweep/sweep_context.cc" \
  's/^  points_\.push_back(point);$/  \/* mutated: AddPoint registration skipped *\//' \
  'mutated: AddPoint registration skipped' ||
  fail "could not apply skip-steiner mutation"
check_mutant "skip-steiner" "point-coverage" \
  "removed points_.push_back(point) from SweepContext::AddPoint (Steiner point never enters the sweep)"

# --- 2b. skip AddHole constrained-edge setup ---------------------------------
mut="$work/open-hole"
mkdir -p "$mut"
cp -R "$root/poly2tri" "$mut/poly2tri" || fail "could not copy sources for open-hole"
apply_mutation "$mut" "poly2tri/sweep/sweep_context.cc" \
  's/^  InitEdges(polyline);$/  \/* mutated: AddHole constrained edges skipped *\//' \
  'mutated: AddHole constrained edges skipped' ||
  fail "could not apply open-hole mutation"
check_mutant "open-hole" "hole-coverage" \
  "removed InitEdges(polyline) from SweepContext::AddHole (hole vertices kept, hole constraints lost)"

# --- 2c. disable the incircle flip branch in Sweep::Legalize -----------------
mut="$work/skip-legalize"
mkdir -p "$mut"
cp -R "$root/poly2tri" "$mut/poly2tri" || fail "could not copy sources for skip-legalize"
apply_mutation "$mut" "poly2tri/sweep/sweep.cc" \
  's|^      bool inside = Incircle(\*p, \*t\.PointCCW(\*p), \*t\.PointCW(\*p), \*op);$|      bool inside = false; /* mutated: incircle flip disabled */|' \
  'mutated: incircle flip disabled' ||
  fail "could not apply skip-legalize mutation"
check_mutant "skip-legalize" "delaunay" \
  "forced Incircle's result to false in Sweep::Legalize (no Delaunay edge flips)"

echo "POLY2TRI_MESH_TARGET_OK"
exit 0
