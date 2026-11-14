#!/usr/bin/env bash
#
# Mutation runner for the independent timing-model regression.
#
# Two throw-away copies of the tree are built.  In the first, the single
# MTC wrap-around correction in pt_time.c is removed; in the second, the
# lost_cyc reset performed at an MTC recovery is removed.  Each mutated
# library must still compile and must then be rejected by the new tests
# with a TIMING_MISMATCH diagnostic.  Anything else (compile failure,
# signal, timeout, missing/skipped tests) does not count.
#
# The working tree is never modified: every mutation lives in a temp copy.
#
# Only Bash and the standard userland are used.

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

cc_bin=${CC:-cc}
time_limit=60

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Mirror the include/source layout used by task_support/run.sh.  Paths are
# relative so they resolve inside each throw-away copy after cd-ing there.
incs=(-Ibuild/include -Ilibipt/internal/include -Iinclude -I.
      -Iptunit/include)
sources=(libipt/src/pt_encoder.c libipt/src/pt_last_ip.c
	 libipt/src/pt_event_decoder.c libipt/src/pt_sync.c
	 libipt/src/pt_tnt_cache.c libipt/src/pt_time.c
	 libipt/src/pt_event_queue.c libipt/src/pt_query_decoder.c
	 libipt/src/pt_packet.c libipt/src/pt_packet_decoder.c
	 libipt/src/pt_config.c libipt/src/pt_block_cache.c)

fatal() { echo "MUTATE_FAIL $*" >&2; exit 1; }

# Run a command with a hard timeout; preserve its exit code and flag kills.
run_limited() {
	local limit=$1 pid watcher rc
	shift
	"$@" &
	pid=$!
	( sleep "$limit"; kill -KILL "$pid" 2>/dev/null ) &
	watcher=$!
	if wait "$pid"; then rc=0; else rc=$?; fi
	kill "$watcher" 2>/dev/null || true
	wait "$watcher" 2>/dev/null || true
	if [ "$rc" -gt 128 ]; then
		echo "SIGNAL($rc)"
		return 99
	fi
	return "$rc"
}

# Prepare one isolated copy plus the generated public header.
make_copy() {
	local name=$1 dst=$2
	mkdir -p "$dst"
	cp -R libipt include ptunit task_support tests CMakeLists.txt "$dst"/
	mkdir -p "$dst/build/include"
	sed -e 's/${PT_VERSION_MAJOR}/2/g' \
	    -e 's/${PT_VERSION_MINOR}/3/g' \
	    -e 's/${PT_VERSION_PATCH}/0/g' \
	    libipt/include/intel-pt.h.in > "$dst/build/include/intel-pt.h"
	echo "$dst"
}

# Build the new regression against whatever pt_time.c the copy holds.
build_target() {
	local dst=$1
	( cd "$dst" && "$cc_bin" -std=c11 -O1 "${incs[@]}" "${sources[@]}" \
		task_support/main.c tests/task_cases.c -o build/target )
}

# Sanity gate: the unmodified library must pass all four groups.  This also
# rules out "all skipped" or missing-test configurations.
base=$work/base
make_copy base "$base" >/dev/null
build_target "$base" || fatal "baseline library did not compile"
if ! run_limited "$time_limit" "$base/build/target" \
	>"$work/base.out" 2>"$work/base.err"; then
	cat "$work/base.err" >&2
	fatal "unmodified library failed before mutation"
fi
for g in anchors wrap overlap recalibrate; do
	grep -q "PASS $g" "$work/base.out" || fatal "missing group $g"
done
grep -q TARGET_OK "$work/base.out" || fatal "missing TARGET_OK marker"

# One mutation run.  $1 is the KILLED label; the remaining arguments are a
# sed program (one string or several -e fragments).  $MARKER must appear
# exactly once in the mutated file afterwards.
run_mutation() {
	local label=$1 marker=$2 dst out rc
	shift 2
	dst=$work/$label
	out=$work/$label.out
	make_copy "$label" "$dst" >/dev/null

	# Apply the mutation only inside the throw-away copy.
	( cd "$dst" && sed -i.mutbak "$@" libipt/src/pt_time.c )
	if [ "$(grep -c "$marker" "$dst/libipt/src/pt_time.c")" -ne 1 ]; then
		echo "MUTATE_FAIL $label mutation did not apply" >&2
		return 1
	fi

	if ! build_target "$dst"; then
		echo "MUTATE_FAIL $label compile error" >&2
		return 1
	fi

	set +e
	run_limited "$time_limit" "$dst/build/target" >"$out" 2>&1
	rc=$?
	set -e

	if [ "$rc" = 99 ]; then
		echo "MUTATE_FAIL $label timeout or signal" >&2
		return 1
	fi
	if [ "$rc" -eq 0 ]; then
		echo "MUTATE_FAIL $label mutated library was not rejected" >&2
		return 1
	fi
	if ! grep -q TIMING_MISMATCH "$out"; then
		echo "MUTATE_FAIL $label rejected without TIMING_MISMATCH" >&2
		return 1
	fi

	# The diagnostic must name a packet/field rather than abort early.
	if ! grep -Eq 'TIMING_MISMATCH .* field=(tsc|lost_cyc|lost_mtc|fcr|update_errcode)' \
		"$out"; then
		echo "MUTATE_FAIL $label mismatch lacks a named field" >&2
		return 1
	fi

	echo "KILLED $label"
	grep -m1 TIMING_MISMATCH "$out" >&2 || true
}

# The legacy ptunit-time suite must keep passing in parallel with the
# mutation runs; it is built from the unmodified working tree.
run_old_time() {
	local dst=$work/oldtime
	mkdir -p "$dst/build/include"
	sed -e 's/${PT_VERSION_MAJOR}/2/g' \
	    -e 's/${PT_VERSION_MINOR}/3/g' \
	    -e 's/${PT_VERSION_PATCH}/0/g' \
	    "$root/libipt/include/intel-pt.h.in" \
	    > "$dst/build/include/intel-pt.h"
	( cd "$dst" && "$cc_bin" -std=c11 -O1 \
		-Ibuild/include -I"$root/libipt/internal/include" \
		-I"$root/include" -I"$root" -I"$root/ptunit/include" \
		"$root/libipt/src/pt_time.c" "$root/ptunit/src/ptunit.c" \
		"$root/libipt/test/src/ptunit-time.c" -o old-time ) || {
		echo "MUTATE_FAIL old-time compile error" >&2
		return 1
	}
	if ! run_limited "$time_limit" "$dst/old-time" \
		>"$work/oldtime.out" 2>&1; then
		echo "MUTATE_FAIL old-time unit suite failed" >&2
		return 1
	fi
	grep -q "fails: 0" "$work/oldtime.out" || {
		echo "MUTATE_FAIL old-time reported failures" >&2
		return 1
	}
	echo "OLD_TIME_OK"
}

# Mutation 1: drop the single-wrap correction in pt_time_ctc_delta().
wrap_prog='s|ctc += 1u << (config->mtc_freq + pt_pl_mtc_bit_size);|ctc = ctc; /* MUTATED_WRAP */|'

# Mutation 2: drop the lost_cyc recovery in pt_time_update_mtc().  The same
# assignment exists in pt_time_update_tsc(), so restrict the edit to the
# update_mtc() function body.  Braces are split into separate -e blocks so
# the program works with both BSD and GNU sed.
loss_prog=(-e '/^int pt_time_update_mtc/,/^}/{' -e 's|time->lost_cyc = 0;|/* MUTATED_LOSS_RESET */ (void) 0;|' -e '}')

run_mutation wrap MUTATED_WRAP -e "$wrap_prog" &
pid_wrap=$!
run_mutation loss_reset MUTATED_LOSS_RESET "${loss_prog[@]}" &
pid_loss=$!
run_old_time &
pid_old=$!

status=0
wait "$pid_wrap" || status=1
wait "$pid_loss" || status=1
wait "$pid_old" || status=1

if [ "$status" -eq 0 ]; then
	echo "MUTATE_OK both mutations killed, ptunit-time clean"
fi
exit "$status"
