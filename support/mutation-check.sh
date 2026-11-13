#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
base=$PWD
work=$(mktemp -d "${TMPDIR:-/tmp}/mutations.XXXXXX")
trap 'rm -rf "$work"' EXIT
for kind in counter direction termination; do
 copy="$work/$kind"
 mkdir -p "$copy"
 tar --exclude=.git --exclude='build*' -cf - . | tar -xf - -C "$copy"
 (cd "$copy" && patch -s -p1 < "$base/support/mutations/$kind.patch")
 cmake -S "$copy" -B "$copy/build" -DBDD_INCLUDE_TOOL=OFF -DBDD_INCLUDE_ISAGENERATOR_X86=OFF -DBDD_INCLUDE_TESTS=OFF -DBDD_INCLUDE_FUZZERS=OFF > "$work/$kind-configure.log"
 cmake --build "$copy/build" -j2 > "$work/$kind-build.log"
 cc -std=c11 -I"$copy" -I"$copy/inc" "$copy/support/main.c" "$copy/regress_tests/regression.c" "$copy/build/libbdshemu.a" "$copy/build/libbddisasm.a" -o "$copy/test"
 set +e
 SHEMU_TARGET_CASE="rep_${kind}_witness" "$copy/test" > "$work/$kind-stdout" 2> "$work/$kind-stderr"
 status=$?
 set -e
 cat "$work/$kind-stdout" "$work/$kind-stderr"
 if [[ "$status" != 1 ]] || ! grep -q "^FAIL tag=$kind " "$work/$kind-stderr"; then
   echo "MUTATION_RUNNER_ERROR $kind status=$status" >&2
   exit 1
 fi
 echo "MUTATION_KILLED $kind build=0 semantic_exit=1"
done
printf '%s\n' 'MUTATIONS_COMPLETE count=3'
