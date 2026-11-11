#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s src/dynamic_any_test.cc
cmake -S userfb_baseline -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$PWD/.deps/install" -DProtobuf_PROTOC_EXECUTABLE="$PWD/.deps/install/bin/protoc"
cmake --build build --target dynamic_any_tests old_mutator_tests -j4
filter='DynamicAny.RuntimeFix:DynamicAny.MutationAndCrossover:DynamicAny.OpaqueAndReplacement:DynamicAny.CallbackOrderAndIsolation'
./build/dynamic_any_tests --gtest_list_tests --gtest_filter="$filter" > build/dynamic-any-list.txt
for name in RuntimeFix MutationAndCrossover OpaqueAndReplacement CallbackOrderAndIsolation; do
  grep -Eq "^  ${name}$" build/dynamic-any-list.txt || { echo "missing test: $name" >&2; exit 1; }
done
./build/dynamic_any_tests --gtest_filter="$filter" --gtest_output=xml:build/dynamic-any-results.xml
if grep -Eq 'skipped="[1-9]' build/dynamic-any-results.xml; then echo 'skipped target tests' >&2; exit 1; fi
grep -Eq 'tests="4"' build/dynamic-any-results.xml
./build/old_mutator_tests --gtest_filter='MutatorTypedTest/*.FailedMutations:MutatorTypedTest/*.RegisterPostProcessor:MutatorTypedTest/*.Maps:MutatorMessagesTest.NeverCopyUnknownEnum'
