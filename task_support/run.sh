#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
mkdir "$build/include"
sed -e 's/${PT_VERSION_MAJOR}/2/g' -e 's/${PT_VERSION_MINOR}/3/g' -e 's/${PT_VERSION_PATCH}/0/g' libipt/include/intel-pt.h.in > "$build/include/intel-pt.h"
incs=(-I"$build/include" -Ilibipt/internal/include -Iinclude -I. -Iptunit/include)
sources=(libipt/src/pt_encoder.c libipt/src/pt_last_ip.c libipt/src/pt_event_decoder.c libipt/src/pt_sync.c libipt/src/pt_tnt_cache.c libipt/src/pt_time.c libipt/src/pt_event_queue.c libipt/src/pt_query_decoder.c libipt/src/pt_packet.c libipt/src/pt_packet_decoder.c libipt/src/pt_config.c libipt/src/pt_block_cache.c)
build_case() {
 cc -std=c11 -O1 "${incs[@]}" "${sources[@]}" "$1" -o "$2"
 cc -std=c11 -O1 "${incs[@]}" libipt/src/pt_time.c ptunit/src/ptunit.c libipt/test/src/ptunit-time.c -o "$build/old-time"
 "$build/old-time"
}
build_target() { cc -std=c11 -O1 "${incs[@]}" "${sources[@]}" task_support/main.c tests/task_cases.c -o "$build/target"; }
if [[ "$mode" == baseline ]]; then
  build_case task_support/baseline.c "$build/baseline"
  "$build/baseline"
elif [[ "$mode" == target ]]; then
  test -s tests/task_cases.c || { echo 'missing tests/task_cases.c' >&2; exit 2; }
  build_target
  "$build/target"
  test -s tests/mutate_timing.sh || { echo "missing tests/mutate_timing.sh" >&2; exit 2; }
  bash tests/mutate_timing.sh
  build_case task_support/baseline.c "$build/baseline"
  "$build/baseline"
else
  exit 2
fi
