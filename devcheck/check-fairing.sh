#!/bin/bash
# devcheck/check-fairing.sh
#
# Compiles devcheck/fairing.cpp and drives each scenario group explicitly,
# re-verifying the per-group PASS lines and the final CINOLIB_FAIRING_OK
# token. It then runs the same executable with no arguments, so the "self
# driven" entry point and the shell entry point use exactly the same code.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"

b=$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/cinolib-fairing-check.$$")
trap 'rm -rf "$b"' EXIT
mkdir -p "$b"

binary="$b/fairing"
c++ -std=c++17 -O1 -I"$root/include" -I"$root/external/eigen" "$here/fairing.cpp" -o "$binary"

# Run each group separately. The executable itself prints its PASS line with
# the real number of driven scenarios; the shell script only verifies that the
# group actually ran, that it reported cases>0, and that it produced the final
# token. This way the shell entry point really executes the compiled code
# rather than trusting a static log.
run_group() {
    local tag="$1"
    local log="$b/$tag.log"
    if ! "$binary" "$tag" >"$log" 2>&1; then
        cat "$log"
        return 1
    fi
    cat "$log"
    grep -Eq "^PASS ${tag} cases=[0-9]+$" "$log"
    grep -Eq "^PASS ${tag} cases=[1-9][0-9]*$" "$log"
    grep -Fxq CINOLIB_FAIRING_OK "$log"
    if grep -q "FAILED" "$log"; then
        return 1
    fi
}

for tag in constrained-descent component-volumes geometry-state stopping-errors; do
    run_group "$tag"
done

# direct, self-driven run: must print the same groups and the final token
direct="$b/direct.log"
"$binary" >"$direct" 2>&1
cat "$direct"
for tag in constrained-descent component-volumes geometry-state stopping-errors; do
    grep -Eq "^PASS ${tag} cases=[1-9][0-9]*$" "$direct"
done
grep -Fxq CINOLIB_FAIRING_OK "$direct"
if grep -q "FAILED" "$direct"; then
    exit 1
fi

printf 'CINOLIB_FAIRING_OK\n'
