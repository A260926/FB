#!/usr/bin/env bash
#
# Build and run the ENABLE_FREELIST_GUARD contract and regression checks, then rerun the original
# baseline to prove the default (guard off) build keeps its behaviour.
#
# It compiles four binaries with plain cc (no upstream Python build entry):
#   1. guard off contract control: a corrupted link word is neither detected nor aborted
#   2. guard on contract with the test only observation seams: every protected chain aborts with
#      SIGABRT after reporting FREELIST_CORRUPTION, while normal allocation/free/reuse stays correct
#   3. the full allocator test suite built with the guard and first class heaps
#   4. a short multithreaded stress build with the guard
#
# Override the compiler with CC and the stress duration with RPMALLOC_GUARD_STRESS_SECONDS.
#
# SPDX-FileCopyrightText: 2026 Mattias Jansson
# SPDX-License-Identifier: Unlicense OR MIT

set -eu

cd "$(dirname "$0")/.."
mkdir -p build-checks

compiler="${CC:-cc}"
guard_flags="-std=c11 -O1 -pthread -Wall -Wextra"
suite_flags="-std=c11 -O1 -pthread -DENABLE_OVERRIDE=0 -DENABLE_STATISTICS=1 -DENABLE_ASSERTS=1 -DRPMALLOC_FIRST_CLASS_HEAPS=1"

echo "[1/5] guard off contract control (no detection, no abort)"
# shellcheck disable=SC2086
"$compiler" $guard_flags -Irpmalloc rpmalloc/rpmalloc.c test/freelist_guard_contract.c \
	-o build-checks/guard-contract-off
./build-checks/guard-contract-off

echo "[2/5] guard on contract (SIGABRT and FREELIST_CORRUPTION for every protected chain)"
# shellcheck disable=SC2086
"$compiler" $guard_flags -DENABLE_FREELIST_GUARD=1 -DRPMALLOC_GUARD_TEST_SEAMS=1 -Irpmalloc \
	rpmalloc/rpmalloc.c test/freelist_guard_contract.c -o build-checks/guard-contract
./build-checks/guard-contract

echo "[3/5] full allocator test suite with the guard and first class heaps"
# shellcheck disable=SC2086
"$compiler" $suite_flags -DENABLE_FREELIST_GUARD=1 -Irpmalloc -Itest \
	rpmalloc/rpmalloc.c test/thread.c test/main.c -o build-checks/guard-suite
./build-checks/guard-suite

echo "[4/5] multithreaded stress with the guard"
# shellcheck disable=SC2086
"$compiler" $suite_flags -DENABLE_FREELIST_GUARD=1 -Irpmalloc -Itest \
	rpmalloc/rpmalloc.c test/thread.c test/stress.c -o build-checks/guard-stress
RPMALLOC_STRESS_SECONDS="${RPMALLOC_GUARD_STRESS_SECONDS:-5}" RPMALLOC_STRESS_THREADS=4 \
	./build-checks/guard-stress

echo "[5/5] original baseline with the default guard off build"
bash checks/baseline.sh

echo "all freelist guard checks passed"
