#!/usr/bin/env bash
#
# Free list guard contract checks.
#
# Builds rpmalloc with ENABLE_FREELIST_GUARD and runs test/freelist_guard_contract.c, which exercises
# normal allocator behaviour and, in forked children, corrupts the chain word of freed blocks on the
# page local, page remote, heap remote and thread batch free lists, requiring a FREELIST_CORRUPTION
# diagnostic and SIGABRT. A guard-off control build proves the default build keeps the original
# behaviour. All builds use the C11 toolchain and pthread directly, never the upstream Python entry.
#
# SPDX-FileCopyrightText: 2026 Mattias Jansson
# SPDX-License-Identifier: Unlicense OR MIT

set -eu

cd "$(dirname "$0")/.."
mkdir -p build-checks

CC_BIN="${CC:-cc}"
WARN="-std=c11 -O1 -Wall -Wextra -pthread"
INCLUDES="-Irpmalloc -Itest"

echo "== guard enabled (first class heaps on) =="
"$CC_BIN" $WARN $INCLUDES \
	-DRPMALLOC_FIRST_CLASS_HEAPS=1 -DENABLE_FREELIST_GUARD=1 -DRPMALLOC_TEST_SEAMS \
	rpmalloc/rpmalloc.c test/freelist_guard_contract.c -o build-checks/guard-contract
./build-checks/guard-contract

echo "== guard enabled (first class heaps off) =="
"$CC_BIN" $WARN $INCLUDES \
	-DRPMALLOC_FIRST_CLASS_HEAPS=0 -DENABLE_FREELIST_GUARD=1 -DRPMALLOC_TEST_SEAMS \
	rpmalloc/rpmalloc.c test/freelist_guard_contract.c -o build-checks/guard-contract-no-fch
./build-checks/guard-contract-no-fch > /dev/null

echo "== guard disabled control (default behaviour) =="
"$CC_BIN" $WARN $INCLUDES \
	-DRPMALLOC_FIRST_CLASS_HEAPS=1 -DRPMALLOC_TEST_SEAMS \
	rpmalloc/rpmalloc.c test/freelist_guard_contract.c -o build-checks/guard-contract-off
./build-checks/guard-contract-off

echo "== guard enabled, asserts disabled (must still abort, not an assert) =="
"$CC_BIN" $WARN $INCLUDES \
	-DRPMALLOC_FIRST_CLASS_HEAPS=1 -DENABLE_FREELIST_GUARD=1 -DENABLE_ASSERTS=0 -DRPMALLOC_TEST_SEAMS \
	rpmalloc/rpmalloc.c test/freelist_guard_contract.c -o build-checks/guard-contract-noassert
./build-checks/guard-contract-noassert > /dev/null

echo "== shadow mapping failure fails the allocation cleanly =="
"$CC_BIN" $WARN $INCLUDES \
	-DRPMALLOC_FIRST_CLASS_HEAPS=1 -DENABLE_FREELIST_GUARD=1 -DRPMALLOC_TEST_SEAMS \
	rpmalloc/rpmalloc.c test/freelist_guard_mapfail.c -o build-checks/guard-mapfail
./build-checks/guard-mapfail

echo "guard checks passed"
