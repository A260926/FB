#!/usr/bin/env bash
# Geometry regression for the xatlas baking path.
#
#   bash source/test/check_atlas_geometry.sh
#
# 1. Compiles the real source/xatlas/xatlas.cpp in a temp dir together with
#    source/test/atlas_geometry_regression.cpp and runs the generated cases.
# 2. Applies three small benign degradations to temp copies of the library
#    source; each copy must still compile and must be caught by a concrete
#    numeric/ownership assertion of the regression binary.
# 3. Prints the real executed case count and, on success, XATLAS_GEOMETRY_OK.
#
# checks/baseline.sh stays untouched: it records the plain generation flow.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/xatlas-geometry.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

cxx=${CXX:-c++}
cxxflags=(-std=c++11 -O1 -pthread)
regression=source/test/atlas_geometry_regression.cpp

echo "== building regression binary (real xatlas.cpp) =="
"$cxx" "${cxxflags[@]}" -Isource/xatlas source/xatlas/xatlas.cpp "$regression" -o "$tmp/atlas_geometry_regression"

echo "== running geometry regression =="
if ! out=$("$tmp/atlas_geometry_regression" "$root"); then
	printf '%s\n' "$out"
	echo "geometry regression FAILED on the unmodified library"
	exit 1
fi
printf '%s\n' "$out"
cases=$(printf '%s\n' "$out" | sed -n 's/^ATLAS_GEOMETRY_CASES=//p')
failures=$(printf '%s\n' "$out" | sed -n 's/^ATLAS_GEOMETRY_FAILURES=//p')
if [ -z "$cases" ] || [ "$failures" != "0" ]; then
	echo "geometry regression did not report a clean run (cases='$cases' failures='$failures')"
	exit 1
fi
echo "geometry cases executed: $cases"

# --- benign degradations -----------------------------------------------------
# Each mutation is applied to a private copy of the library sources. The copy
# must compile (a build failure is never counted as a detection) and the
# regression binary must reject it with a [FAIL] assertion.
apply_mutation() {
	local file=$1 name=$2
	case "$name" in
	xref-shift)
		# Output xref is shifted onto another (still valid) source vertex.
		perl -0pi -e 's/vertex\.xref = chart->mapChartVertexToSourceVertex\(v\);/vertex.xref = (chart->mapChartVertexToSourceVertex(v) + 1) % ctx->meshes[i]->vertexCount();/' "$file"
		grep -q 'vertex.xref = (chart->mapChartVertexToSourceVertex(v) + 1)' "$file"
		;;
	chartindex-local)
		# Vertex chartIndex is written as a per-mesh local id instead of the
		# atlas-global chart id (identical for mesh 0, wrong from mesh 1 on).
		perl -0pi -e 's/vertex\.chartIndex = \(int32_t\)chartIndex;/vertex.chartIndex = (int32_t)meshChartIndex;/' "$file"
		grep -q 'vertex.chartIndex = (int32_t)meshChartIndex;' "$file"
		;;
	ignored-atlas)
		# Ignored-face output vertices wrongly carry a valid atlas index.
		perl -0pi -e 's/vertex\.atlasIndex = -1;\s*\n\s*vertex\.chartIndex = -1;\s*\n\s*vertex\.uv\[0\] = vertex\.uv\[1\] = 0\.0f;/vertex.atlasIndex = 0;\n\t\t\t\t\tvertex.chartIndex = -1;\n\t\t\t\t\tvertex.uv[0] = vertex.uv[1] = 0.0f;/' "$file"
		grep -q 'vertex.atlasIndex = 0;' "$file"
		;;
	*)
		echo "unknown mutation '$name'"
		return 1
		;;
	esac
}

for mutation in xref-shift chartindex-local ignored-atlas; do
	mdir="$tmp/mut-$mutation"
	mkdir -p "$mdir"
	cp source/xatlas/xatlas.cpp source/xatlas/xatlas.h "$mdir/"
	if ! apply_mutation "$mdir/xatlas.cpp" "$mutation"; then
		echo "mutation '$mutation': failed to apply source edit"
		exit 1
	fi
	echo "== mutation '$mutation': building degraded copy =="
	if ! "$cxx" "${cxxflags[@]}" -I"$mdir" "$mdir/xatlas.cpp" "$regression" -o "$mdir/check" 2> "$mdir/build.log"; then
		cat "$mdir/build.log"
		echo "mutation '$mutation': BUILD FAILED (not counted as a detection)"
		exit 1
	fi
	echo "== mutation '$mutation': running (expecting detection) =="
	status=0
	(cd "$mdir" && ./check "$mdir") > "$mdir/run.log" 2>&1 || status=$?
	if [ "$status" -eq 0 ]; then
		echo "mutation '$mutation': NOT DETECTED (regression passed on degraded library)"
		exit 1
	fi
	if ! grep -q '^\[FAIL\]' "$mdir/run.log"; then
		cat "$mdir/run.log"
		echo "mutation '$mutation': exit code $status but no failing assertion was reported"
		exit 1
	fi
	echo "mutation '$mutation' detected (exit $status); failing assertion(s):"
	grep '^\[FAIL\]' "$mdir/run.log" | sed -n '1,5p'
done

echo "geometry cases executed: $cases"
echo "XATLAS_GEOMETRY_OK"
