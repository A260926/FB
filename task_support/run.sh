#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode="${1:-baseline}"
case "$mode" in baseline|target) ;; *) exit 2 ;; esac
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/basis-texture.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
compiler="${CXX:-c++}"
flags=(-std=c++17 -O1 -fno-strict-aliasing -DBASISD_SUPPORT_KTX2=0 -DBASISD_SUPPORT_KTX2_ZSTD=0 -Itranscoder)
if [ "$mode" = target ]; then
    test -s transcoder/basisu_texture_job.h
    test -s transcoder/basisu_texture_job.cpp
    "$compiler" "${flags[@]}" -include transcoder/basisu_texture_job.h \
        transcoder/basisu_transcoder.cpp transcoder/basisu_texture_job.cpp \
        task_support/baseline.cpp -o "$build_dir/baseline"
else
    "$compiler" "${flags[@]}" transcoder/basisu_transcoder.cpp \
        task_support/baseline.cpp -o "$build_dir/baseline"
fi
"$build_dir/baseline" assets/upstream-video.basis
"$build_dir/baseline" assets/alpha-video.basis
if [ "$mode" = target ]; then
    echo 'TARGET_BUILD_ONLY new_api_behavior_not_executed'
fi
