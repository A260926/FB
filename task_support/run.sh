#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if ! javac -version >/dev/null 2>&1; then
  for jdk_home in \
    /opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home \
    /usr/local/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home \
    /opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home \
    /Library/Java/JavaVirtualMachines/*/Contents/Home; do
    if [[ -x "$jdk_home/bin/javac" ]]; then
      export JAVA_HOME="$jdk_home"
      export PATH="$JAVA_HOME/bin:$PATH"
      break
    fi
  done
fi
mode=${1:-baseline}
bash task_support/build.sh
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOMODCACHE="$PWD/.task-build/go-mod-cache" GOCACHE="$PWD/.task-build/go-build-cache"
mkdir -p "$GOMODCACHE" "$GOCACHE"
if [[ $mode == target ]]; then
  test -s task_support/switch_regression_test.go
  output_log=$(mktemp "${TMPDIR:-/tmp}/jdcore-target.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -count=1 -v ./task_support -run '^(TestSwitchStackMerges|TestSwitchTypes|TestSwitchCompatibility)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestSwitchStackMerges TestSwitchTypes TestSwitchCompatibility; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
bash task_support/baseline.sh
