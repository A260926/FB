#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in
 baseline) file=task_support/baseline.mjs ;;
 target) test -s test/skin-world.mjs || { echo 'missing test/skin-world.mjs' >&2; exit 2; }; file=task_support/target-runner.mjs ;;
 *) exit 2 ;;
esac
task_skin_scratch=$(mktemp -d "${TMPDIR:-/tmp}/clay-skin.XXXXXX")
trap 'rm -rf "$task_skin_scratch"' EXIT HUP INT TERM
export XDG_CACHE_HOME="$task_skin_scratch/cache" NODE_PATH= NODE_OPTIONS= npm_config_offline=true
set +e
node --permission --allow-fs-read='*' --no-addons --no-warnings --import ./task_support/bootstrap.mjs "$file" > "$task_skin_scratch/output" 2>&1
task_skin_status=$?
set -e
cat "$task_skin_scratch/output"
test "$task_skin_status" = 0 || exit "$task_skin_status"
if [ "$mode" = target ]; then
  grep -Eq '^SKIN_WORLD_COMPLETE cases=[5-9]$|^SKIN_WORLD_COMPLETE cases=[1-9][0-9]+$' "$task_skin_scratch/output"
  grep -q '^static_visible=1 static_hits=1$' "$task_skin_scratch/output"
fi
