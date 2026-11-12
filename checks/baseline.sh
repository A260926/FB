#!/bin/bash
set -euo pipefail
if [[ ! -s .checks-build-dir ]] || [[ ! -x "$(cat .checks-build-dir)/origin" ]]; then bash checks/setup.sh; fi
b=$(cat .checks-build-dir)
cmake --build "$b" --target origin -j2
"$b/origin"
