#!/bin/bash
set -euo pipefail
mkdir -p .build
test -s devcheck/dialog_contract.test.cjs
bash devcheck/node.sh --test --test-reporter=tap devcheck/dialog_contract.test.cjs | tee .build/dialog-contract.tap
for name in one_registration typed_priority reentrant_consumption regex_state callback_error; do
 grep -Eq "^ok [0-9]+ - ${name}$" .build/dialog-contract.tap
done
