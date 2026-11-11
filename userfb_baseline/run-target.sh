#!/usr/bin/env bash
set -euo pipefail
test -s testing/audio_lacing_userfb_test.cc || { echo 'missing target: testing/audio_lacing_userfb_test.cc' >&2; exit 2; }
bash userfb_baseline/install.sh
cmake --build build-userfb --target userfb_audio_lacing_tests -j 2
build-userfb/userfb_audio_lacing_tests --gtest_list_tests --gtest_filter='AudioLacingUserFB.*' > build-userfb/target-list.txt
grep -Eq '^  [[:alnum:]_].*' build-userfb/target-list.txt || { echo 'zero selected AudioLacingUserFB tests' >&2; exit 3; }
build-userfb/userfb_audio_lacing_tests --gtest_filter='AudioLacingUserFB.*' --gtest_color=no | tee build-userfb/target.log
grep -Eq '^\[ RUN      \] AudioLacingUserFB\.' build-userfb/target.log
grep -Eq '^\[  PASSED  \] [1-9][0-9]* tests?\.' build-userfb/target.log
if grep -Eq 'DISABLED|\[  SKIPPED \]' build-userfb/target.log; then echo 'disabled/skipped target tests' >&2; exit 4; fi
bash userfb_baseline/run-old-tests.sh
