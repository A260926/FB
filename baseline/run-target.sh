#!/usr/bin/env bash
set -euo pipefail
export GEM_HOME="${TMPDIR:-/tmp}/userfb-z1227-gems" GEM_PATH="${TMPDIR:-/tmp}/userfb-z1227-gems"; bash -e -o pipefail -c 'test -f test/tests/render_context_userfb.rb; t=$(mktemp); ruby -Ilib test/tests/render_context_userfb.rb -v | tee "$t"; grep -q "test_locals_order" "$t"; grep -q "test_nested_context" "$t"; grep -Eq "[1-9][0-9]* runs, .*0 failures, 0 errors, 0 skips" "$t"; ruby -Ilib test/tests/render_path.rb'
