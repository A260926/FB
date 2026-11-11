#!/usr/bin/env bash
# Mutation runner for the AArch64 addressing contract test.
#
# Each patch in tests/arm64_mutations/ breaks exactly one decoding
# behaviour.  Every mutant is applied to a private copy of the source
# tree, must build cleanly, and then runs the very same
# tests/arm64_address_contract.c binary compiled against the mutated
# library.  A mutant counts as killed only when the checks tagged with
# its named behaviour fail and nothing else does.
#
# Runner errors (patch does not apply, build or compile failure,
# timeout, death by signal, mutant not detected, extra failures) are
# reported on stderr and end in a non-zero exit status.

set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

err()
{
	echo "run_arm64_mutations: error: $*" >&2
	exit 1
}

# run_with_timeout SECONDS CMD...  -> command status, 124 on timeout
run_with_timeout()
{
	local secs=$1 flag="$work/timeout.flag" pid watcher rc
	shift
	rm -f "$flag"
	"$@" &
	pid=$!
	( sleep "$secs"; touch "$flag"; kill -KILL "$pid" 2>/dev/null ) &
	watcher=$!
	wait "$pid" 2>/dev/null
	rc=$?
	kill "$watcher" 2>/dev/null
	wait "$watcher" 2>/dev/null
	if [ -f "$flag" ]; then
		return 124
	fi
	return "$rc"
}

run_mutant()
{
	local name=$1
	local patch="$root/tests/arm64_mutations/$name.patch"
	local src="$work/src-$name"
	local build="$work/build-$name"
	local out="$work/out-$name"
	local rc

	[ -f "$patch" ] || err "missing patch $patch"

	mkdir -p "$src" || err "cannot create $src"
	(cd "$root" && tar --exclude=./.git --exclude=./.tmp -cf - .) \
		| (cd "$src" && tar -xf -) || err "source copy for $name failed"
	(cd "$src" && patch -s -p1 < "$patch") || err "applying $name.patch failed"

	mkdir -p "$build" || err "cannot create $build"
	bash "$src/baseline/build.sh" "$build" > "$work/build-$name.log" 2>&1 \
		|| err "mutant $name failed to build"
	[ -s "$build/libcapstone.a" ] || err "mutant $name produced no library"

	cc -std=c11 -I"$src/include" "$src/tests/arm64_address_contract.c" \
		"$build/libcapstone.a" -o "$work/contract-$name" \
		|| err "mutant $name test compile failed"

	run_with_timeout 120 "$work/contract-$name" > "$out" 2>&1
	rc=$?
	[ "$rc" -eq 124 ] && err "mutant $name timed out"
	[ "$rc" -gt 128 ] && err "mutant $name died by signal"
	[ "$rc" -eq 0 ] && err "mutant $name was not detected by the contract test"

	grep -q "^FAIL $name " "$out" \
		|| err "mutant $name did not fail the $name checks"
	if grep "^FAIL " "$out" | grep -vq "^FAIL $name "; then
		err "mutant $name broke checks outside $name"
	fi
	echo "KILLED $name"
}

run_mutant post_index
run_mutant signed_offset
run_mutant base_writeback
