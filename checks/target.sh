#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -I include -I checks -DZEPHYR_CONFIG_OVERRIDE=ffconf_override.h ff.c option/ffunicode.c checks/memdisk.c checks/recovery_main.c checks/exfat_recovery_test.c -o "$build/recovery"
for group in sets allocation reads isolation; do
 "$build/recovery" "$group" | tee "$build/$group.log"
 grep -Eq "^EXFAT_RECOVERY_GROUP $group assertions=[1-9][0-9]*$" "$build/$group.log"
 grep -Fx EXFAT_RECOVERY_GROUP_COMPLETE "$build/$group.log"
done
bash checks/baseline.sh
