#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/uf-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O1 -Iinclude -Ichecks checks/target.cpp checks/suite_main.cpp -o "$b/target"
for group in coupling reactive realtime; do
 "$b/target" "$group"
done
bash checks/baseline.sh
