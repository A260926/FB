#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
sources=()
for name in fdt.c fdt_ro.c fdt_wip.c fdt_sw.c fdt_rw.c fdt_strerror.c fdt_empty_tree.c fdt_addresses.c fdt_overlay.c fdt_check.c; do sources+=("libfdt/$name"); done
case "${1:-target}" in
baseline)
 cc -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Ilibfdt "${sources[@]}" tools/baseline.c -o "$work/base"
 "$work/base"
 ;;
target)
 test -s libfdt/fdt_mmio.c && test -s libfdt/fdt_mmio.h && test -s tests/mmio_cases.c
 cc -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Ilibfdt -Itools "${sources[@]}" libfdt/fdt_mmio.c tests/mmio_cases.c tools/mmio_driver.c -o "$work/mmio"
 "$work/mmio"
 bash tools/run-mmio.sh baseline
 ;;
*) exit 2;;
esac
