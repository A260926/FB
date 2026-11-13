#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
bash task_support/build.sh "$work/target" task_support/harness.c tests/evex_contract.c
if "$work/target" > "$work/target.log" 2>&1; then cat "$work/target.log"; else result=$?; cat "$work/target.log"; exit "$result"; fi
awk '$1=="GROUP_COMPLETE" && $2=="prefix" && $3 ~ /^cases=[1-9][0-9]*$/ {ok=1} END{exit !ok}' "$work/target.log"
awk '$1=="GROUP_COMPLETE" && $2=="tuple" && $3 ~ /^cases=[1-9][0-9]*$/ {ok=1} END{exit !ok}' "$work/target.log"
awk '$1=="GROUP_COMPLETE" && $2=="syntax" && $3 ~ /^cases=[1-9][0-9]*$/ {ok=1} END{exit !ok}' "$work/target.log"
awk '$1=="UNIT_ALL_COMPLETE" && $2 ~ /^cases=[1-9][0-9]*$/ && $3 ~ /^checks=[1-9][0-9]*$/ {ok=1} END{exit !ok}' "$work/target.log"
bash task_support/legacy.sh
echo TARGET_COMPLETE
