#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/sqli.XXXXXX")
trap 'rm -rf "$b"' EXIT
src=(src/libinjection_sqli.c src/libinjection_xss.c src/libinjection_html5.c)
cc -std=c99 -O1 -Isrc baseline/smoke.c "${src[@]}" -o "$b/smoke"
"$b/smoke"
cc -std=c99 -O1 -Isrc src/testdriver.c "${src[@]}" -o "$b/old"
"$b/old" tests/test-tokens-comments-cstylenested-001.txt tests/test-tokens-string-008.txt tests/test-sqli-049.txt
printf 'BASELINE_OLD_3_OK\n'
