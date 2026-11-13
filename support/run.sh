#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline) opt=OFF;; target) test -f tests/dual_quaternion_target.cc; opt=ON;; *) exit 2;; esac
cmake -S support -B .build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCHECK_TARGET="$opt"
cmake --build .build --target baseline -j 2
.build/baseline
if [ "$mode" = target ]; then
 cmake --build .build --target target -j 2
 .build/target
 cmake --build .build --target dq_example -j 2
 .build/dq_example
fi
