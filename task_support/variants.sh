#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/run.sh target
bash task_support/run.sh baseline
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
for variant in unsigned_time fragment_anchor expiry_boundary; do
 mkdir "$scratch/$variant"
 tar --exclude=.git --exclude=.task-build -cf - . | tar -xf - -C "$scratch/$variant"
 (
  cd "$scratch/$variant"
  patch -p1 < "task_support/mutations/$variant.patch" > patch.log
  set +e
  bash task_support/run.sh target > result.log 2>&1
  status=$?
  set -e
  if [[ $status -eq 0 || $status -ge 128 ]]; then cat result.log; exit 1; fi
  # driver emits FAIL only after executing actual content assertions.
  grep -q '^FAIL ' result.log || { cat result.log; exit 1; }
  if grep -Eq '(error:|Segmentation fault|Abort trap|No such file)' result.log; then cat result.log; exit 1; fi
  echo "MUTATION_DETECTED $variant status=$status"
  grep '^FAIL ' result.log
 )
done
echo JITTER_VARIANTS_COMPLETE
