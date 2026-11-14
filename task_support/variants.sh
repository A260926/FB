#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/variants
export TASK_GOCACHE="${TASK_GOCACHE:-$PWD/.task-build/go-cache}"
bash task_support/run.sh target > .task-build/variants/original.log 2>&1
echo ORIGINAL_CONTENT_OK
variant_root=$(mktemp -d)
trap 'rm -rf "$variant_root"' EXIT
# Source copies include future tests and vendored dependencies, never build caches.
for variant in forget_delta freeze_clock ack_jump; do
  dest="$variant_root/$variant"
  mkdir -p "$dest"
  tar --exclude=.git --exclude=.task-build -cf - . | tar -xf - -C "$dest"
  file=pkg/rawmessage/reader.go
  if [[ $variant == forget_delta ]]; then file=pkg/rawmessage/writer.go; fi
  awk -v variant="$variant" '
    {line=$0}
    variant=="forget_delta" && $0 ~ /wc.lastTimestampDelta = timestampDelta/ {sub(/= timestampDelta/,"= nil",line);n++}
    variant=="freeze_clock" && $0 ~ /rc.curTimestamp \+= rc.curTimestampDelta/ {sub(/rc.curTimestampDelta/,"0",line);n++}
    variant=="ack_jump" && $0 ~ /rc.mr.LastAckCount \+= rc.mr.ackWindowSize/ {sub(/\+= rc.mr.ackWindowSize/,"= count",line);n++}
    {print line}
    END {if(n!=1)exit 9}
  ' "$file" > "$dest/$file"
  (cd "$dest"; bash task_support/run.sh baseline) > ".task-build/variants/$variant-build.log" 2>&1 || {
    # Old tests may reject a semantic mutant; compilation is checked separately below.
    true
  }
  (cd "$dest"; export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOCACHE="${TASK_GOCACHE:-$PWD/.task-build/go-cache}" GOMODCACHE="$PWD/.task-build/go-modcache";
    go test -mod=vendor -vet=off -run '^$' ./pkg/rawmessage) > ".task-build/variants/$variant-compile.log" 2>&1
  set +e
  (cd "$dest"; bash task_support/run.sh target) > ".task-build/variants/$variant.log" 2>&1
  status=$?
  set -e
  [[ $status == 1 ]]
  grep -Eq '^--- FAIL: TestClockWire(Writer|Reader|Ack)( |/)' ".task-build/variants/$variant.log"
  grep -Eq 'clock_wire_test.go:[0-9]+:' ".task-build/variants/$variant.log"
  if grep -Eq 'panic:|fatal error:|\[build failed\]|task group executed no content checks|task body did not return normally' ".task-build/variants/$variant.log"; then exit 5; fi
  echo "MUTANT_CONTENT_REJECTED $variant"
done
echo RTMP_VARIANTS_COMPLETE
