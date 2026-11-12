#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .check-build
./testx509 > .check-build/old-x509.log 2>&1 || { cat .check-build/old-x509.log; exit 1; }
cat .check-build/old-x509.log
grep -q '^Name extraction: OK$' .check-build/old-x509.log
grep -q '^Maximum return stack usage:' .check-build/old-x509.log
count=$(grep -c ': OK$' .check-build/old-x509.log)
test "$count" -gt 0
printf 'BEARSSL_OLD_X509_OK cases=%s\n' "$count"
