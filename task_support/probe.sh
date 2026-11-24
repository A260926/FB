#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
rm -f .task-build/identity-good.log .task-build/identity-error.log .task-build/identity-zero.log
./sparse -Wcontext-identity task_support/fixtures/healthy.c > .task-build/identity-good.log 2>&1
if test -s .task-build/identity-good.log; then cat .task-build/identity-good.log; echo IDENTITY_PROBE_HEALTHY_FAILED; exit 41; fi
./sparse -Wcontext-identity task_support/fixtures/error_return.c > .task-build/identity-error.log 2>&1
# Two distinct resources must survive the scalar cancellation and the error return.
if ! grep -F 'resource context' .task-build/identity-error.log; then cat .task-build/identity-error.log; echo IDENTITY_PROBE_DIAGNOSTIC_MISSING; exit 42; fi
grep -F 'error_path' .task-build/identity-error.log
grep -Eq '(^|[^[:alnum:]_])a([^[:alnum:]_]|$)' .task-build/identity-error.log
grep -Eq '(^|[^[:alnum:]_])b([^[:alnum:]_]|$)' .task-build/identity-error.log
./sparse -Wcontext-identity task_support/fixtures/zero_net.c > .task-build/identity-zero.log 2>&1
if ! grep -F 'resource context' .task-build/identity-zero.log; then cat .task-build/identity-zero.log; echo IDENTITY_PROBE_INPUT_OBLIGATION_MISSING; exit 43; fi
grep -F 'zero_net' .task-build/identity-zero.log
echo RESOURCE_IDENTITY_PROBE_OK
