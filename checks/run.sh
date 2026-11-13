#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=.portal-build
mkdir -p "$build_dir"
case "$(uname -s)" in Darwin) dead_strip=-Wl,-dead_strip;; *) dead_strip=-Wl,--gc-sections;; esac
compile() {
    output=$1; shift
    "${CC:-cc}" -std=c99 -O1 -g -ffunction-sections -fdata-sections -I. -Ichecks \
       renderer/core/*.c renderer/shaders/*.c renderer/scenes/*.c \
       renderer/tests/test_helper.c "$@" "$dead_strip" -lm -o "$output"
}
build() { compile "$build_dir/baseline" checks/baseline.c; }
baseline() { "$build_dir/baseline"; }
case "${1:-}" in
 build) build;;
 baseline) build; baseline;;
 target)
    test -s tests/portal_contract.c || { echo 'missing tests/portal_contract.c' >&2; exit 11; }
    test -s examples/portal_preview.c || { echo 'missing examples/portal_preview.c' >&2; exit 12; }
    build; baseline
    compile "$build_dir/target" checks/contract_main.c tests/portal_contract.c
    status=0
    "$build_dir/target" >"$build_dir/target.stdout" 2>"$build_dir/target.stderr" || status=$?
    cat "$build_dir/target.stdout"; cat "$build_dir/target.stderr" >&2
    test "$status" -eq 0 || exit "$status"
    awk '
      /^CASE_START / { starts++ }
      /^CASE_PASS [^ ]+ [^ ]+ checks=[1-9][0-9]*$/ { passes++; split($4,a,"="); observed_checks+=a[2] }
      /^GROUP_PASS (view-mapping|clip-and-occlusion|recursion-and-state) cases=[1-9][0-9]* checks=[1-9][0-9]*$/ {
        if(seen[$2]++) bad=1; split($3,a,"="); split($4,b,"="); groups++; cases+=a[2]; checks+=b[2]
      }
      /^PORTAL_TARGET_COMPLETE groups=3 cases=[1-9][0-9]* checks=[1-9][0-9]*$/ {
        complete++; split($3,a,"="); split($4,b,"="); final_cases=a[2]; final_checks=b[2]
      }
      END { if(bad || groups!=3 || complete!=1 || cases!=passes || starts!=passes || cases!=final_cases || checks!=final_checks || checks!=observed_checks) exit 21 }
    ' "$build_dir/target.stdout"
    compile "$build_dir/preview" examples/portal_preview.c
    mkdir -p "$build_dir/images"
    rm -f "$build_dir/images/depth0.ppm" "$build_dir/images/depth1.ppm" "$build_dir/images/depth2.ppm"
    "$build_dir/preview" "$build_dir/images"
    for name in depth0 depth1 depth2; do
      file="$build_dir/images/$name.ppm"
      test -s "$file" && test "$(head -c 2 "$file")" = P6 && test "$(wc -c < "$file")" -gt 12 || exit 22
    done
    echo PORTAL_DEMO_COMPLETE images=3;;
 *) echo 'usage: sh checks/run.sh build|baseline|target' >&2; exit 2;;
esac
