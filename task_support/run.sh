#!/bin/bash
set -euo pipefail
mode=${1:-baseline}
[[ $# -le 1 && ( $mode == baseline || $mode == target ) ]] || { echo "usage: run.sh [baseline|target]" >&2; exit 2; }
b=$(mktemp -d "${TMPDIR:-/tmp}/openmesh-check.XXXXXX")
trap 'rm -rf "$b"' EXIT
core=(src/OpenMesh/Core/Mesh/ArrayKernel.cc src/OpenMesh/Core/Mesh/BaseKernel.cc src/OpenMesh/Core/Mesh/PolyConnectivity.cc src/OpenMesh/Core/Mesh/TriConnectivity.cc src/OpenMesh/Core/Utils/BaseProperty.cc src/OpenMesh/Core/System/omstream.cc)
for f in "${core[@]}"; do c++ -std=c++17 -O0 -Isrc -c "$f" -o "$b/$(basename "$f").o"; done
ar rcs "$b/core.a" "$b"/*.o
c++ -std=c++17 -O0 -Isrc task_support/baseline.cpp "$b/core.a" -o "$b/baseline"
"$b/baseline"
# Upstream legacy tests include C++14-era std::ptr_fun via MeshIO.hh.
c++ -std=c++14 -O0 -Isrc -Itask_support/legacy_gtest task_support/legacy_main.cpp "$b/core.a" -o "$b/legacy"
"$b/legacy"
if [[ $mode == target ]]; then
 c++ -std=c++17 -O0 -Isrc -Itask_support task_support/main.cpp task_tests/target.cpp "$b/core.a" -o "$b/target"
 # Preserve stdout outside the test process before checking its full transcript.
 status=0
 "$b/target" arrangement topology attributes >"$b/target.stdout" || status=$?
 cat "$b/target.stdout"
 if [[ $status != 0 ]]; then
  echo "target process failed: $status" >&2
  exit "$status"
 fi
 awk '
 BEGIN { group[1]="arrangement"; group[2]="topology"; group[3]="attributes"; current=1 }
 function reject(message) { print "target transcript rejected: " message > "/dev/stderr"; bad=1; exit 1 }
 {
  if (complete) reject("output after completion")
  if ($1 == "case") {
   if (current > 3 || NF != 3 || $3 !~ /^checks=[1-9][0-9]*$/) reject("invalid case")
   slash=index($2,"/")
   if (!slash || substr($2,1,slash-1) != group[current] || length($2) == slash) reject("case group/order")
   split($3,value,"="); cases++; checks+=value[2]
  } else if ($1 == "PASS") {
   if (current > 3 || NF != 4 || $2 != group[current] || $3 !~ /^cases=[1-9][0-9]*$/ || $4 !~ /^checks=[1-9][0-9]*$/) reject("invalid PASS")
   split($3,n,"="); split($4,c,"=")
   if (n[2] != cases || c[2] != checks) reject("case/check totals")
   current++; cases=0; checks=0
  } else if ($1 == "TARGET_COMPLETE") {
   if ($0 != "TARGET_COMPLETE" || current != 4) reject("premature completion")
   complete=1
  }
 }
 END {
  if (bad) exit 1
  if (!complete || current != 4) { print "target transcript rejected: incomplete required groups/completion" > "/dev/stderr"; exit 1 }
 }
 ' "$b/target.stdout"
fi
