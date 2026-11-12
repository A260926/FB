#!/bin/bash
set -euo pipefail
test -s test/root_motion_test.c || { echo 'missing test/root_motion_test.c' >&2; exit 1; }
test -s examples/cgltf_motion.c || { echo 'missing examples/cgltf_motion.c' >&2; exit 1; }
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O0 -g -I. test/root_motion_test.c examples/cgltf_motion.c -lm -o "$b/test"
"$b/test" | tee "$b/output"
grep -Eq '^ROOT_MOTION_OK cases=[1-9][0-9]*$' "$b/output"
bash checks/run-baseline.sh
