#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -std=c99 -O2 -Ichecks -Ilibtwolame simplefrontend/simplefrontend.c simplefrontend/audio_wave.c .task-build/libtwolame.a -lm -o .task-build/stwolame
.task-build/stwolame tests/testcase-44100.wav .task-build/legacy.mp2 > .task-build/legacy.log 2>&1
read -r checksum bytes path < <(cksum .task-build/legacy.mp2)
[[ $checksum == 3026851135 && $bytes == 13772 ]]
printf 'TWOLAME_LEGACY_OK bytes=%s checksum=%s\n' "$bytes" "$checksum"
