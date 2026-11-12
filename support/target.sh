#!/bin/bash
set -euo pipefail
b=${CHECK_BUILD_DIR:-$(mktemp -d)}
bash support/compile.sh "$b"
lib=$(find "$b" -name libxlsxwriter.a -type f)
cc -O1 -Iinclude -Isupport -Isupport/zlib -Isupport/zipreader support/target_main.c tests/disk_sst.c support/zipreader/{unzip,ioapi}.c "$lib" "$b/libz.a" -lm -o "$b/target"
(cd "$b"; ./target) | tee "$b/target.log"
for group in identity spill errors; do
 grep -Eq "^SST_GROUP $group PASS cases=[1-9][0-9]* checks=[1-9][0-9]*$" "$b/target.log"
done
CHECK_BUILD_DIR="$b" bash support/baseline.sh
