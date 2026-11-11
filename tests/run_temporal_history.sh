#!/usr/bin/env bash
#
# Temporal history regression tests for normaliz0r (C ABI) and delay0r
# (frei0r.hpp C++ wrapper ABI).  Each plugin is built together with its
# independent reference model and driven solely through the public f0r_*
# entry points; no production instance state is inspected by the tests.
#
# On success the last line is: frei0r temporal history: ok
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/.tmp/temporal-history"
mkdir -p "$build"
trap 'rm -rf "$build"; rmdir "$root/.tmp" 2>/dev/null || true' EXIT

cc -std=c99 -O2 -Wall -Wextra -I"$root/include" \
  "$root/tests/normaliz_history.c" \
  "$root/tests/normaliz_ref.c" \
  "$root/src/filter/normaliz0r/normaliz0r.c" \
  -lm -o "$build/normaliz_history"

c++ -std=c++11 -O2 -Wall -Wextra -I"$root/include" \
  "$root/tests/delay_history.cpp" \
  "$root/tests/delay_ref.cpp" \
  "$root/src/filter/delay0r/delay0r.cpp" \
  -o "$build/delay_history"

"$build/normaliz_history"
"$build/delay_history"

echo "frei0r temporal history: ok"
