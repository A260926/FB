#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -r "$build"' EXIT
flags='-std=c99 -O1 -DMA_NO_DEVICE_IO -DMA_NO_DECODING -DMA_NO_ENCODING -DMA_NO_RESOURCE_MANAGER -DMA_NO_ENGINE'
cc $flags checks/repro.c -lpthread -lm -o "$build/single"
"$build/single" --normal
"$build/single" --observe
cc $flags -DCHECK_SPLIT checks/repro.c extras/miniaudio_split/miniaudio.c -lpthread -lm -o "$build/split"
"$build/split" --normal
"$build/split" --observe
