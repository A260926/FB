#!/usr/bin/env bash
#
# Verification for the regional film grain entry point dav1d_apply_grain_region().
#
# Builds dav1d (meson/ninja when available, otherwise a direct-cc no-asm build
# of the same sources) in a temporary directory, compiles tests/grain_region.c
# against the static library, and runs it on both embedded fixtures. The test
# prints the number of scenarios and compared pixels and a GRAIN_REGION_PASS
# marker; a missing test binary, an empty run or any mismatch exits non-zero.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

b="$(mktemp -d "${TMPDIR:-/tmp}/dav1d-grain-region.XXXXXX")"
trap 'rm -rf "$b"' EXIT

cc="${CC:-cc}"
cflags=(-std=c11 -O1 -DNDEBUG -Wall -Wextra -Wno-unused-parameter)
incs=(-I"$b" -I"$b/include" -I"$root" -I"$root/include" -I"$root/tests")
# The test interposes on malloc via dlsym(RTLD_NEXT, ...); macOS ships that in
# libSystem, glibc keeps it in libdl.
libs=(-lpthread -lm)
[ "$(uname -s)" = "Linux" ] && libs+=(-ldl)

# Locate meson/ninja: prefer the pinned .build-tools, then PATH.
meson_bin=""
ninja_bin=""
if [ -x "$root/.build-tools/bin/meson" ]; then
    meson_bin="$root/.build-tools/bin/meson"
    ninja_bin="$root/.build-tools/bin/ninja"
    export PATH="$root/.build-tools/bin:$PATH"
elif command -v meson >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1; then
    meson_bin="$(command -v meson)"
    ninja_bin="$(command -v ninja)"
fi

if [ -n "$meson_bin" ]; then
    echo "building with meson: $meson_bin"
    "$meson_bin" setup "$b/build" --buildtype=release --default-library=static \
        -Denable_asm=false -Denable_tools=false -Denable_tests=true \
        -Denable_examples=false -Dtrim_dsp=false >"$b/config.log" 2>&1 || {
        cat "$b/config.log"; exit 1; }
    "$ninja_bin" -C "$b/build" -j4 grain_region >"$b/build.log" 2>&1 || {
        cat "$b/build.log"; exit 1; }
    grain_region_bin="$b/build/tests/grain_region"
else
    echo "meson/ninja not found; building the same C sources directly with $cc"
    plain="cdf cpu ctx data decode dequant_tables getbits intra_edge itx_1d
           lf_mask lib log mem msac obu pal picture qm ref refmvs scan tables
           thread_task warpmv wedge"
    tmpl="cdef_apply_tmpl cdef_tmpl fg_apply_tmpl filmgrain_tmpl
          ipred_prepare_tmpl ipred_tmpl itx_tmpl lf_apply_tmpl loopfilter_tmpl
          looprestoration_tmpl lr_apply_tmpl mc_tmpl recon_tmpl"

    # Select the host architecture macros the same way meson.build would; the
    # C-only fallback disables all assembly regardless of the host.
    arch_common=(-DARCH_PPC64LE=0 -DARCH_RISCV=0 -DARCH_RV32=0 -DARCH_RV64=0
                 -DARCH_LOONGARCH=0 -DARCH_LOONGARCH32=0 -DARCH_LOONGARCH64=0)
    case "$(uname -m)" in
        arm64|aarch64) arch=("${arch_common[@]}" -DARCH_AARCH64=1 -DARCH_ARM=0
                             -DARCH_X86=0 -DARCH_X86_64=0 -DARCH_X86_32=0);;
        x86_64|amd64)  arch=("${arch_common[@]}" -DARCH_AARCH64=0 -DARCH_ARM=0
                             -DARCH_X86=1 -DARCH_X86_64=1 -DARCH_X86_32=0);;
        *)             arch=("${arch_common[@]}" -DARCH_AARCH64=0 -DARCH_ARM=0
                             -DARCH_X86=0 -DARCH_X86_64=0 -DARCH_X86_32=0);;
    esac

    cat >"$b/config.h" <<'EOF'
#define CONFIG_8BPC 1
#define CONFIG_16BPC 1
#define CONFIG_ASM_DEBUG 0
#define CONFIG_LOG 1
#define TRIM_DSP_FUNCTIONS 0
#define HAVE_ASM 0
#define HAVE_AS_FUNC 0
#define HAVE_AS_ARCH_DIRECTIVE 0
#define HAVE_DOTPROD 0
#define HAVE_I8MM 0
#define HAVE_SVE 0
#define HAVE_SVE2 0
#define HAVE_GETAUXVAL 0
#define HAVE_ELF_AUX_INFO 0
#define HAVE_CLOCK_GETTIME 1
#define HAVE_SIGACTION 1
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_MEMALIGN 0
#define HAVE_ALIGNED_ALLOC 1
#define HAVE_DLSYM 0
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_IO_H 0
#define HAVE_PTHREAD_NP_H 0
#define HAVE_PTHREAD_GETAFFINITY_NP 0
#define HAVE_PTHREAD_SETAFFINITY_NP 0
#define HAVE_PTHREAD_SETNAME_NP 0
#define HAVE_PTHREAD_SET_NAME_NP 0
#define HAVE_C11_GENERIC 1
#define ENDIANNESS_BIG 0
#define PREFIX 1
EOF
    echo '#define DAV1D_VERSION "verification-direct-build"' >"$b/vcs_version.h"

    objs=()
    mkdir -p "$b/obj"
    for s in $plain; do
        "$cc" "${cflags[@]}" "${arch[@]}" "${incs[@]}" \
             -c "$root/src/$s.c" -o "$b/obj/$s.o"
        objs+=("$b/obj/$s.o")
    done
    for s in $tmpl; do
        "$cc" "${cflags[@]}" "${arch[@]}" "${incs[@]}" -DBITDEPTH=8 \
             -c "$root/src/$s.c" -o "$b/obj/${s}_8.o"
        "$cc" "${cflags[@]}" "${arch[@]}" "${incs[@]}" -DBITDEPTH=16 \
             -c "$root/src/$s.c" -o "$b/obj/${s}_16.o"
        objs+=("$b/obj/${s}_8.o" "$b/obj/${s}_16.o")
    done
    ar rcs "$b/libdav1d-direct.a" "${objs[@]}"
    static_lib="$b/libdav1d-direct.a"

    [ -f "$static_lib" ] || {
        echo "missing static library $static_lib" >&2; exit 1; }
    "$cc" "${cflags[@]}" "${incs[@]}" "$root/tests/grain_region.c" \
         "$static_lib" "${libs[@]}" -o "$b/grain_region"
    grain_region_bin="$b/grain_region"
fi

[ -f "$root/tests/grain_region.c" ] || {
    echo "missing tests/grain_region.c" >&2; exit 1; }
[ -x "$grain_region_bin" ] || {
    echo "missing test binary $grain_region_bin" >&2; exit 1; }

total_scenarios=0
total_pixels=0
for spec in "verification/fixtures/420-8.ivf 8" "verification/fixtures/420-10.ivf 10"; do
    set -- $spec
    fixture="$root/$1"; bpc="$2"
    [ -f "$fixture" ] || { echo "missing fixture $fixture" >&2; exit 1; }
    out="$("$grain_region_bin" "$fixture" "$bpc")"
    echo "$out"
    echo "$out" | grep -q GRAIN_REGION_PASS || {
        echo "region test failed for $fixture" >&2; exit 1; }
    sc="$(printf '%s' "$out" | sed -n 's/.*scenarios=\([0-9][0-9]*\).*/\1/p')"
    px="$(printf '%s' "$out" | sed -n 's/.*pixels_compared=\([0-9][0-9]*\).*/\1/p')"
    [ -n "$sc" ] && [ "$sc" -gt 0 ] || {
        echo "no scenarios reported for $fixture" >&2; exit 1; }
    [ -n "$px" ] && [ "$px" -gt 0 ] || {
        echo "no pixel comparisons reported for $fixture" >&2; exit 1; }
    total_scenarios=$((total_scenarios + sc))
    total_pixels=$((total_pixels + px))
done

[ "$total_scenarios" -gt 0 ] || { echo "zero scenarios ran" >&2; exit 1; }
[ "$total_pixels" -gt 0 ] || { echo "zero pixels compared" >&2; exit 1; }
echo "grain region totals: scenarios=$total_scenarios " \
     "pixels_compared=$total_pixels GRAIN_REGION_PASS"
