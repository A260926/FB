#!/usr/bin/env bash
#
# Mutation self-test for the narrowband PLC regression.
#
# For each mutation below, a throw-away copy of the sources is built with the
# exact same compiler flags used by `bash task_support/run.sh target`, and the
# same content assertions run.  A mutation counts as detected only when:
#   * the mutated library and tests compile,
#   * the runner exits with status 4 (a task_check failure),
#   * the output contains a CONTENT_MISMATCH line (an actual assertion fired
#     on real case data, not a crash or an empty case list).
#
# The original tree is never written to.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
pristine_sum=$(cksum "$repo/libspeex/nb_celp.c")

tmpbase=${TMPDIR:-/tmp}
work=$(mktemp -d "$tmpbase/plc-mutations.XXXXXXXX")
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

mkdir -p "$work/src"
cp -R "$repo/libspeex" "$repo/include" "$repo/task_support" "$repo/tests" "$work/src/"
cp "$work/src/libspeex/nb_celp.c" "$work/nb_celp.pristine.c"

# `run.sh` calls a bare `mktemp` for its log file.  On macOS that ignores
# TMPDIR and tries the per-user /var/folders directory, which may be closed in
# a sandbox.  Put a shim in front of PATH that supplies a template under
# TMPDIR.  The repository itself and run.sh are untouched.
real_mktemp=$(command -v mktemp)
mkdir -p "$work/shim"
cat > "$work/shim/mktemp" <<EOF
#!/bin/sh
if [ "\$#" -gt 0 ]; then
  exec "$real_mktemp" "\$@"
fi
exec "$real_mktemp" "\${TMPDIR:-/tmp}/plc-target.XXXXXXXX"
EOF
chmod +x "$work/shim/mktemp"

mutate() {
   python3 -I - "$1" "$2" "$3" "$4" <<'PY'
import sys
path, old, new, tag = sys.argv[1:5]
text = open(path, 'r', encoding='utf-8').read()
count = text.count(old)
if count != 1:
    sys.stderr.write("%s: expected exactly one match, found %d\n" % (tag, count))
    sys.exit(2)
open(path, 'w', encoding='utf-8').write(text.replace(old, new))
PY
}

run_mutation() {
   local name=$1
   shift
   local log="$work/$name.log"
   local status

   cp "$work/nb_celp.pristine.c" "$work/src/libspeex/nb_celp.c"
   rm -rf "$work/src/.task-build"

   while [[ $# -gt 0 ]]; do
      mutate "$work/src/libspeex/nb_celp.c" "$1" "$2" "$name"
      shift 2
   done

   set +e
   ( cd "$work/src" && PATH="$work/shim:$PATH" bash task_support/run.sh target ) \
      >"$log" 2>&1
   status=$?
   set -e

   if [[ ! -x "$work/src/.task-build/target" ]]; then
      echo "MUTATION $name: NOT DETECTED (mutated tree failed to build)"
      sed -n '1,12p' "$log" | sed 's/^/    /'
      return 1
   fi
   if ! grep -q '^PLC_CASE_BEGIN ' "$log"; then
      echo "MUTATION $name: NOT DETECTED (runner produced no case output)"
      sed -n '1,12p' "$log" | sed 's/^/    /'
      return 1
   fi
   if [[ $status -ne 4 ]]; then
      if [[ $status -gt 128 ]]; then
         echo "MUTATION $name: NOT DETECTED (runner crashed, status=$status)"
      else
         echo "MUTATION $name: NOT DETECTED (runner status=$status, expected 4)"
      fi
      tail -n 12 "$log" | sed 's/^/    /'
      return 1
   fi
   if ! grep -q '^CONTENT_MISMATCH ' "$log"; then
      echo "MUTATION $name: NOT DETECTED (exit 4 without a content assertion)"
      return 1
   fi
   if grep -q '^TARGET_COMPLETE ' "$log"; then
      echo "MUTATION $name: NOT DETECTED (target completed anyway)"
      return 1
   fi

   local first
   first=$(grep -m1 '^CONTENT_MISMATCH ' "$log")
   local caught
   caught=$(printf '%s\n' "$first" | sed -E 's/^CONTENT_MISMATCH group=([^ ]+) case=([^ ]+) .*/\1\/\2/')
   echo "MUTATION $name: build OK, runner status 4, DETECTED"
   echo "  caught case: $caught"
   echo "  first deviation: $first"
   return 0
}

fail=0

# Mutation 1: the lost-frame excitation only repeats the old pitch period --
# no pitch gain, no per-sample noise and no noise random draw.
run_mutation repeat_old_pitch \
   '   for (i=0;i<NB_FRAME_SIZE;i++)
   {
      st->exc[i]= MULT16_16_Q15(pitch_gain, (st->exc[i-pitch_val]+VERY_SMALL)) +
            speex_rand(noise_gain, &st->seed);
   }' \
   '   /* MUTATION: repeat the old pitch period only */
   for (i=0;i<NB_FRAME_SIZE;i++)
   {
      st->exc[i]= st->exc[i-pitch_val];
   }' || fail=1

# Mutation 2: the pitch-gain ring stores the unattenuated gain instead of the
# attenuated result that drives the next frame's median/pitch gain.
run_mutation unattenuated_ring_writeback \
   '   spx_word16_t noise_gain;' \
   '   spx_word16_t noise_gain;
   spx_word16_t raw_gain;' \
   '   pitch_gain = MULT16_16_Q15(fact,pitch_gain) + VERY_SMALL;' \
   '   raw_gain = pitch_gain;
   pitch_gain = MULT16_16_Q15(fact,pitch_gain) + VERY_SMALL;' \
   '   st->pitch_gain_buf[st->pitch_gain_buf_idx++] = PSHR16(pitch_gain,9);' \
   '   st->pitch_gain_buf[st->pitch_gain_buf_idx++] = PSHR16(raw_gain,9);' || fail=1

# The mutations must have been applied to the copy only.
if [[ $(cksum "$repo/libspeex/nb_celp.c") != "$pristine_sum" ]]; then
   echo "MUTATION_CHECK_FAILED original libspeex/nb_celp.c was modified"
   exit 1
fi

if [[ $fail -ne 0 ]]; then
   echo "MUTATION_CHECK_FAILED"
   exit 1
fi

echo "PLC_MUTATION_SELFTEST_OK mutations=2 original_tree_unchanged=1"
