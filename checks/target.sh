#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/uf-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O1 -I. -Ichecks checks/target.cpp checks/suite_main.cpp iir/*.cpp -o "$b/target"
for group in boundaries reference reuse; do
 "$b/target" "$group"
done
bash checks/baseline.sh
