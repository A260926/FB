#!/bin/bash
#
# Offline mutation testing for the s1851/s1314 plane-section path.
#
# Three independent, temporary copies of the SISL sources are built with
# the same C driver (checks/section-qa-main.c + checks/intersections.c):
#
#   normal         - unmodified production sources; all three assertion
#                    groups must print their PASS counts.
#   missing-branch - src/s1851.c drops one real branch of the two-branch
#                    topology result.  s1314 is untouched, so the surviving
#                    curves are geometrically valid and the defect can only
#                    be caught by the analytic-coverage assertion.
#   marching-plane - src/s1314.c adds a small constant-term bias to the
#                    normalized plane equation; the s1851 topology guide
#                    points are left intact.  The bias is 1e-4, well above
#                    the 1e-7*L plane budget but far too small to push the
#                    oblique cylinder section off the band boundaries.
#
# A copy only counts as detected if it compiles, reaches the intersection
# calls, emits finite observations and is then rejected by the expected
# assertion group.  Compile failures, crashes, timeouts and "no curves
# found" are treated as test harness failures, not as detections.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
work=$(mktemp -d "$here/.tmp/check.XXXXXX")
if [ -z "${QA_KEEP_WORK:-}" ]; then
  trap 'rm -rf "$work"' EXIT
else
  trap - EXIT
  echo "QA_KEEP_WORK=$work" >&2
fi

CC=${CC:-cc}
CFLAGS="-std=c99 -O2 -I$root/include -I$root/checks"
SRC="$root/src"
PLANE_BIAS="1.0e-4"

# Compile every translation unit of one source tree and archive it.
build_tree () {
  local srcdir=$1 lib=$2 define=$3
  local objdir="$work/$(basename "$lib" .a)"
  mkdir -p "$objdir"
  local f obj
  for f in "$srcdir"/*.c; do
    obj="$objdir/$(basename "$f" .c).o"
    if [ -n "$define" ]; then
      $CC $CFLAGS "$define" -c "$f" -o "$obj"
    else
      $CC $CFLAGS -c "$f" -o "$obj"
    fi
  done
  ar rcs "$lib" "$objdir"/*.o
}

# Compile the QA driver against one library.
build_driver () {
  local lib=$1 exe=$2
  $CC $CFLAGS "$root/checks/section-qa-main.c" "$root/checks/intersections.c" \
      "$lib" -lm -o "$exe"
}

# A mutation run must reach real intersection work and print finite values.
finite_observations () {
  local log=$1
  grep -q '^RUN ' "$log" || return 1
  grep -q '^OBS ' "$log" || return 1
  if grep '^OBS ' "$log" | grep -Eiq 'nan|inf'; then return 1; fi
  return 0
}

expect_rejected () {
  local tag=$1 group=$2 log=$3
  if finite_observations "$log"; then
    if grep -q "^QA_CASE_FAILED group=$group " "$log"; then
      echo "DETECTED $tag"
      return 0
    fi
    echo "QA_MUTATION_NOT_REJECTED $tag (expected group=$group)" >&2
  else
    echo "QA_MUTATION_NO_OBSERVATIONS $tag" >&2
  fi
  return 1
}

# ---------------------------------------------------------------- normal
cp -R "$SRC" "$work/normal-src"
build_tree "$work/normal-src" "$work/libnormal.a" ""
build_driver "$work/libnormal.a" "$work/qa-normal"
set +e
"$work/qa-normal" >"$work/normal.log" 2>&1
normal_rc=$?
set -e
cat "$work/normal.log"
[ "$normal_rc" -eq 0 ] || { echo "QA_NORMAL_RUN_FAILED rc=$normal_rc" >&2; exit 1; }
for tag in topology paired-curves analytic-coverage; do
  grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$work/normal.log"
done
grep -Eq '^SECTION_QA_EXECUTED cases=[1-9][0-9]*$' "$work/normal.log"

# --------------------------------------------------------- missing-branch
cp -R "$SRC" "$work/missing-branch-src"
perl -0777 -i -pe 's/(\Q  if(kstat < 0) goto error;\E\n)/$1\n#if defined(QA_DROP_BRANCH)\n  if (wcurve \&\& *wcurve \&\& *jcrv >= 2) {\n    fprintf(stderr, "QA_MUTATION missing-branch: dropping one of %d branches\\n", *jcrv);\n    freeIntcurve((*wcurve)[*jcrv - 1]);\n    (*wcurve)[*jcrv - 1] = SISL_NULL;\n    *jcrv -= 1;\n  }\n#endif\n/' \
  "$work/missing-branch-src/s1851.c"
grep -q 'QA_DROP_BRANCH' "$work/missing-branch-src/s1851.c"
build_tree "$work/missing-branch-src" "$work/libmissing.a" "-DQA_DROP_BRANCH=1"
build_driver "$work/libmissing.a" "$work/qa-missing"
set +e
"$work/qa-missing" >"$work/missing.log" 2>&1
missing_rc=$?
set -e
[ "$missing_rc" -ne 0 ] || { echo "QA_MUTATION_ESCAPED missing-branch" >&2; exit 1; }
expect_rejected "missing-branch" "analytic-coverage" "$work/missing.log"

# --------------------------------------------------------- marching-plane
cp -R "$SRC" "$work/marching-plane-src"
perl -0777 -i -pe 's/\Q  simpli[3] = -s6scpr(epoint,snorm,idim);\E/  \/\* QA mutation: constant-term bias in the normalized plane equation. *\/\n#if defined(QA_PLANE_BIAS)\n  simpli[3] = -s6scpr(epoint,snorm,idim) + (double)(QA_PLANE_BIAS);\n  fprintf(stderr, "QA_MUTATION marching-plane: constant-term bias=%g\\n", (double)(QA_PLANE_BIAS));\n#else\n  simpli[3] = -s6scpr(epoint,snorm,idim);\n#endif/' \
  "$work/marching-plane-src/s1314.c"
grep -q 'QA_PLANE_BIAS' "$work/marching-plane-src/s1314.c"
build_tree "$work/marching-plane-src" "$work/libmarching.a" \
  "-DQA_PLANE_BIAS=$PLANE_BIAS"
build_driver "$work/libmarching.a" "$work/qa-marching"
set +e
"$work/qa-marching" >"$work/marching.log" 2>&1
marching_rc=$?
set -e
[ "$marching_rc" -ne 0 ] || { echo "QA_MUTATION_ESCAPED marching-plane" >&2; exit 1; }
expect_rejected "marching-plane" "paired-curves" "$work/marching.log"

echo "SISL_SECTION_QA_OK"
