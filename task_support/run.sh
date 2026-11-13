#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/sisl-check.XXXXXX")
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release >"$b/config.log" 2>&1 || { cat "$b/config.log"; exit 1; }
cmake --build "$b" --parallel 8 >"$b/build.log" 2>&1 || { cat "$b/build.log"; exit 1; }
cc -std=c99 -Iinclude task_support/baseline.c "$b/libsisl.a" -lm -o "$b/baseline"
if [[ ${1:-baseline} == target ]]; then
 c++ -std=c++17 -Iinclude -Itask_support task_support/main.cpp task_tests/target.cpp "$b/libsisl.a" -lm -o "$b/target"
 # Keep the child's original stdout until the parent has checked completion.
 rc=0
 "$b/target" exact-composition knot-preimages compatibility >"$b/target.stdout" || rc=$?
 cat "$b/target.stdout"
 if [[ $rc -ne 0 ]]; then
  echo "target process failed: exit=$rc" >&2
  exit "$rc"
 fi
 # main.cpp can be bypassed by exit(0) inside a callback. The parent requires
 # all three completed groups, reconciles real callback rows, and checks EOF.
 awk '
 BEGIN {
  group[1]="exact-composition"; group[2]="knot-preimages"; group[3]="compatibility"
  next_group=1
 }
 /^case / {
  if (complete || next_group>3 || index($0,"case " group[next_group] "/")!=1 ||
      $0 !~ /\/[^[:space:]].* checks=[1-9][0-9]*$/) { bad=1; next }
  value=$NF; sub(/^checks=/,"",value)
  cases++; checks+=value
  next
 }
 /^PASS / {
  if (complete || next_group>3 || NF!=4 || $2!=group[next_group] ||
      $3 !~ /^cases=[1-9][0-9]*$/ || $4 !~ /^checks=[1-9][0-9]*$/) { bad=1; next }
  n=$3; sub(/^cases=/,"",n); c=$4; sub(/^checks=/,"",c)
  if (n!=cases || c!=checks || cases==0) bad=1
  cases=0; checks=0; next_group++
  next
 }
 /^TARGET_COMPLETE/ {
  if ($0!="TARGET_COMPLETE" || complete || next_group!=4 || cases!=0) bad=1
  complete=1
  next
 }
 { if (complete) bad=1 }
 END {
  if (bad || !complete || next_group!=4 || cases!=0) {
   print "target stdout incomplete or inconsistent" > "/dev/stderr"
   exit 1
  }
 }
 ' "$b/target.stdout"
fi
"$b/baseline"
