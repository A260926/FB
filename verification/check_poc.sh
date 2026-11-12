#!/usr/bin/env bash
# Verifies the access-unit-level picture order count reader:
#   1. runs the poc_contract integration tests (must be >=10 passing, 0 ignored)
#   2. runs the poc_dump example against the checked-in fixture and checks the
#      fixed picture order count sequence
set -euo pipefail

cd "$(dirname "$0")/.."

b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
export CARGO_TARGET_DIR="$b/target"
export CARGO_HOME="$PWD/.cargo-cache"

echo "== running poc_contract tests =="
cargo +1.88.0 test --locked --test poc_contract -- --nocapture 2>&1 | tee "$b/poc.log"

# Require an all-green summary (zero failures, zero ignored) and at least ten
# actual passing tests.
summary=$(grep -E '^test result:' "$b/poc.log" | tail -1)
echo "$summary"
echo "$summary" | grep -Eq 'test result: ok\. [0-9]+ passed; 0 failed; 0 ignored'
passes=$(echo "$summary" | sed -E 's/.*ok\. ([0-9]+) passed.*/\1/')
if [ "$passes" -lt 10 ]; then
    echo "expected at least 10 passing tests, found $passes" >&2
    exit 1
fi

echo "== running poc_dump example against the fixture =="
cargo +1.88.0 run --locked --example poc_dump -- verification/poc-fixture.h264 \
    2>&1 | tee "$b/dump.log"

# Fixed POC sequence derived from verification/poc-fixture.h264 (type 0,
# MaxPicOrderCntLsb 16): an IDR, then reference and non-reference pictures.
grep -Fq 'picture 1: frame_num=0 sps=0 pps=0 top=0 bottom=- min=0' "$b/dump.log"
grep -Fq 'picture 2: frame_num=1 sps=0 pps=0 top=2 bottom=- min=2' "$b/dump.log"
grep -Fq 'picture 3: frame_num=2 sps=0 pps=0 top=4 bottom=- min=4' "$b/dump.log"
grep -Fq 'picture 4: frame_num=2 sps=0 pps=0 top=4 bottom=- min=4' "$b/dump.log"
grep -Fq 'picture 5: frame_num=3 sps=0 pps=0 top=6 bottom=- min=6' "$b/dump.log"
grep -Fq 'picture 6: frame_num=4 sps=0 pps=0 top=8 bottom=- min=8' "$b/dump.log"
test "$(grep -c '^picture' "$b/dump.log")" -eq 6

echo H264_POC_PASS
