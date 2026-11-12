#!/bin/bash
# Mutation regression for the new offset reference tests.
#
# Three independent temporary source copies each receive exactly one source change:
#   arc_metric      raw offset arc radius contribution direction is inverted
#   hole_direction  Shape cw (hole) loops are offset with the opposite sign
#   slice_filter    cross-loop slice distance filtering is restricted to the parent loop
#
# Each copy is built on its own first. Building a copy is a prerequisite, not a detection result:
# compile errors, crashes, timeouts and runs with zero executed tests all make this script fail.
# A mutation is only accepted as detected when the reference tests finish normally and reject it
# with a geometric numeric assertion (our OFFSET_REFERENCE_ASSERT marker plus numbers).
set -euo pipefail
cd "$(dirname "$0")/.."

repo_root=$PWD
work=$(mktemp -d "${TMPDIR:-/tmp}/cavalier-offset-mutants.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Override the parent CARGO_HOME/CARGO_TARGET_DIR so every copy resolves the vendored crates
# offline and gets its own target directory.
export CARGO_HOME="$work/cargo-home"
mkdir -p "$CARGO_HOME"

mutations=(arc_metric hole_direction slice_filter)

# ---------------------------------------------------------------------------------------------
# Copy the workspace (production crate, manifests, lock file, vendored sources, cargo config).
# ---------------------------------------------------------------------------------------------
prepare_copy() {
    local dest=$1
    mkdir -p "$dest"
    cp -a Cargo.toml Cargo.lock .cargo vendor cavalier_contours "$dest/"
}

# Replace exactly one occurrence of a literal string in a file; fail if that is not the case.
replace_once() {
    local file=$1 old=$2 new=$3
    local count
    count=$(MUT_OLD="$old" MUT_FILE="$file" perl -0777 -ne \
        'my $p = $ENV{MUT_OLD}; my $n = () = /\Q$p\E/g; print $n' "$file")
    if [[ $count != 1 ]]; then
        echo "mutation precondition failed: expected 1 occurrence of a literal in $file, found $count" >&2
        exit 1
    fi
    MUT_OLD="$old" MUT_NEW="$new" perl -0777 -pi -e \
        'my $old = $ENV{MUT_OLD}; my $new = $ENV{MUT_NEW}; s/\Q$old\E/$new/' "$file"
    if ! grep -Fq -- "$new" "$file"; then
        echo "mutation postcondition failed: replacement text missing in $file" >&2
        exit 1
    fi
}

apply_mutation() {
    local name=$1 dir=$2
    case $name in
    arc_metric)
        # raw_pline_offset: flip the sign the radius offset receives for ccw vs cw arcs.
        replace_once \
            "$dir/cavalier_contours/src/polyline/internal/raw_pline_offset.rs" \
            'let offset_direction = if is_clockwise { offset } else { -offset };' \
            'let offset_direction = if is_clockwise { -offset } else { offset };'
        ;;
    hole_direction)
        # shape_algorithms: only the cw (hole) loops receive a negated offset distance.
        replace_once \
            "$dir/cavalier_contours/src/shape_algorithms/mod.rs" \
            'for pline in &self.cw_plines {
            for offset_pline in pline.parallel_offset_for_shape(offset, options) {' \
            'for pline in &self.cw_plines {
            for offset_pline in pline.parallel_offset_for_shape(-offset, options) {'
        ;;
    slice_filter)
        # shape_algorithms: keep loop intersection and stitching, but check slice distance
        # against the parent loop instead of every non-parent loop.
        replace_once \
            "$dir/cavalier_contours/src/shape_algorithms/mod.rs" \
            '(0..(self.ccw_plines.len() + self.cw_plines.len())).filter(|i| *i != parent_idx)' \
            '(0..(self.ccw_plines.len() + self.cw_plines.len())).filter(|i| *i == parent_idx)'
        ;;
    *)
        echo "unknown mutation: $name" >&2
        exit 1
        ;;
    esac
}

# Run a command under a wall clock alarm so a hang is reported instead of blocking forever. The
# alarm survives the exec and its default action terminates the child, leaving no normal test
# summary for the caller to accept.
run_with_timeout() {
    local seconds=$1
    shift
    perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"
}

mutant_log="$work/mutant.log"
: >"$mutant_log"

for name in "${mutations[@]}"; do
    dir="$work/$name"
    prepare_copy "$dir"
    apply_mutation "$name" "$dir"

    echo "BUILDING_MUTANT $name"
    (
        cd "$dir"
        CARGO_TARGET_DIR="$dir/target" run_with_timeout 600 \
            cargo test --frozen -q -p cavalier_contours --test offset_reference --no-run
    ) >"$work/$name.build" 2>&1 || {
        echo "MUTANT_BUILD_FAILED $name" >&2
        cat "$work/$name.build" >&2
        exit 1
    }

    # The built binary must list the three required tests before it is trusted to run them.
    (
        cd "$dir"
        CARGO_TARGET_DIR="$dir/target" run_with_timeout 120 \
            cargo test --frozen -q -p cavalier_contours --test offset_reference -- --list
    ) >"$work/$name.list" 2>&1 || {
        echo "MUTANT_RUN_INVALID $name (test binary did not list tests)" >&2
        cat "$work/$name.list" >&2
        exit 1
    }
    for required in offset_metric_reference offset_topology_reference offset_transform_reference; do
        if ! grep -Fq "$required: test" "$work/$name.list"; then
            echo "MUTANT_RUN_INVALID $name (required test missing from binary: $required)" >&2
            cat "$work/$name.list" >&2
            exit 1
        fi
    done

    echo "RUNNING_MUTANT $name"
    (
        cd "$dir"
        CARGO_TARGET_DIR="$dir/target" run_with_timeout 600 \
            cargo test --frozen -q -p cavalier_contours --test offset_reference -- \
            --show-output --test-threads=1
    ) >"$work/$name.run" 2>&1 || true

    cat "$work/$name.run" >>"$mutant_log"

    # The run must have executed tests and reported a normal failing summary. A compile error,
    # crash/abort, timeout or zero executed cases leaves no such summary and fails the script.
    summary=$(sed -nE 's/^test result: FAILED\. ([0-9]+) passed; ([0-9]+) failed; ([0-9]+) ignored;.*$/\1 \2 \3/p' \
        "$work/$name.run" | tail -n 1)
    if [[ -z $summary ]]; then
        echo "MUTANT_RUN_INVALID $name (no normal failing test summary; crash, timeout, compile failure or zero cases)" >&2
        tail -n 40 "$work/$name.run" >&2
        exit 1
    fi
    read -r passed failed ignored <<<"$summary"
    if ((failed < 1 || passed + failed < 1)); then
        echo "MUTANT_RUN_INVALID $name (passed=$passed failed=$failed ignored=$ignored)" >&2
        tail -n 40 "$work/$name.run" >&2
        exit 1
    fi

    # Only a geometric numeric assertion counts as detection.
    if ! grep -Fq 'OFFSET_REFERENCE_ASSERT' "$work/$name.run"; then
        echo "MUTANT_NOT_DETECTED $name (no geometric numeric assertion in output)" >&2
        exit 1
    fi

    first_message=$(grep -F 'OFFSET_REFERENCE_ASSERT' "$work/$name.run" | head -n 1)
    echo "MUTANT_REJECTED $name assertion"
    echo "FIRST_FAILURE $name: $first_message"
done
