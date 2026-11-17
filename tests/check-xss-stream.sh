#!/usr/bin/env bash
# Incremental XSS stream contract tests.
#
# Builds and runs tests/xss_stream_contract.c entirely in memory with
# the system C compiler: no browser, no network, no extra packages.
# The three OK markers (POLICY/BOUNDARY/SPACE) are printed by the C
# program itself.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/.baseline"
mkdir -p "$work"

srcs="$root/src/libinjection_xss.c $root/src/libinjection_xss_stream.c $root/src/libinjection_html5.c"

# the contract test: lifecycle/error contract, every split boundary
# against the one-shot API, and 16 MiB bounded-memory/single-pass work
${CC:-cc} -std=c99 -O2 -Wall -Wextra -I"$root/src" \
    "$root/tests/xss_stream_contract.c" $srcs -o "$work/xss_stream_contract"
"$work/xss_stream_contract"

# the documented calling example must build and run cleanly as well
${CC:-cc} -std=c99 -O2 -Wall -Wextra -I"$root/src" \
    "$root/examples/xss_stream_example.c" $srcs -o "$work/xss_stream_example"
"$work/xss_stream_example"
