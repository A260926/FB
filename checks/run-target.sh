#!/bin/bash
set -euo pipefail
for source in checks/intersections.c checks/check-intersections.sh; do
  test -s "$source" || { echo "QA_MISSING_OR_EMPTY $source" >&2; exit 64; }
done
mkdir -p checks/.tmp
b=$(mktemp -d checks/.tmp/target.XXXXXX)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -Dsisl_COMPILE_APPS=OFF -Dsisl_COMPILE_EXAMPLES=OFF -Dsisl_COMPILE_VIEWER=OFF >"$b/configure.log" 2>&1 || { cat "$b/configure.log"; exit 1; }
cmake --build "$b" --target sisl -j 8 >"$b/build.log" 2>&1 || { cat "$b/build.log"; exit 1; }
cc -std=c99 -Iinclude checks/baseline.c "$b/libsisl.a" -lm -o "$b/baseline"
"$b/baseline"
# This executable belongs to the supplied runner, not the new shell script.
# Its registry counts completed C callbacks; shell success markers cannot replace it.
cc -std=c99 -Iinclude -Ichecks checks/section-qa-main.c checks/intersections.c "$b/libsisl.a" -lm -o "$b/intersections"
"$b/intersections" >"$b/direct.log" 2>&1 || { cat "$b/direct.log"; exit 1; }
cat "$b/direct.log"
grep -Eq '^SECTION_QA_EXECUTED cases=[1-9][0-9]*$' "$b/direct.log"
bash checks/check-intersections.sh >"$b/result" 2>&1 || { cat "$b/result"; exit 1; }
cat "$b/result"
for tag in topology paired-curves analytic-coverage; do
  grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$b/direct.log"
  grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$b/result"
done
for tag in missing-branch marching-plane; do grep -Fx "DETECTED $tag" "$b/result"; done
grep -Fx 'SISL_SECTION_QA_OK' "$b/result"
