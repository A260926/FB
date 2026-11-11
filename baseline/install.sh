#!/usr/bin/env bash
set -euo pipefail
rev=58d77fa8070e8cec2dc1ed015d66b454c8d78850
deps=.deps/googletest
if [[ ! -d "$deps/.git" ]]; then
  mkdir -p "$deps"
  git -C "$deps" init -q
  git -C "$deps" -c core.hooksPath=/dev/null fetch --depth 1 --no-tags https://github.com/google/googletest.git "$rev"
  git -C "$deps" -c core.hooksPath=/dev/null checkout -q --detach FETCH_HEAD
fi
[[ "$(git -C "$deps" rev-parse HEAD)" == "$rev" ]]
git -C "$deps" diff --quiet HEAD --
cmake -S . -B build-check -DENABLE_TESTS=ON -DLIBWEBM_DISABLE_GTEST_CMAKE=ON -DGTEST_SRC_DIR="$PWD/$deps" -DENABLE_WEBMTS=OFF -DENABLE_WEBMINFO=OFF -DENABLE_SAMPLE_PROGRAMS=OFF -DENABLE_IWYU=OFF -DENABLE_WEBM_PARSER=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-check --target lacing_baseline mkvmuxer_tests mkvparser_tests -j 2
printf 'DEPENDENCY_LOCK googletest=%s\n' "$rev"
