#!/bin/sh
# Windowless CPU check for the sphere UV contract.
#
# Runs the explicit `sphere_uv_contract` test target from devcheck's own Cargo project.
# The test target, not a binary, is intentional: Cargo must report the named test as
# passed. A missing target or a target that runs zero tests is a failure.
set -eu

repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
tmp_root=${TMPDIR:-/tmp}
build_dir=$(mktemp -d "$tmp_root/pathtrace-sphere-uv.XXXXXX")
log="$build_dir/cargo-test.log"
cleanup() {
    rm -rf "$build_dir"
}
trap cleanup EXIT HUP INT TERM

# Match devcheck/baseline.sh: use the local Cargo home unless the caller provides one.
: "${CARGO_HOME:=$repo_root/.userfb-cargo}"
export CARGO_HOME
CARGO_TARGET_DIR="$build_dir/target"
export CARGO_TARGET_DIR

if ! cargo test \
        --manifest-path "$repo_root/devcheck/Cargo.toml" \
        --locked \
        --test sphere_uv_contract \
        -- --exact sphere_uv_contract >"$log" 2>&1
then
    cat "$log"
    echo "SPHERE_UV_TARGET_FAILED: cargo test did not pass" >&2
    exit 1
fi

cat "$log"

# Require the exact named test to have run and passed. `cargo test --exact` exits successfully
# with zero tests when the name does not match, so checking the exit status alone is not enough.
if ! grep -F 'test sphere_uv_contract ... ok' "$log" >/dev/null; then
    echo "SPHERE_UV_TARGET_FAILED: test 'sphere_uv_contract' was not run" >&2
    exit 1
fi
if grep -F 'running 0 tests' "$log" >/dev/null; then
    echo "SPHERE_UV_TARGET_FAILED: zero tests were run" >&2
    exit 1
fi
if ! grep -F 'test result: ok. 1 passed; 0 failed' "$log" >/dev/null; then
    echo "SPHERE_UV_TARGET_FAILED: expected exactly one passing test" >&2
    exit 1
fi
if grep -F 'test sphere_uv_contract ... FAILED' "$log" >/dev/null; then
    echo "SPHERE_UV_TARGET_FAILED: test 'sphere_uv_contract' failed" >&2
    exit 1
fi

echo "sphere_uv_contract PASS"
echo "SPHERE_UV_TARGET_OK"
