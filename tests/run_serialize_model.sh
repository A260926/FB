#!/usr/bin/env bash
#
# Structured serialization model test entry point.
#
# For each of four configurations (narrow / PUGIXML_WCHAR_MODE, each with the
# default output stack and PUGIXML_MEMORY_OUTPUT_STACK=128) this builds the
# driver under ASan/UBSan and runs:
#
#   1. every fixed alignment family individually (at least six are required),
#   2. the deterministic sweep exactly as `--seed 1319 --cases 120`,
#
# reporting the actual number of content executions per config and per output
# encoding.  It also builds and executes the exported fuzz entry point, then
# applies three reviewed source mutants to temporary copies of src/ and
# requires each to compile, to reach a named content assertion, and to be
# caught by the same case suite with concrete differing bytes.
#
# No Python, no external XML library, and no modifications to the working
# source tree.

set -euo pipefail

cd "$(dirname "$0")/.."

repo_root=$(pwd)
src_dir="$repo_root/src"
tests_dir="$repo_root/tests"
mutants_dir="$tests_dir/serialize_mutants"

fail()
{
	echo "SERIALIZE_MODEL_FUZZ_FAILED: $*" >&2
	exit 1
}

require_file()
{
	if [[ ! -f "$1" ]]; then
		fail "missing required file: $1"
	fi
}

require_file "$src_dir/pugixml.cpp"
require_file "$src_dir/pugixml.hpp"
require_file "$tests_dir/serialize_model_core.hpp"
require_file "$tests_dir/fuzz_serialize_model.cpp"
require_file "$tests_dir/serialize_model_main.cpp"
require_file "$tests_dir/run_serialize_model.sh"

# Record the pristine source so the end-to-end run can prove that mutants were
# applied only to temporary copies.
source_checksum_before=$(cksum "$src_dir/pugixml.cpp")

tmp_root=${TMPDIR:-${TMP:-/tmp}}
work="$tmp_root/serialize_model.$$.$RANDOM"
(umask 077 && mkdir "$work") || fail "cannot create work directory under $tmp_root"
trap 'rm -rf "$work"' EXIT

compiler=${CXX:-clang++}
sanitizer_flags=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
common_flags=(-std=c++17 -O1 -g "${sanitizer_flags[@]}")

configs=()
configs+=("narrow:default:")
configs+=("narrow:small:-DPUGIXML_MEMORY_OUTPUT_STACK=128")
configs+=("wide:default:-DPUGIXML_WCHAR_MODE")
configs+=("wide:small:-DPUGIXML_WCHAR_MODE -DPUGIXML_MEMORY_OUTPUT_STACK=128")

encodings=(utf8 utf16_le utf16_be utf32_le utf32_be)

total_cases=0
total_fixed=0
fixed_family_minimum=6

build_driver()
{
	local name="$1"
	local defs="$2"
	local out="$3"

	# shellcheck disable=SC2086
	if ! "$compiler" "${common_flags[@]}" $defs -I"$src_dir" \
		"$src_dir/pugixml.cpp" "$tests_dir/serialize_model_main.cpp" \
		-o "$out" > "$work/build-$name.log" 2>&1; then
		cat "$work/build-$name.log" >&2
		fail "driver build failed for $name"
	fi

	[[ -x "$out" ]] || fail "driver was not produced for $name"
}

run_config()
{
	local name="$1"
	local chars="$2"
	local defs="$3"
	local stack="$4"

	local driver="$work/driver-$name"
	build_driver "$name" "$defs" "$driver"

	local log="$work/run-$name.log"

	echo "=== config $name chars=$chars output_stack=$stack ==="

	# Fixed alignment families run first and must not be empty.
	local fixed_count
	fixed_count=$("$driver" --dump-fixed-count)
	if ! [[ "$fixed_count" =~ ^[0-9]+$ ]] || [[ "$fixed_count" -lt "$fixed_family_minimum" ]]; then
		fail "config $name exposes only $fixed_count fixed families (need at least $fixed_family_minimum)"
	fi

	# The fixed suite as a whole must cover every required content feature.
	# This is a model-level invariant, independent of the byte comparison.
	if ! "$driver" --check-fixed-invariants > "$work/invariants-$name.log" 2>&1; then
		cat "$work/invariants-$name.log" >&2
		fail "fixed-suite model invariants failed for $name"
	fi

	echo "fixed-suite invariants PASS ($name)"

	for family in $(seq 0 $((fixed_count - 1))); do
		local family_log="$work/family-$name-$family.log"

		if ! "$driver" --family "$family" > "$family_log" 2>&1; then
			cat "$family_log" >&2
			fail "fixed alignment family $family failed for $name"
		fi

		local fam_name
		fam_name=$(sed -n 's/^fixed family=//p' "$family_log" | head -n 1)
		[[ -n "$fam_name" ]] || fail "fixed family $family produced no name for $name"

		echo "fixed-family $fam_name PASS ($name)"
	done

	# The required deterministic sweep, run exactly as specified.
	if ! "$driver" --seed 1319 --cases 120 > "$log" 2>&1; then
		cat "$log" >&2
		fail "seed sweep failed for $name"
	fi

	local cases_run fixed_run
	cases_run=$(sed -n 's/^cases=\([0-9][0-9]*\) fixed=.*/\1/p' "$log" | tail -n 1)
	fixed_run=$(sed -n 's/^cases=[0-9][0-9]* fixed=\([0-9][0-9]*\)$/\1/p' "$log" | tail -n 1)

	if [[ -z "$cases_run" || "$cases_run" -eq 0 ]]; then
		cat "$log" >&2
		fail "zero executed cases for $name"
	fi

	if [[ -z "$fixed_run" || "$fixed_run" -lt "$fixed_family_minimum" ]]; then
		cat "$log" >&2
		fail "the sweep for $name executed $fixed_run fixed families"
	fi

	# The sweep invocation must contribute exactly the 120 seeded cases.
	if [[ "$cases_run" -ne 120 ]]; then
		cat "$log" >&2
		fail "seed sweep for $name executed $cases_run cases instead of 120"
	fi

	echo "config $name seeded_cases=$cases_run fixed_cases=$fixed_run"

	local encoding_total=0

	for enc in "${encodings[@]}"; do
		local count
		count=$(sed -n "s/^executions encoding=$enc count=//p" "$log" | tail -n 1)
		if [[ -z "$count" || "$count" -eq 0 ]]; then
			cat "$log" >&2
			fail "zero executions for encoding $enc in $name"
		fi

		echo "config $name encoding=$enc executions=$count"
		encoding_total=$((encoding_total + count))
	done

	if [[ "$encoding_total" -eq 0 ]]; then
		fail "all encodings skipped for $name"
	fi

	total_cases=$((total_cases + cases_run))
	total_fixed=$((total_fixed + fixed_run))
}

for entry in "${configs[@]}"; do
	IFS=: read -r mode stack_mode defs <<< "$entry"
	if [[ "$mode" == wide ]]; then chars=wchar_t; else chars=char; fi
	if [[ "$stack_mode" == small ]]; then stack=128; else stack=default; fi
	run_config "$mode-$stack_mode" "$chars" "$defs" "$stack"
done

if [[ "$total_cases" -eq 0 ]]; then
	fail "no cases executed across all configurations"
fi

echo "total seeded cases across configurations: $total_cases"
echo "total fixed cases across configurations: $total_fixed"

# ---------- fuzz entry point -----------------------------------------------

# Build the exported entry point.  The standalone corpus main is used only
# because this toolchain does not ship the libFuzzer runtime; the entry point
# itself and the shared checking core are identical either way.
build_fuzz()
{
	local name="$1"
	local defs="$2"
	local out="$3"

	# shellcheck disable=SC2086
	if ! "$compiler" "${common_flags[@]}" -DSERIALIZE_MODEL_STANDALONE_FUZZ_MAIN $defs \
		-I"$src_dir" "$src_dir/pugixml.cpp" "$tests_dir/fuzz_serialize_model.cpp" \
		-o "$out" > "$work/fuzz-build-$name.log" 2>&1; then
		cat "$work/fuzz-build-$name.log" >&2
		fail "fuzz entry build failed for $name"
	fi

	[[ -x "$out" ]] || fail "fuzz entry was not produced for $name"
}

run_fuzz_entry()
{
	local name="$1"
	local defs="$2"
	local binary="$work/fuzz-$name"

	build_fuzz "$name" "$defs" "$binary"

	local corpus="$work/corpus-$name"
	mkdir -p "$corpus"

	# One corpus file per fixed family plus a few arbitrary byte inputs.
	local fixed_count
	fixed_count=$("$work/driver-$name" --dump-fixed-count)

	for family in $(seq 0 $((fixed_count - 1))); do
		local file="$corpus/family-$family.bin"
		printf 'SMF%s' "$(printf "\\$(printf '%03o' "$family")")" > "$file"
	done

	printf '' > "$corpus/empty.bin"
	printf 'arbitrary-bytes-0123456789' > "$corpus/ascii.bin"
	printf '\xff\xfe\x00\x80\x7f\xf0\x9f\x99\x82' > "$corpus/bytes.bin"

	local out="$work/fuzz-run-$name.log"

	if ! "$binary" "$corpus"/*.bin > "$out" 2>&1; then
		cat "$out" >&2
		fail "fuzz entry failed for $name"
	fi

	local executed
	executed=$(sed -n 's/^fuzz_inputs=//p' "$out" | tail -n 1)
	if [[ -z "$executed" || "$executed" -eq 0 ]]; then
		cat "$out" >&2
		fail "fuzz entry executed zero inputs for $name"
	fi

	echo "fuzz-entry $name inputs=$executed PASS"
}

run_fuzz_entry "narrow-default" ""
run_fuzz_entry "wide-default" "-DPUGIXML_WCHAR_MODE"

# ---------- mutant verification -------------------------------------------

apply_mutant()
{
	local patch_name="$1"
	local tree="$2"

	rm -rf "$tree"
	mkdir -p "$tree/src"
	cp "$src_dir/pugixml.cpp" "$tree/src/pugixml.cpp"
	cp "$src_dir/pugixml.hpp" "$tree/src/pugixml.hpp"

	if ! ( cd "$tree" && patch -p1 --forward < "$mutants_dir/$patch_name" > patch.log 2>&1 ); then
		cat "$tree/patch.log" >&2
		fail "mutant $patch_name did not apply to the temporary source copy"
	fi

	if cmp -s "$src_dir/pugixml.cpp" "$tree/src/pugixml.cpp"; then
		fail "mutant $patch_name is a no-op"
	fi
}

verify_mutant()
{
	local patch_name="$1"
	local expected_label="$2"

	local patch="$mutants_dir/$patch_name"
	local tree="$work/mutant-$(basename "$patch_name" .patch)"

	require_file "$patch"
	apply_mutant "$patch_name" "$tree"

	local mutant_driver="$tree/driver"

	# Must compile.  A compiler error is not a detection.
	if ! "$compiler" "${common_flags[@]}" -I"$tree/src" -I"$src_dir" \
		"$tree/src/pugixml.cpp" "$tests_dir/serialize_model_main.cpp" \
		-o "$mutant_driver" > "$tree/build.log" 2>&1; then
		cat "$tree/build.log" >&2
		fail "mutant $patch_name failed to compile (compiler error does not count as detection)"
	fi

	[[ -x "$mutant_driver" ]] || fail "mutant $patch_name produced no driver"

	local out="$tree/run.log"
	local rc=0

	# The same case suite as the normal run: fixed families plus --seed/--cases.
	if "$mutant_driver" --seed 1319 --cases 120 > "$out" 2>&1; then
		rc=0
	else
		rc=$?
	fi

	# A startup failure or sanitizer runtime error is not a content
	# detection.  Our content failure exits with status 1 and a named
	# assertion; anything else (abort, signal, bad arguments) is rejected.
	if [[ "$rc" -ne 1 ]]; then
		cat "$out" >&2
		fail "mutant $patch_name did not fail through a content assertion (rc=$rc)"
	fi

	if ! grep -q '^CONTENT_ASSERTION_FAILED: content\.' "$out"; then
		cat "$out" >&2
		fail "mutant $patch_name did not report a named content assertion"
	fi

	if ! grep -q '^  first_diff_byte=' "$out" || ! grep -q '^  expected_byte=' "$out" || ! grep -q '^  scalar_index=' "$out"; then
		cat "$out" >&2
		fail "mutant $patch_name did not report concrete differing bytes and a scalar position"
	fi

	local assertion
	assertion=$(sed -n 's/^CONTENT_ASSERTION_FAILED: //p' "$out" | head -n 1)

	echo "mutant $patch_name DETECTED ($expected_label) by $assertion"

	# The same detection must reproduce on a second run.
	if "$mutant_driver" --seed 1319 --cases 120 > "$out.second" 2>&1; then
		fail "mutant $patch_name passed on the second run"
	fi

	local key_first="$tree/key.first"
	local key_second="$tree/key.second"

	# Avoid process substitution: some sandboxed shells cannot open /dev/fd.
	sed -n -e '/^CONTENT_ASSERTION_FAILED/p' -e '/^  first_diff_byte=/p' \
		-e '/^  expected_byte=/p' -e '/^  scalar_index=/p' "$out" > "$key_first"
	sed -n -e '/^CONTENT_ASSERTION_FAILED/p' -e '/^  first_diff_byte=/p' \
		-e '/^  expected_byte=/p' -e '/^  scalar_index=/p' "$out.second" > "$key_second"

	if ! cmp -s "$key_first" "$key_second"; then
		fail "mutant $patch_name detection is not deterministic"
	fi
}

verify_mutant "0001-chunk-split-utf8-boundary.patch" \
	"转码分块未退回完整 UTF-8 字符边界"

verify_mutant "0002-supplementary-low-surrogate.patch" \
	"补充平面转 UTF-16 时低代理项错误"

verify_mutant "0003-endian-swap-missing.patch" \
	"输出端序交换失效"

# ---------- pristine source guarantee -------------------------------------

source_checksum_after=$(cksum "$src_dir/pugixml.cpp")

if [[ "$source_checksum_before" != "$source_checksum_after" ]]; then
	fail "the working source tree changed; mutants must only be applied to temporary copies"
fi

echo "source-checksum unchanged: $source_checksum_after"
echo "SERIALIZE_MODEL_FUZZ_OK"
