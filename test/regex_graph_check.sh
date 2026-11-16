#!/usr/bin/env bash
#
# regex_graph_check.sh - build the production library and run the four
# regex-graph regression groups plus the two example programs.
#
# Everything is built from in-repo sources with the C compiler and CMake
# only.  Both dispatch configurations are built and compared:
#
#   direct  - default build (direct-threaded opcode table)
#   switch  - ONIG_DISABLE_DIRECT_THREADING
#
# A compile or run failure makes the script exit non-zero.  Build and run
# logs are kept under $TMPDIR/regex_graph_check_log (overridable with
# REGEX_GRAPH_LOG_DIR; falls back to ./regex_graph_check_log when TMPDIR is
# unset).
#
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC_BIN=${CC:-cc}
LOG_ROOT=${TMPDIR:-/tmp}/regex_graph_check_log
LOG_ROOT=${REGEX_GRAPH_LOG_DIR:-$LOG_ROOT}
rm -rf "$LOG_ROOT"
mkdir -p "$LOG_ROOT"

FAILURES=0
SUMMARY=""

log() {
    printf '%s\n' "$*" | tee -a "$LOG_ROOT/driver.log" >&2
}

fail() {
    FAILURES=$((FAILURES + 1))
    log "FAIL: $*"
}

note_pass() {
    SUMMARY="$SUMMARY
PASS: $*"
    log "PASS: $*"
}

run_logged() {
    # run_logged NAME COMMAND...
    _name=$1
    shift
    _log=$LOG_ROOT/$_name.log
    printf '$' >"$_log"
    for _a in "$@"; do printf ' %s' "$_a" >>"$_log"; done
    printf '\n' >>"$_log"
    "$@" >>"$_log" 2>&1
    _rc=$?
    return $_rc
}

# ---------------------------------------------------------------------
# Build matrix.
# ---------------------------------------------------------------------

build_mode() {
    _mode=$1
    _flags=""
    [ "$_mode" = switch ] && _flags="-DONIG_DISABLE_DIRECT_THREADING"
    _bdir=$LOG_ROOT/build-$_mode

    log "== building library (mode=$_mode) =="
    if ! run_logged "cmake-configure-$_mode" \
        cmake -S "$ROOT" -B "$_bdir" \
              -DBUILD_SHARED_LIBS=OFF \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
              -DCMAKE_C_FLAGS="$_flags"; then
        fail "cmake configure ($_mode); see $LOG_ROOT/cmake-configure-$_mode.log"
        return 1
    fi
    if ! run_logged "cmake-build-$_mode" \
        cmake --build "$_bdir" --parallel 4; then
        fail "library build ($_mode); see $LOG_ROOT/cmake-build-$_mode.log"
        return 1
    fi
    if [ ! -f "$_bdir/libonig.a" ]; then
        fail "libonig.a missing after build ($_mode)"
        return 1
    fi
    note_pass "library build ($_mode)"
    return 0
}

compile_program() {
    # compile_program MODE NAME SOURCE
    _mode=$1
    _name=$2
    _src=$3
    _bdir=$LOG_ROOT/build-$_mode
    _flags=""
    [ "$_mode" = switch ] && _flags="-DONIG_DISABLE_DIRECT_THREADING"
    _out=$LOG_ROOT/bin-$_mode-$_name
    mkdir -p "$LOG_ROOT/bin"
    if ! run_logged "compile-$_mode-$_name" \
        "$CC_BIN" -std=c99 -O1 -Wall -Wextra -Werror \
        -DHAVE_CONFIG_H -DONIG_STATIC $_flags \
        -I "$_bdir" -I "$ROOT/src" \
        "$_src" "$_bdir/libonig.a" -o "$_out"; then
        fail "compile $_name ($_mode); see $LOG_ROOT/compile-$_mode-$_name.log"
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------
# Group 1: contract test (four internal groups).
# ---------------------------------------------------------------------

run_contract() {
    _mode=$1
    _bin=$LOG_ROOT/bin-$_mode-contract
    log "== contract test (mode=$_mode) =="
    if ! run_logged "contract-$_mode" "$_bin"; then
        fail "contract test ($_mode); see $LOG_ROOT/contract-$_mode.log"
        return 1
    fi
    if ! grep -q '^RESULT PASS groups=4 ' "$LOG_ROOT/contract-$_mode.log"; then
        fail "contract test ($_mode) did not print RESULT PASS"
        return 1
    fi
    _n=$(grep -c '^GROUP ' "$LOG_ROOT/contract-$_mode.log" || true)
    if [ "$_n" -ne 4 ]; then
        fail "contract test ($_mode) reported $_n groups, expected 4"
        return 1
    fi
    note_pass "contract test, four groups ($_mode)"
    return 0
}

# ---------------------------------------------------------------------
# Group 2: audit regression test.
# ---------------------------------------------------------------------

run_audit_contract() {
    _mode=$1
    _audit=$LOG_ROOT/bin-$_mode-audit
    _bin=$LOG_ROOT/bin-$_mode-auditcontract
    log "== audit regression (mode=$_mode) =="
    if ! run_logged "audit-contract-$_mode" "$_bin" "$_audit"; then
        fail "audit regression ($_mode); see $LOG_ROOT/audit-contract-$_mode.log"
        return 1
    fi
    if ! grep -q '^RESULT PASS ' "$LOG_ROOT/audit-contract-$_mode.log"; then
        fail "audit regression ($_mode) did not print RESULT PASS"
        return 1
    fi
    note_pass "audit regression ($_mode)"
    return 0
}

# ---------------------------------------------------------------------
# Group 3: determinism across processes and dispatch modes.
# ---------------------------------------------------------------------

run_determinism() {
    log "== determinism across processes and dispatch modes =="
    _s=$LOG_ROOT/det-switch-1.txt
    _s2=$LOG_ROOT/det-switch-2.txt
    _d=$LOG_ROOT/det-direct-1.txt
    _d2=$LOG_ROOT/det-direct-2.txt
    if ! run_logged "det-switch-1" "$LOG_ROOT/bin-switch-determinism" emit "$_s" 1; then
        fail "determinism run (switch, seed 1)"
        return 1
    fi
    if ! run_logged "det-switch-2" "$LOG_ROOT/bin-switch-determinism" emit "$_s2" 424242; then
        fail "determinism run (switch, seed 424242)"
        return 1
    fi
    if ! run_logged "det-direct-1" "$LOG_ROOT/bin-direct-determinism" emit "$_d" 1; then
        fail "determinism run (direct, seed 1)"
        return 1
    fi
    if ! run_logged "det-direct-2" "$LOG_ROOT/bin-direct-determinism" emit "$_d2" 987654; then
        fail "determinism run (direct, seed 987654)"
        return 1
    fi
    if ! cmp -s "$_s" "$_s2"; then
        fail "two processes with different allocation orders differ (switch)"
        return 1
    fi
    if ! cmp -s "$_d" "$_d2"; then
        fail "two processes with different allocation orders differ (direct)"
        return 1
    fi
    if ! cmp -s "$_s" "$_d"; then
        fail "switch and direct builds differ"
        return 1
    fi
    # No pointer-looking tokens in the canonical signature.
    if grep -Eq '0x[0-9a-fA-F]{6,}' "$_s"; then
        fail "determinism output contains a pointer-like token"
        return 1
    fi

    # The actual serialized output must be byte-identical across two
    # independent processes and across the two dispatch builds.  The
    # pattern is written with an explicit byte length (a trailing NUL and
    # newline), so the comparison also exercises length-driven decoding.
    _pat=$LOG_ROOT/det-pattern.txt
    printf '((?:a|b){2,4}){2,4}|(?<node>a(?:\\g<node>)?b)' >"$_pat"
    for _fmt in json dot; do
        for _mode in switch direct; do
            _ex=$LOG_ROOT/bin-$_mode-example
            if ! "$_ex" --pattern-file "$_pat" --format "$_fmt" \
                >"$LOG_ROOT/det-out-$_fmt-$_mode-1.out" \
                2>"$LOG_ROOT/det-out-$_fmt-$_mode-1.err"; then
                fail "determinism example output ($_fmt, $_mode)"
                return 1
            fi
        done
        # Second, separate process for each mode.
        for _mode in switch direct; do
            _ex=$LOG_ROOT/bin-$_mode-example
            if ! "$_ex" --pattern-file "$_pat" --format "$_fmt" \
                >"$LOG_ROOT/det-out-$_fmt-$_mode-2.out" \
                2>"$LOG_ROOT/det-out-$_fmt-$_mode-2.err"; then
                fail "determinism example output, second process ($_fmt, $_mode)"
                return 1
            fi
        done
        if ! cmp -s "$LOG_ROOT/det-out-$_fmt-switch-1.out" \
                    "$LOG_ROOT/det-out-$_fmt-switch-2.out"; then
            fail "$_fmt output differs across processes (switch)"
            return 1
        fi
        if ! cmp -s "$LOG_ROOT/det-out-$_fmt-direct-1.out" \
                    "$LOG_ROOT/det-out-$_fmt-direct-2.out"; then
            fail "$_fmt output differs across processes (direct)"
            return 1
        fi
        if ! cmp -s "$LOG_ROOT/det-out-$_fmt-switch-1.out" \
                    "$LOG_ROOT/det-out-$_fmt-direct-1.out"; then
            fail "$_fmt output differs between dispatch builds"
            return 1
        fi
        if grep -Eq '0x[0-9a-fA-F]{6,}' "$LOG_ROOT/det-out-$_fmt-switch-1.out"; then
            fail "$_fmt output contains a pointer-like token"
            return 1
        fi
    done

    # Audit output must also agree across the two dispatch builds.
    if ! "$LOG_ROOT/bin-switch-audit" \
        --pattern '((?:a|b){2,4}){2,4}' \
        >"$LOG_ROOT/det-audit-switch.out" 2>"$LOG_ROOT/det-audit-switch.err"; then
        fail "determinism audit (switch)"
        return 1
    fi
    if ! "$LOG_ROOT/bin-direct-audit" \
        --pattern '((?:a|b){2,4}){2,4}' \
        >"$LOG_ROOT/det-audit-direct.out" 2>"$LOG_ROOT/det-audit-direct.err"; then
        fail "determinism audit (direct)"
        return 1
    fi
    if ! cmp -s "$LOG_ROOT/det-audit-switch.out" \
                "$LOG_ROOT/det-audit-direct.out"; then
        fail "audit output differs between dispatch builds"
        return 1
    fi

    note_pass "determinism across processes and dispatch modes"
    return 0
}

# ---------------------------------------------------------------------
# Group 4: the two example programs, JSON and DOT.
# ---------------------------------------------------------------------

run_examples() {
    _mode=$1
    _ex=$LOG_ROOT/bin-$_mode-example
    _audit=$LOG_ROOT/bin-$_mode-audit
    _pat=$LOG_ROOT/example-pattern.txt
    log "== examples (mode=$_mode) =="

    # Pattern file with an exact byte length (includes a trailing newline
    # and a NUL byte, so the graph input is length-driven, not C-string
    # driven).
    printf '(?:a|bc){2,5}' >"$_pat"

    if ! run_logged "example-json-$_mode" \
        "$_ex" --pattern-file "$_pat" --format json; then
        fail "example JSON ($_mode)"
        return 1
    fi
    if ! grep -q '"format": "onig-regex-graph"' \
        "$LOG_ROOT/example-json-$_mode.log"; then
        fail "example JSON ($_mode) has no format banner"
        return 1
    fi
    if ! grep -q '"repeats":' "$LOG_ROOT/example-json-$_mode.log"; then
        fail "example JSON ($_mode) has no repeats section"
        return 1
    fi

    if ! run_logged "example-dot-$_mode" \
        "$_ex" --pattern-file "$_pat" --format dot; then
        fail "example DOT ($_mode)"
        return 1
    fi
    if ! grep -q '^digraph regex_graph {' "$LOG_ROOT/example-dot-$_mode.log"; then
        fail "example DOT ($_mode) has no digraph header"
        return 1
    fi
    if ! grep -q 'n0 -> n1' "$LOG_ROOT/example-dot-$_mode.log"; then
        fail "example DOT ($_mode) has no edges"
        return 1
    fi

    # Compile error must be propagated with a non-zero exit.
    printf 'a(' >"$LOG_ROOT/example-bad.txt"
    if run_logged "example-bad-$_mode" \
        "$_ex" --pattern-file "$LOG_ROOT/example-bad.txt" --format json; then
        fail "example accepted a bad pattern ($_mode)"
        return 1
    fi

    # Unsupported pattern must fail graph construction, not fake success.
    printf '(a)\\1' >"$LOG_ROOT/example-unsupported.txt"
    if run_logged "example-unsupported-$_mode" \
        "$_ex" --pattern-file "$LOG_ROOT/example-unsupported.txt" \
        --format json; then
        fail "example accepted an unsupported pattern ($_mode)"
        return 1
    fi
    if ! grep -q 'unsupported-opcode' \
        "$LOG_ROOT/example-unsupported-$_mode.log"; then
        fail "example did not report the unsupported opcode ($_mode)"
        return 1
    fi

    # Audit example: candidate with location, and coverage note for an
    # unsupported pattern.
    if ! run_logged "audit-example-$_mode" \
        "$_audit" --pattern '((?:a|b){2,4}){2,4}'; then
        fail "audit example ($_mode)"
        return 1
    fi
    if ! grep -q '"rule": "nested-repeat"' \
        "$LOG_ROOT/audit-example-$_mode.log"; then
        fail "audit example ($_mode) missed nested repeat"
        return 1
    fi
    if ! run_logged "audit-unsupported-$_mode" "$_audit" --pattern '(a)\1'; then
        fail "audit example failed on unsupported pattern ($_mode)"
        return 1
    fi
    if ! grep -q 'coverage-insufficient' \
        "$LOG_ROOT/audit-unsupported-$_mode.log"; then
        fail "audit example ($_mode) did not flag coverage gap"
        return 1
    fi

    note_pass "examples ($_mode)"
    return 0
}

# ---------------------------------------------------------------------
# Driver.
# ---------------------------------------------------------------------

log "check root: $ROOT"
log "compiler:  $CC_BIN"
log "logs:      $LOG_ROOT"

for MODE in direct switch; do
    if ! build_mode "$MODE"; then
        continue
    fi
    compile_program "$MODE" contract "$ROOT/test/regex_graph_contract.c" || continue
    compile_program "$MODE" determinism "$ROOT/test/regex_graph_determinism.c" || continue
    compile_program "$MODE" auditcontract "$ROOT/test/regex_graph_audit_contract.c" || continue
    compile_program "$MODE" example "$ROOT/examples/regex_graph.c" || continue
    compile_program "$MODE" audit "$ROOT/examples/regex_graph_audit.c" || continue

    run_contract "$MODE"
    run_audit_contract "$MODE"
    run_examples "$MODE"
done

# Only compare dispatch modes when both built completely.
if [ -x "$LOG_ROOT/bin-switch-determinism" ] && \
   [ -x "$LOG_ROOT/bin-direct-determinism" ]; then
    run_determinism
else
    fail "determinism comparison skipped: a build is missing"
fi

log "==== summary ===="
printf '%s\n' "$SUMMARY" >&2

if [ "$FAILURES" -ne 0 ]; then
    log "RESULT FAIL failures=$FAILURES"
    log "logs retained in $LOG_ROOT"
    exit 1
fi

log "RESULT PASS groups=4 modes=2"
log "logs retained in $LOG_ROOT"
exit 0
