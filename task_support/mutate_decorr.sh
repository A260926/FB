#!/usr/bin/env bash
#
# Mutation testing for the decorrelation certification.  Two deliberately
# faulty copies of the independent model / block machinery are built in a
# temporary source tree and the SAME numeric assertions must reject them.
# A mutation that compiles but passes the assertions would be a test gap, so
# this script fails loudly in that case.  Neither crash-only behavior nor a
# CRC-only complaint counts as rejection: the value assertions themselves
# have to fire.
#
# Mutation 1: term -3 uses the CURRENT-beat opposite (the -1 behavior) instead
#             of the previous-beat opposite.
# Mutation 2: negative-term weight clipping is removed, so weights escape
#             the +/-1024 limits.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/.task-mutate.$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

OPT="${1:--O0}"
OBJDIR="$ROOT/.task-dtobj"
LIBA="${DT_LIBA:-$OBJDIR/libdt.a}"

if [ ! -f "$LIBA" ]; then
    echo "mutate: instrumented library $LIBA missing; run check_decorrelation.sh first" >&2
    exit 2
fi

CFLAGS="-std=c99 $OPT -g -Wall -Wextra -DDECORR_TEST_OBSERVE -DHAVE___BUILTIN_CLZ -DHAVE_FSEEKO -I$ROOT/include -I$ROOT/src -I$ROOT/tests"

# Copy the test sources so the production tree stays byte-identical.
cp -R "$ROOT/tests" "$WORK/tests"

run_mutation () {
    local name="$1"
    local log="$WORK/$name.log"

    if cc $CFLAGS "$WORK/tests/decorr_reference.c" "$LIBA" -lm -o "$WORK/ref_$name" 2>"$WORK/build_$name.err"; then
        :
    else
        echo "MUTATION $name: unexpected compile failure" >&2
        cat "$WORK/build_$name.err" >&2
        return 1
    fi

    # A correct test suite must reject the mutation via numeric assertions.
    if "$WORK/ref_$name" >"$log" 2>&1; then
        echo "MUTATION $name: NOT DETECTED (test suite passed a faulty model)" >&2
        return 1
    fi

    if grep -q "FIRST DIFF\|REFERENCE FAIL" "$log"; then
        echo "MUTATION $name: rejected by numeric assertions"
        return 0
    fi

    echo "MUTATION $name: failed but without a numeric assertion (crash/CRC only)" >&2
    return 1
}

# ---- Mutation 1: -3 reads the current-beat opposite ---------------------
#
# In the correct model the term -3 branch predicts each channel from the
# opposite channel saved at the PREVIOUS beat and only swaps the saved
# opposites after both reconstructions.  The mutation moves the swap ahead
# of the prediction, which is exactly the -1 same-beat dependency.
python3 - "$WORK/tests/decorr_test.h" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
needle = """        else { /* term -3: each side uses its own saved opposite from the PREVIOUS beat */
            int32_t srcL = p->hist [0] [0];               /* saved previous right */
            int32_t srcR = p->hist [1] [0];               /* saved previous left  */
            int32_t predL, predR, left, right;
"""
assert needle in s, "mutation 1 anchor not found"
mutated = needle + """
            /* MUTATION: deliberately use the current-beat opposite (term -1 behavior) */
            srcR = out [1];
            srcL = out [0];
"""
s = s.replace(needle, mutated, 1)
open(path, "w").write(s)
PY
run_mutation "neg3_current_opposite"

# restore a pristine copy for the second mutation
rm -rf "$WORK/tests"
cp -R "$ROOT/tests" "$WORK/tests"

# ---- Mutation 2: drop the negative-term weight clip ---------------------
#
# The dt_update helper clips only when its `clip` argument is set, and the
# negative terms pass 1.  The mutation forces clip to 0 for every term, so
# negative-term weights can travel beyond +/-1024 while positive terms are
# unaffected.
python3 - "$WORK/tests/decorr_test.h" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
needle = "        int negclip = (p->term < 0);\n"
assert needle in s, "mutation 2 anchor not found"
s = s.replace(needle, "        int negclip = 0; /* MUTATION: clipping disabled */\n", 1)
open(path, "w").write(s)
PY
run_mutation "no_negative_clip"

echo "ALL MUTATIONS DETECTED"
