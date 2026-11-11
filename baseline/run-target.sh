#!/usr/bin/env bash
set -euo pipefail
test -s testing/audio_lacing_test.cc || { echo 'missing target: testing/audio_lacing_test.cc' >&2; exit 2; }
bash baseline/install.sh
cmake --build build-userfb --target audio_lacing_tests -j 2
build-userfb/audio_lacing_tests --gtest_list_tests --gtest_filter='AudioLacing.*' > build-userfb/target-list.txt
grep -Eq '^  [[:alnum:]_].*' build-userfb/target-list.txt || { echo 'zero selected AudioLacing tests' >&2; exit 3; }
build-userfb/audio_lacing_tests --gtest_filter='AudioLacing.*' --gtest_color=no | tee build-userfb/target.log
grep -Eq '^\[ RUN      \] AudioLacing\.' build-userfb/target.log
grep -Eq '^\[  PASSED  \] [1-9][0-9]* tests?\.' build-userfb/target.log
if grep -Eq 'DISABLED|\[  SKIPPED \]' build-userfb/target.log; then echo 'disabled/skipped target tests' >&2; exit 4; fi
bash baseline/run-old-tests.sh
