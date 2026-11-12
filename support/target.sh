#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
cc -O1 -pthread -Ilibraries/liblmdb -Isupport support/target_main.c tests/range_delete.c libraries/liblmdb/{mdb,midl,module}.c -o "$b/target"
(cd "$b" && ./target) | tee "$b/target.log"
for group in bounds pages cursors; do
 grep -Eq "^RANGE_GROUP $group PASS cases=[1-9][0-9]* checks=[1-9][0-9]*$" "$b/target.log"
done
bash support/baseline.sh
