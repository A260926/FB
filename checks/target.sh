#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/uf-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++14 -O1 -pthread -Iinclude -Isrc -Ichecks checks/target.cpp checks/suite_main.cpp src/{Circuit,Component,Signal,SignalBus}.cpp src/internal/*.cpp -o "$b/target"
for group in recurrence ownership scheduling; do
 "$b/target" "$group"
done
bash checks/baseline.sh
