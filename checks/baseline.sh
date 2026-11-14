#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/ma-baseline.XXXXXX")
trap 'rm -rf "$b"' EXIT
flags=(-std=c99 -O1 -DMA_NO_DEVICE_IO -DMA_NO_DECODING -DMA_NO_ENCODING -DMA_NO_RESOURCE_MANAGER -DMA_NO_ENGINE -I.)
cc "${flags[@]}" -c task_support/miniaudio_impl.c -o "$b/miniaudio.o"
for src in checks/graph_normal.c checks/chain_normal.c; do
 cc "${flags[@]}" "$src" "$b/miniaudio.o" -lm -lpthread -o "$b/probe"
 "$b/probe"
done
