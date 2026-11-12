#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/uf-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
source checks/build.sh
c++ -std=c++14 -O1 -Iinclude -Isrc -Ichecks checks/target.cpp checks/suite_main.cpp "$b"/*.o -lm -o "$b/target"
for group in model recurrence stream; do
 "$b/target" "$group"
done
bash checks/baseline.sh
