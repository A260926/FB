#!/usr/bin/env bash
#
# Build and run the PostgreSQL-mode C tests.
#
# Fails non-zero (under set -e) if any required source file is missing,
# the compile fails, or the test binary reports a failed check or ran no
# checks.  The test binary itself enforces the "zero cases" rule.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
test_c="$root/tests/pg_mode.c"
src=(
    "$root/src/libinjection_sqli.c"
    "$root/src/libinjection_xss.c"
    "$root/src/libinjection_html5.c"
)

for f in "$test_c" "${src[@]}" "$root/src/libinjection.h" \
         "$root/src/libinjection_sqli.h"; do
    if [[ ! -f "$f" ]]; then
        echo "missing required file: $f" >&2
        exit 2
    fi
done

b=$(mktemp -d "${TMPDIR:-/tmp}/pgmode.XXXXXX")
trap 'rm -rf "$b"' EXIT

cc -std=c99 -O1 -Wall -Wextra -I"$root/src" "$test_c" "${src[@]}" \
    -o "$b/pg_mode"

"$b/pg_mode"
