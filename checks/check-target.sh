#!/bin/sh
set -eu
test -s tests/node_graph/fanout_contract.c || { echo 'missing target: tests/node_graph/fanout_contract.c' >&2; exit 1; }
build=$(mktemp -d)
trap 'rm -r "$build"' EXIT
flags='-std=c99 -O1 -DMA_NO_DEVICE_IO -DMA_NO_DECODING -DMA_NO_ENCODING -DMA_NO_RESOURCE_MANAGER -DMA_NO_ENGINE'
cc $flags checks/repro.c -lpthread -lm -o "$build/repro"
"$build/repro" --normal
"$build/repro" --check
cc $flags -I. tests/node_graph/fanout_contract.c -lpthread -lm -o "$build/single"
cc $flags -DCHECK_SPLIT -Iextras/miniaudio_split tests/node_graph/fanout_contract.c extras/miniaudio_split/miniaudio.c -lpthread -lm -o "$build/split"
for exe in single split; do
 "$build/$exe" > "$build/$exe.log" 2>&1 || { cat "$build/$exe.log"; exit 1; }
 cat "$build/$exe.log"
 for marker in FANOUT_WINDOWS_OK FANOUT_ORDER_OK FANOUT_PROCESSING_CACHE_OK FANOUT_REATTACH_OK; do grep -Fx "$marker" "$build/$exe.log" >/dev/null; done
done
