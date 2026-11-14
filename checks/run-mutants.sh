#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/run.sh target
mutation_work=$(mktemp -d "${TMPDIR:-/tmp}/task-mutants.XXXXXX")
trap 'rm -rf "$mutation_work"' EXIT
for mutation_name in double-clock release-wrap slope-trunc; do
  mutation_copy="$mutation_work/$mutation_name"
  mkdir "$mutation_copy"
  tar --exclude=.git --exclude=.task-build -cf - . | (cd "$mutation_copy" && tar -xf -)
  (cd "$mutation_copy" && patch -p1 < "checks/mutations/$mutation_name.patch") >"$mutation_work/$mutation_name.patch.log" 2>&1
  set +e
  (cd "$mutation_copy" && bash task_support/run.sh target) >"$mutation_work/$mutation_name.log" 2>&1
  mutation_status=$?
  set -e
  if [[ $mutation_status != 4 ]] || ! grep -q '^CONTENT_MISMATCH ' "$mutation_work/$mutation_name.log" || ! grep -q '^FAIL ' "$mutation_work/$mutation_name.log"; then
    cat "$mutation_work/$mutation_name.log"
    echo "MUTANT_NOT_CONTENT_REJECTED $mutation_name status=$mutation_status" >&2
    exit 1
  fi
  echo "MUTANT_CONTENT_REJECTED $mutation_name"
done
echo "MUTATION_CHECK_COMPLETE count=3"
