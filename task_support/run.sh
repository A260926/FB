#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/hls_parts.cpp; fi
if [[ $mode == target ]]; then
 test -s liveMedia/include/HLSPartPublisher.hh
 test -s liveMedia/HLSPartPublisher.cpp
 test -s examples/hls-parts.cpp
fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++20 -pthread -DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t -I. -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include -include liveMedia/include/HLSPartPublisher.hh task_support/api_consumer.cpp .task-build/unit-*.o -o .task-build/api-consumer
.task-build/api-consumer
c++ -std=c++20 -pthread -DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t -I. -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include -include liveMedia/include/HLSPartPublisher.hh task_support/driver.c task_tests/hls_parts.cpp .task-build/unit-*.o -o .task-build/target
rm -f .task-build/hls-example
c++ -std=c++20 -pthread -DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t -I. -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include -include liveMedia/include/HLSPartPublisher.hh examples/hls-parts.cpp .task-build/unit-*.o -o .task-build/hls-example
example_dir=$(mktemp -d .task-build/hls-example.XXXXXX)
.task-build/hls-example "$example_dir" > "$example_dir/events.log"
cat "$example_dir/events.log"
example_ts=$(find "$example_dir" -type f -name '*.ts' -size +0c -print -quit)
test -n "$example_ts"
test -s "$example_dir/events.log"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in clock ranges lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
