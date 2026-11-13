#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
 baseline) file=task_support/baseline.mjs ;;
 target) test -s test/scene/local-affine-target.mjs || { echo 'missing local-affine-target.mjs' >&2; exit 2; }; file=task_support/target-runner.mjs ;;
 *) exit 2 ;;
esac
scratch=$(mktemp -d "${TMPDIR:-/tmp}/pc-affine.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
export HOME="$scratch" XDG_CACHE_HOME="$scratch/cache" NODE_PATH= npm_config_offline=true
node "$file"
