#!/bin/sh
set -eu
. ./baseline/env.sh
for file in prog/hints_paging_test.go pkg/fuzzer/hints_paging_test.go docs/hints-paging.md; do
    if ! test -s "$file"; then
        echo "missing or empty target file: $file" >&2
        exit 2
    fi
done
go run baseline/testguard.go ./prog TestHintsPaging
go run baseline/testguard.go ./pkg/fuzzer TestHintsJobPaging
sh baseline/old-tests.sh
