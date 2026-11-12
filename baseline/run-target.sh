#!/usr/bin/env bash
#
# run-target.sh verifies the reachability work end to end:
#   1. the named contract test (and all six of its subtests) really PASS;
#   2. the pre-existing encode/decode and table module roundtrip checks still
#      pass;
#   3. the whole repository test suite stays green.
#
# It exits non-zero on the first failure.
set -euo pipefail

# Best-effort: on macOS, expose the system trust store as a PEM bundle so the
# Go toolchain's TLS verification works inside restricted sandboxes. Any
# pre-existing SSL_CERT_FILE is left untouched.
if [ -z "${SSL_CERT_FILE:-}" ] && command -v security >/dev/null 2>&1; then
	tmp_root="${TMPDIR:-/tmp}"
	mkdir -p "$tmp_root"
	pem="$tmp_root/wabin-roots.$$.pem"
	trap 'rm -f "$pem"' EXIT
	if security find-certificate -a -p \
		/System/Library/Keychains/SystemRootCertificates.keychain >"$pem" 2>/dev/null; then
		export SSL_CERT_FILE="$pem"
	fi
fi

cd "$(dirname "$0")/.."

target_test='TestReachabilityContract'
subtests=(initialization fixedpoint copy signature external malformed)

echo "==> running named target test: ${target_test}"
out="$(go test ./analysis/ -run "^${target_test}\$" -count=1 -v)"
printf '%s\n' "${out}"

for st in "${subtests[@]}"; do
	if ! printf '%s\n' "${out}" | grep -q -- "--- PASS: ${target_test}/${st} "; then
		echo "FAIL: required subtest ${target_test}/${st} did not PASS" >&2
		exit 1
	fi
done
if ! printf '%s\n' "${out}" | grep -q -- '^ok[[:space:]]'; then
	echo "FAIL: package github.com/tetratelabs/wabin/analysis did not report ok" >&2
	exit 1
fi

echo "==> running existing encode/decode and table roundtrip checks"
go test ./binary/ -count=1 -run \
	'^(TestModule_Encode|TestDecodeModule|TestDecodeModule_Errors|TestTableType|TestDecodeTableType_Errors|TestDecodeElementSegment|TestDecodeElementSegment_errors)$'

echo "==> running the full repository test suite"
go test ./... -count=1

echo "PASS: ${target_test} and all existing checks are green"
