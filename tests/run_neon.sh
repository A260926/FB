#!/usr/bin/env bash
#
# Build and run the AArch64 NEON QMF regression suite.
#
# This script never treats "NEON unsupported" as success: on a non-AArch64
# host it fails. On AArch64 it builds both implementations (the scalar one
# with APTX_FORCE_SCALAR and renamed symbols), runs the kernel level and
# public API tests through the real NEON path, records the actual NEON
# kernel call counts and a disassembly proving widening multiply-add
# instructions are emitted, and finally reports a timing comparison.
#
# Zero test cases, a scalar-only build, missing sources or any byte
# difference make the script exit non-zero. On full success it prints:
#   OPENAPTX_NEON_OK cases=6

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/openaptx-neon.XXXXXX")
trap 'rm -rf "$build"' EXIT

cc=${CC:-cc}
common=(-std=c99 -UNDEBUG -I"$root")
opt=(-O3)

log() { printf '%s\n' "$*"; }
fail() { printf 'run_neon: FAIL: %s\n' "$*" >&2; exit 1; }

# --- 1. The host really must be AArch64 and the compiler must emit NEON. ---
arch=$(uname -m)
[ "$arch" = "arm64" ] || [ "$arch" = "aarch64" ] || \
    fail "host arch '$arch' is not AArch64; NEON kernels cannot be validated here"

cat >"$build/probe.c" <<'EOF'
#include <arm_neon.h>
int main(void) {
    int32x4_t a = vdupq_n_s32(7), b = vdupq_n_s32(9);
    int64x2_t c = vmovl_s32(vget_low_s32(a));
    c = vmlal_s32(c, vget_low_s32(a), vget_low_s32(b));
    return (int)(vgetq_lane_s64(c, 0) == 70 ? 0 : 1);
}
EOF
"$cc" "${common[@]}" "${opt[@]}" "$build/probe.c" -o "$build/probe" || \
    fail "cannot compile AArch64 NEON probe"
"$build/probe" || fail "NEON probe did not execute correctly"
log "run_neon: AArch64 NEON probe executed successfully"

# --- 2. Required sources must exist. ---
for f in openaptx.c qmf_neon.c openaptx_internal.h openaptx.h \
         tests/qmf_kernel.c tests/qmf_stream.c tests/qmf_bench.c tests/qmf_ref.h; do
    [ -f "$root/$f" ] || fail "missing source file $f"
done

# Symbol renames so the scalar reference library can be linked together with
# the NEON library in one process without duplicate global symbols.
renames=(
    -Daptx_init=aptx_init_ref
    -Daptx_reset=aptx_reset_ref
    -Daptx_finish=aptx_finish_ref
    -Daptx_encode=aptx_encode_ref
    -Daptx_encode_finish=aptx_encode_finish_ref
    -Daptx_decode=aptx_decode_ref
    -Daptx_decode_sync=aptx_decode_sync_ref
    -Daptx_decode_sync_finish=aptx_decode_sync_finish_ref
    -Daptx_major=aptx_major_ref
    -Daptx_minor=aptx_minor_ref
    -Daptx_patch=aptx_patch_ref
    -Daptx_qmf_outer_coeffs=aptx_qmf_outer_coeffs_ref
    -Daptx_qmf_inner_coeffs=aptx_qmf_inner_coeffs_ref
)

# --- 3. Scalar reference (APTX_FORCE_SCALAR), renamed symbols. ---
"$cc" "${common[@]}" "${opt[@]}" -DAPTX_FORCE_SCALAR "${renames[@]}" \
    -c "$root/openaptx.c" -o "$build/openaptx_ref.o"

# --- 4. NEON library, default kernel selection. ---
"$cc" "${common[@]}" "${opt[@]}" -c "$root/openaptx.c" -o "$build/openaptx_neon.o"
"$cc" "${common[@]}" "${opt[@]}" -c "$root/qmf_neon.c"  -o "$build/qmf_neon.o"

# The NEON tree symbols must be defined and referenced by the codec object.
nm "$build/qmf_neon.o" | grep -q ' _aptx_qmf_tree_analysis_stereo_neon$' || \
    fail "NEON analysis kernel symbol missing"
nm "$build/qmf_neon.o" | grep -q ' _aptx_qmf_tree_synthesis_stereo_neon$' || \
    fail "NEON synthesis kernel symbol missing"
nm "$build/openaptx_neon.o" | grep -q ' _aptx_qmf_tree_analysis_stereo_neon$' || \
    fail "codec object does not call the NEON analysis kernel"
nm "$build/openaptx_neon.o" | grep -q ' _aptx_qmf_tree_synthesis_stereo_neon$' || \
    fail "codec object does not call the NEON synthesis kernel"

# A scalar-only build must still compile (other platforms / forced compare).
"$cc" "${common[@]}" -DAPTX_FORCE_SCALAR -c "$root/openaptx.c" -o "$build/scalar_check.o"
"$cc" "${common[@]}" -DAPTX_FORCE_SCALAR -c "$root/qmf_neon.c"  -o "$build/qmf_scalar_check.o"

# --- 5. Disassembly evidence: real 64bit widening multiply-adds. ---
disasm="$build/qmf_neon.disasm"
if command -v otool >/dev/null 2>&1; then
    # otool may emit a harmless xcrun cache warning on sandboxed hosts; the
    # disassembly itself is still written to stdout, so discard stderr.
    otool -tV "$build/qmf_neon.o" 2>/dev/null >"$disasm"
else
    objdump -d "$build/qmf_neon.o" >"$disasm"
fi
smlal_count=$(grep -c -E 'smlal2?\.|smlal2? ' "$disasm" || true)
addp_count=$(grep -c -E 'addp\.2d' "$disasm" || true)
[ "$smlal_count" -gt 0 ] || fail "no widening multiply-add (smlal) found in NEON kernel disassembly"
[ "$addp_count" -gt 0 ] || fail "no 64bit horizontal reduction (addp.2d) found in NEON kernel disassembly"
log "run_neon: disassembly saved to $disasm"
log "run_neon: smlal/smlal2 instructions=$smlal_count  addp.2d reductions=$addp_count"
grep -E 'smlal2?\.2d' "$disasm" | head -4 | sed 's/^/run_neon:   /'

# --- 6. Kernel level test (independent scalar oracle, six named cases). ---
"$cc" "${common[@]}" "${opt[@]}" "$root/tests/qmf_kernel.c" \
    "$build/openaptx_neon.o" "$build/qmf_neon.o" -o "$build/qmf_kernel"
kernel_out=$("$build/qmf_kernel")
printf '%s\n' "$kernel_out" | sed 's/^/run_neon: [kernel] /'
kernel_cases=$(printf '%s\n' "$kernel_out" | grep -c ': PASS$' || true)
[ "$kernel_cases" -ge 6 ] || fail "qmf_kernel ran only $kernel_cases named cases (need 6)"
kernel_calls=$(printf '%s\n' "$kernel_out" | sed -n 's/.*neon_kernel_calls=\([0-9][0-9]*\).*/\1/p')
[ -n "${kernel_calls:-}" ] && [ "$kernel_calls" -gt 0 ] || \
    fail "qmf_kernel did not record real NEON kernel calls"

# --- 7. Public API lockstep test (scalar renamed symbols vs NEON). ---
"$cc" "${common[@]}" "${opt[@]}" "$root/tests/qmf_stream.c" \
    "$build/openaptx_neon.o" "$build/qmf_neon.o" "$build/openaptx_ref.o" \
    -o "$build/qmf_stream"
stream_out=$("$build/qmf_stream")
printf '%s\n' "$stream_out" | sed 's/^/run_neon: [stream] /'
stream_cases=$(printf '%s\n' "$stream_out" | grep -c ': PASS' || true)
[ "$stream_cases" -ge 1 ] || fail "qmf_stream ran no passing scenarios"
printf '%s\n' "$stream_out" | grep -q 'neon_kernel_calls_delta=[1-9]' || \
    fail "public API path never executed the NEON kernels"
if printf '%s\n' "$stream_out" | grep -q ' FAIL'; then
    fail "qmf_stream reported a failure"
fi

# --- 8. Timing comparison, same compiler/-O3/input/warmup, medians. ---
"$cc" "${common[@]}" "${opt[@]}" "$root/tests/qmf_bench.c" \
    "$build/openaptx_neon.o" "$build/qmf_neon.o" "$build/openaptx_ref.o" \
    -o "$build/qmf_bench"
"$build/qmf_bench" | sed 's/^/run_neon: [bench] /'

# Keep a copy of the evidence next to the test directory for manual review.
evidence="$root/tests/neon_evidence"
mkdir -p "$evidence"
cp "$disasm" "$evidence/qmf_neon.disasm"
{
    printf 'kernel named cases: %s\n' "$kernel_cases"
    printf 'kernel NEON calls : %s\n' "$kernel_calls"
    printf 'stream scenarios  : %s\n' "$stream_cases"
    printf 'smlal/smlal2      : %s\n' "$smlal_count"
    printf 'addp.2d           : %s\n' "$addp_count"
} >"$evidence/summary.txt"
log "run_neon: evidence written to $evidence"

log "OPENAPTX_NEON_OK cases=6"
