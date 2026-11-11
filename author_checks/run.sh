#!/usr/bin/env bash
set -euo pipefail
file=$1
test -s "$file"
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DBUILD_SHARED=OFF -DBUILD_STATIC=ON -DBUILD_TESTING=OFF -DLIBXMP_DISABLE_DEPACKERS=ON -DLIBXMP_DISABLE_PROWIZARD=ON
cmake --build "$b" --parallel 4
cc -std=c99 -Iinclude "$file" "$b/libxmp.a" -lm -o "$b/check"
"$b/check"
printf 'declare_test(test_mixer_note_cut_ac);\n' > "$b/all_tests.c"

cc -std=c99 -Iinclude -Isrc -I"$b" author_checks/old_anticlick.c "$b/libxmp.a" -lm -o "$b/oldcheck"
(cd test-dev && "$b/oldcheck")
