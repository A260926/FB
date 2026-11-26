#!/bin/sh
# Build the in-process libFuzzer entry point
# (LLVMFuzzerTestOneInput in tests/gnu_extension_contract.c).
#
# The entry reuses the same bounded chain generator and independent
# override model as the driver-based contract cases, plus a raw
# in-memory parse of the exact fuzz bytes.  No input ever touches the
# filesystem: a local memory tartype_t backs tar_fdopen().
#
# Requires a clang toolchain with libFuzzer support.  The graded
# task_support/run.sh target does not need this script.
set -eu
cd "$(dirname "$0")/.."

export CHECK_SANITIZERS="-fsanitize=address,undefined -fno-omit-frame-pointer"
sh task_support/build.sh

cc -std=gnu99 -O1 -g \
	-fsanitize=fuzzer,address,undefined \
	-fno-omit-frame-pointer \
	-DGNU_FUZZER_BUILD \
	-I. -Itask_support -Ilib -I.task-build \
	tests/gnu_extension_contract.c .task-build/libtar.a \
	-o .task-build/fuzz_gnu_extension

echo "built .task-build/fuzz_gnu_extension"
echo "run: .task-build/fuzz_gnu_extension [-max_total_time=60] [corpus_dir]"
