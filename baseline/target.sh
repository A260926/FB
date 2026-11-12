#!/usr/bin/env bash
#
# Line number program regression contract.
#
#   1. Builds the harness against an independent temp copy of the repo and runs
#      the line_contract test: the healthy reader must pass all 24 combinations
#      and print LINE_CONTRACT_CASES=24.
#   2. For each of the three source mutations in baseline/mutations, builds a
#      fresh temp copy, applies the patch, and runs the *same* test. A mutation
#      counts as detected only when the run exits non-zero *and* reports a
#      LINE_MODEL_MISMATCH failure. Compile breakage, missing tests, zero
#      matches or skipped tests are explicitly rejected.
#   3. Re-runs the healthy baseline and prints the final summary line.
#
# No network access is required: the harness has only an in-repo path
# dependency and cargo is invoked with --offline against an empty CARGO_HOME.
set -uo pipefail

BASELINE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$BASELINE_DIR/.." && pwd)"
HARNESS_DIR="$BASELINE_DIR/harness"
MUTATION_DIR="$BASELINE_DIR/mutations"
TEST_FILE="$HARNESS_DIR/tests/line_contract.rs"

CASES=24
MUTANTS=(carry fixed transient)

if [[ ! -f "$TEST_FILE" ]]; then
    echo "missing contract test: $TEST_FILE" >&2
    exit 1
fi
for mutant in "${MUTANTS[@]}"; do
    if [[ ! -f "$MUTATION_DIR/$mutant.patch" ]]; then
        echo "missing mutation patch: $MUTATION_DIR/$mutant.patch" >&2
        exit 1
    fi
done

# Create an isolated build root: a full copy of the repo as the gimli path
# dependency plus a copy of the harness whose Cargo.toml points at ../gimli.
make_workdir() {
    local work="$1"
    cp -R "$REPO_ROOT" "$work/gimli"
    rm -rf "$work/gimli/target" "$work/gimli/baseline/harness/target"
    mkdir -p "$work/harness"
    cp -R "$HARNESS_DIR/." "$work/harness/"
    rm -rf "$work/harness/target"
    mkdir -p "$work/cargo-home"
}

run_contract() {
    # $1: work dir. Prints combined output, returns the cargo exit status.
    local work="$1"
    CARGO_HOME="$work/cargo-home" cargo test \
        --offline \
        --manifest-path "$work/harness/Cargo.toml" \
        --test line_contract -- --nocapture 2>&1
}

# ---- Healthy baseline -----------------------------------------------------
BASE_WORK="$(mktemp -d "${TMPDIR:-/tmp}/line-contract-base.XXXXXX")"
trap 'rm -rf "$BASE_WORK"' EXIT
make_workdir "$BASE_WORK"

base_out="$(run_contract "$BASE_WORK")"
base_status=$?
if [[ $base_status -ne 0 ]]; then
    printf '%s\n' "$base_out" >&2
    echo "healthy baseline failed to build or run" >&2
    exit 1
fi
if ! grep -q "LINE_CONTRACT_CASES=$CASES" <<<"$base_out"; then
    printf '%s\n' "$base_out" >&2
    echo "baseline did not report LINE_CONTRACT_CASES=$CASES" >&2
    exit 1
fi
# A run that silently skipped everything must not count as a pass.
if grep -Eq "test result: ok\. 0 passed" <<<"$base_out"; then
    echo "baseline matched zero rows" >&2
    exit 1
fi
echo "LINE_CONTRACT_CASES=$CASES"

# ---- Mutations ------------------------------------------------------------
detected=0
for mutant in "${MUTANTS[@]}"; do
    mut_work="$(mktemp -d "${TMPDIR:-/tmp}/line-contract-$mutant.XXXXXX")"
    make_workdir "$mut_work"

    if ! patch -p1 -d "$mut_work/gimli" <"$MUTATION_DIR/$mutant.patch" >/dev/null; then
        rm -rf "$mut_work"
        echo "mutation $mutant: patch did not apply" >&2
        exit 1
    fi

    mut_out="$(run_contract "$mut_work")"
    mut_status=$?
    rm -rf "$mut_work"

    # Detection requires both a non-zero exit and the contract's own mismatch
    # marker. A pure compile failure (no LINE_MODEL_MISMATCH) is not accepted.
    if [[ $mut_status -eq 0 ]]; then
        printf '%s\n' "$mut_out" >&2
        echo "mutation $mutant: test unexpectedly passed" >&2
        exit 1
    fi
    if ! grep -q "LINE_MODEL_MISMATCH" <<<"$mut_out"; then
        printf '%s\n' "$mut_out" >&2
        echo "mutation $mutant: failed without LINE_MODEL_MISMATCH (compile error?)" >&2
        exit 1
    fi
    if grep -Eq "test result: ok" <<<"$mut_out"; then
        echo "mutation $mutant: reported a passing test result" >&2
        exit 1
    fi
    echo "mutation $mutant: detected via LINE_MODEL_MISMATCH"
    detected=$((detected + 1))
done

# ---- Plain baseline rerun -------------------------------------------------
rerun_work="$(mktemp -d "${TMPDIR:-/tmp}/line-contract-rerun.XXXXXX")"
make_workdir "$rerun_work"
rerun_out="$(run_contract "$rerun_work")"
rerun_status=$?
rm -rf "$rerun_work"
if [[ $rerun_status -ne 0 ]] || ! grep -q "LINE_CONTRACT_CASES=$CASES" <<<"$rerun_out"; then
    printf '%s\n' "$rerun_out" >&2
    echo "baseline rerun failed" >&2
    exit 1
fi

if [[ $detected -ne ${#MUTANTS[@]} ]]; then
    echo "only $detected of ${#MUTANTS[@]} mutations detected" >&2
    exit 1
fi

echo "GIMLI_LINE_CONTRACT_OK cases=$CASES mutants=$detected"
