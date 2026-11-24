#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $(uname -s) == Linux ]]; then
  exec bash task_support/run.sh "$@"
fi
if [[ $(uname -s) != Darwin ]]; then
  echo 'This source requires Linux; see task_support/LINUX_TOOLCHAIN.md.' >&2
  exit 2
fi
command -v docker >/dev/null || { echo 'Linux toolchain required; see task_support/LINUX_TOOLCHAIN.md.' >&2; exit 2; }
image=b99b86a28812b1e6453a231a947dc43d76fe192788a12f344a9b568bf9f5d24c
actual=$(docker image inspect --format '{{.Id}}' "$image")
[[ $actual == sha256:$image ]] || { echo 'Unexpected Linux SDK identity' >&2; exit 2; }
exec docker run --rm --pull=never --network=none   --mount "type=bind,source=$PWD,target=/work" --workdir /work   --env CXX=g++ --env CC=gcc --env CCACHE_DISABLE=1   "$image" bash task_support/run.sh "$@"
