#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
export NODE_PATH="$PWD/support/deps" WS_NO_BUFFER_UTIL=1 WS_NO_UTF_8_VALIDATE=1
export npm_config_offline=true
mode=${1:-baseline}
files=(support/baseline.test.cjs)
if [[ "$mode" == target ]]; then test -s event-stream.test.cjs; files+=(event-stream.test.cjs)
elif [[ "$mode" != baseline ]]; then exit 2; fi
node support/run-tests.cjs "$mode" "${files[@]}"
