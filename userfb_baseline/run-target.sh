#!/usr/bin/env bash
set -euo pipefail
test -s test/userfb_ctu_parameter_index.sh || { echo 'missing required new test: test/userfb_ctu_parameter_index.sh' >&2; exit 1; }
sh userfb_baseline/build.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for method in ctuParameterIndexUserFB ctuParameterCorrelationUserFB ctuParameterXmlUserFB; do
    name="TestBufferOverrun::$method"
    .userfb-build/bin/testrunner "$name" 2>&1 | tee "$work/test.log"
    grep -Fxq "$name" "$work/test.log"
    grep -Eq '^Number of tests: 1$' "$work/test.log"
    grep -Eq '^Number of todos: 0$' "$work/test.log"
    grep -Eq '^Tests failed: 0$' "$work/test.log"
done
bash userfb_baseline/run-old-tests.sh
bash test/userfb_ctu_parameter_index.sh .userfb-build/bin/cppcheck | tee "$work/cli.log"
grep -Fxq 'CTU_PARAMETER_INDEX_XML_OK' "$work/cli.log"
printf '%s\n' 'CTU_PARAMETER_INDEX_TARGET_OK'
