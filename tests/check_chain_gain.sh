#!/usr/bin/env bash
#
# Mutation-test driver for tests/chain_gain.c.
#
# The same regression binary is built and run against:
#   1. the pristine source tree (must pass and print CHAIN_GAIN_OK);
#   2. a temporary copy where the OpusHead header gain is no longer applied in
#      OP_HEADER_GAIN mode (must be caught: DETECTED_HEADER_GAIN);
#   3. a temporary copy where EOS last-packet end-trimming is disabled
#      (must be caught: DETECTED_END_TRIM).
#
# A mutated copy has to COMPILE successfully and then be stopped by a PCM or
# frame-count assertion in the test itself (exit status 1). Missing files,
# compile failures and crashes do not count as detections, and the working
# source tree is never modified: every mutation happens in a temp copy.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
#Keep scratch work inside the tree: the OS temp dir may not be writable in a
# sandboxed environment (same convention as harness/run.sh).
work=$(mktemp -d ./.chain-gain.XXXXXX)
trap 'rm -rf "$work"' EXIT

fail(){
  echo "CHECK_CHAIN_GAIN_ERROR: $*" >&2
  exit 1
}

#Fixed-string, single-occurrence replacement.
#bash 3.2's ${var//old/new} is quadratic on large files (opusfile.c is ~130 KB),
# so do the literal substitution with perl instead.
replace_once(){
  local file=$1 old=$2 new=$3
  [ -f "$file" ] || fail "mutation target missing: $file"
  [ "$(grep -F -c -- "$old" "$file" || true)" = "1" ] || \
    fail "mutation anchor not unique in $file"
  CHAIN_OLD="$old" CHAIN_NEW="$new" perl -i -pe '
    BEGIN { $old=$ENV{CHAIN_OLD}; $new=$ENV{CHAIN_NEW}; }
    s/\Q$old\E/$new/g;
  ' "$file"
}

compile_chain_test(){
  local srcdir=$1 binary=$2
  local -a flags
  read -r -a flags <<< "$(pkg-config --cflags --libs opus ogg)"
  cc -std=c99 -O1 -DOP_HAVE_LRINTF -I"$srcdir/include" \
    "$srcdir/tests/chain_gain.c" \
    "$srcdir/src/info.c" "$srcdir/src/internal.c" \
    "$srcdir/src/opusfile.c" "$srcdir/src/stream.c" \
    "${flags[@]}" -lm -o "$binary"
}

#Build one tree, run the regression, and classify the result.
#  $1 source tree, $2 binary name, $3 expected result: "pass" or "detect"
#  $4 detection token printed on a successful catch
run_tree(){
  local srcdir=$1 binary=$2 expectation=$3 token=$4 log rc
  for f in tests/chain_gain.c src/opusfile.c src/info.c \
    src/internal.c src/stream.c include/opusfile.h; do
    [ -f "$srcdir/$f" ] || fail "required source missing: $srcdir/$f"
  done
  if ! compile_chain_test "$srcdir" "$work/$binary" >"$work/$binary.build" 2>&1; then
    cat "$work/$binary.build" >&2
    fail "$binary did not compile"
  fi
  set +e
  "$work/$binary" >"$work/$binary.log" 2>&1
  rc=$?
  set -e
  if [ "$expectation" = pass ]; then
    if [ "$rc" -ne 0 ]; then
      cat "$work/$binary.log" >&2
      fail "pristine tree failed (rc=$rc)"
    fi
    grep -q "CHAIN_GAIN_OK" "$work/$binary.log" \
      || fail "pristine run did not print CHAIN_GAIN_OK"
  else
    #The test returns 1 on a failed CHAIN_CHECK assertion. Signals, setup
    #errors (exit 2) and clean passes must not be mistaken for a detection.
    if [ "$rc" -ne 1 ]; then
      cat "$work/$binary.log" >&2
      fail "$token: mutation was not caught by an assertion (rc=$rc)"
    fi
    grep -E -q "FAIL: .*(PCM|frame)" "$work/$binary.log" \
      || fail "$token: failure was not a PCM/frame assertion"
    echo "$token"
  fi
}

pkg-config --exact-version=1.6.1 opus >/dev/null \
  || fail "libopus 1.6.1 not found through pkg-config"
pkg-config --exact-version=1.3.6 ogg >/dev/null \
  || fail "libogg 1.3.6 not found through pkg-config"

#1. Pristine tree.
run_tree "$root" pristine pass ""
echo "CHAIN_GAIN_OK"

#2. Header gain removed: OP_HEADER_GAIN must still add head->output_gain.
mkdir -p "$work/header_gain_tree"
cp -R "$root/src" "$root/include" "$root/tests" "$work/header_gain_tree/"
replace_once "$work/header_gain_tree/src/opusfile.c" \
  "case OP_HEADER_GAIN:gain_q8+=head->output_gain;break;" \
  "case OP_HEADER_GAIN:break;"
run_tree "$work/header_gain_tree" no_header_gain detect DETECTED_HEADER_GAIN

#3. End trim removed: the EOS packet must be shortened to the granule limit.
mkdir -p "$work/end_trim_tree"
cp -R "$root/src" "$root/include" "$root/tests" "$work/end_trim_tree/"
replace_once "$work/end_trim_tree/src/opusfile.c" \
  "if(OP_UNLIKELY(pop->e_o_s)){" \
  "if(0){"
run_tree "$work/end_trim_tree" no_end_trim detect DETECTED_END_TRIM

#The existing small-scope, HTTP-free harness must keep working.
bash "$root/harness/run.sh" >"$work/harness.log" 2>&1 \
  || { cat "$work/harness.log" >&2; fail "harness/run.sh failed"; }
