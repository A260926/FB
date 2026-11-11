#!/bin/sh
# check_shadow.sh - build and run the coloured soft shadow regression contract.
#
# Usage: sh harness/check_shadow.sh [--mutations]
#
# Builds harness/shadow_contract.c against the pristine sources in a temporary
# directory and runs it. With --mutations, three extra copies of the sources
# are built, each carrying exactly one single-point behaviour change:
#   bounded_overwrite    bounded transparent objects overwrite the intensity
#                        with kt instead of multiplying it (accel.c)
#   unbound_channel_drop unbounded transparent objects skip one kt channel
#                        (object.c)
#   bounded_distance_bound
#                        bounded shadow ignores the light distance upper bound
#                        (accel.c)
# Every mutated copy must still compile and run, and must be caught by the
# relevant numeric or occlusion assertion. A mutation that does not apply,
# fails to build, crashes, times out, or is not detected fails this script.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$script_dir/.." && pwd)
cd "$root"

[ -f harness/shadow_contract.c ] || {
	echo "check_shadow: missing harness/shadow_contract.c" >&2
	exit 1
}

mutations=0
for arg in "$@"; do
	case "$arg" in
	--mutations) mutations=1 ;;
	*) echo "check_shadow: unknown argument '$arg'" >&2 ; exit 1 ;;
	esac
done

b=$(mktemp -d "${TMPDIR:-/tmp}/shadow_contract.XXXXXX")
trap 'rm -rf "$b"' EXIT HUP INT TERM

CC=${CC:-cc}
CFLAGS="-std=c11 -O1 -DUNBOUND_OBJECTS -I src/core -I src/raytracer -I lib/SimplexNoise"
LIBSRCS="src/core/*.c src/raytracer/material.c src/raytracer/object.c src/raytracer/accel.c src/raytracer/render.c lib/SimplexNoise/SimplexNoise.c"

# run_with_timeout SECONDS CMD [ARGS...] - returns the command's exit status,
# or 137 if it had to be killed after SECONDS.
run_with_timeout() {
	tsec=$1
	shift
	"$@" &
	pid=$!
	( sleep "$tsec"; kill -KILL "$pid" 2>/dev/null ) &
	wp=$!
	if wait "$pid"; then rc=0; else rc=$?; fi
	kill "$wp" 2>/dev/null || :
	wait "$wp" 2>/dev/null || :
	return "$rc"
}

echo "check_shadow: building pristine checker"
# shellcheck disable=SC2086
$CC $CFLAGS harness/shadow_contract.c $LIBSRCS -lm -o "$b/shadow_contract"

echo "check_shadow: running pristine contract"
if run_with_timeout 60 "$b/shadow_contract" "$b/shadow_contract.ppm"; then
	:
else
	echo "check_shadow: pristine contract FAILED (rc=$?)" >&2
	exit 1
fi
cp "$b/shadow_contract.ppm" ./shadow_contract.ppm
echo "check_shadow: wrote ./shadow_contract.ppm"

# run_mutation NAME FILE OLD NEW EXPECT
run_mutation() {
	mname=$1
	mfile=$2
	mold=$3
	mnew=$4
	mexpect=$5
	mdir="$b/mut_$mname"
	mkdir -p "$mdir/harness"
	cp -R src "$mdir/src"
	cp -R lib "$mdir/lib"
	cp harness/shadow_contract.c "$mdir/harness/"
	target="$mdir/$mfile"

	if [ "$(grep -cF "$mold" "$target" || :)" != "1" ]; then
		echo "check_shadow: mutation $mname: pattern not found exactly once in $mfile" >&2
		exit 1
	fi
	mnew_esc=$(printf '%s' "$mnew" | sed 's/[&\\]/\\&/g')
	sed -i.bak "s|$mold|$mnew_esc|" "$target"
	rm -f "$target.bak"
	grep -qF "$mnew" "$target" || {
		echo "check_shadow: mutation $mname: change did not apply" >&2
		exit 1
	}

	if (cd "$mdir" && $CC $CFLAGS harness/shadow_contract.c $LIBSRCS -lm -o "$mdir/checker"); then
		:
	else
		echo "check_shadow: mutation $mname: failed to compile" >&2
		exit 1
	fi

	if run_with_timeout 60 "$mdir/checker" "$mdir/out.ppm" >"$mdir/out.log" 2>&1; then
		mrc=0
	else
		mrc=$?
	fi
	if [ "$mrc" -eq 0 ]; then
		echo "check_shadow: mutation $mname: NOT detected (checker passed)" >&2
		exit 1
	fi
	if [ "$mrc" -ne 1 ]; then
		echo "check_shadow: mutation $mname: abnormal termination (rc=$mrc)" >&2
		cat "$mdir/out.log" >&2
		exit 1
	fi
	fline=$(grep '^SHADOW_CONTRACT_FAIL' "$mdir/out.log" | head -n 1 || :)
	[ -n "$fline" ] || {
		echo "check_shadow: mutation $mname: no failure report" >&2
		cat "$mdir/out.log" >&2
		exit 1
	}
	if ! echo "$fline" | grep -qF "$mexpect"; then
		echo "check_shadow: mutation $mname: caught by unexpected assertion:" >&2
		echo "  $fline" >&2
		echo "  expected first failure matching: $mexpect" >&2
		exit 1
	fi
	echo "check_shadow: mutation $mname detected: $fline"
}

if [ "$mutations" -eq 1 ]; then
	# bounded transparent object overwrites intensity with kt instead of multiplying
	run_mutation bounded_overwrite \
		src/raytracer/accel.c \
		'mul3v(light_intensity, object->material->kt, light_intensity);' \
		'assign3(light_intensity, object->material->kt);' \
		'case=A1 channel=0'

	# unbounded transparent object misses multiplying one channel
	run_mutation unbound_channel_drop \
		src/raytracer/object.c \
		'mul3v(light_intensity, object->material->kt, light_intensity);' \
		'{ light_intensity[0] *= object->material->kt[0]; light_intensity[1] *= object->material->kt[1]; }' \
		'case=A1 channel=2'

	# bounded shadow ignores the light distance upper bound
	run_mutation bounded_distance_bound \
		src/raytracer/accel.c \
		'get_intersection(object, ray, &tmin, normal) && tmin < distance' \
		'get_intersection(object, ray, &tmin, normal)' \
		'case=A11 channel=0'
fi

echo SHADOW_CONTRACT_OK
