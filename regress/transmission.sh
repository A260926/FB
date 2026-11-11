#!/bin/sh
set -eu
b=$(mktemp -d "${TMPDIR:-/tmp}/rpt-transmission.XXXXXX")
trap 'rm -rf "$b"' EXIT HUP INT TERM
log="$b/output.log"
status=0
CARGO_HOME="$PWD/.userfb-cargo" CARGO_TARGET_DIR="$b/target" \
    cargo run --manifest-path regress/Cargo.toml --locked --quiet \
    --bin transmission -- "$b/transmission.png" >"$log" 2>&1 || status=$?
cat "$log"
[ "$status" -eq 0 ] || exit "$status"
grep -q "TRANSMISSION_TARGET_OK" "$log" || {
    echo "missing TRANSMISSION_TARGET_OK" >&2
    exit 1
}
[ -s "$b/transmission.png" ] || {
    echo "missing rendered PNG" >&2
    exit 1
}
