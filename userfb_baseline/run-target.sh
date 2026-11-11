#!/bin/sh
set -eu
. ./userfb_baseline/env.sh
for file in prog/hints_paging_userfb_test.go pkg/fuzzer/hints_paging_userfb_test.go docs/hints-paging.md; do
    if ! test -s "$file"; then
        echo "missing or empty target file: $file" >&2
        exit 2
    fi
done
go run userfb_baseline/testguard.go ./prog TestHintsPagingUserFB
go run userfb_baseline/testguard.go ./pkg/fuzzer TestHintsJobPagingUserFB
sh userfb_baseline/old-tests.sh
