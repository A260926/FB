#!/usr/bin/env bash
set -euo pipefail
test -s testing/audio_lacing_test.cc || { echo 'missing target: testing/audio_lacing_test.cc' >&2; exit 2; }
bash baseline/install.sh
cmake --build build-check --target audio_lacing_tests -j 2
build-check/audio_lacing_tests --gtest_list_tests --gtest_filter='AudioLacing.*' > build-check/target-list.txt
grep -Eq '^  [[:alnum:]_].*' build-check/target-list.txt || { echo 'zero selected AudioLacing tests' >&2; exit 3; }
build-check/audio_lacing_tests --gtest_filter='AudioLacing.*' --gtest_color=no | tee build-check/target.log
grep -Eq '^\[ RUN      \] AudioLacing\.' build-check/target.log
grep -Eq '^\[  PASSED  \] [1-9][0-9]* tests?\.' build-check/target.log
if grep -Eq 'DISABLED|\[  SKIPPED \]' build-check/target.log; then echo 'disabled/skipped target tests' >&2; exit 4; fi
bash baseline/run-old-tests.sh
