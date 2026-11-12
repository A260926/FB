#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -f checks/nc_contract.c || { echo 'missing checks/nc_contract.c' >&2; exit 2; }
sh checks/install.sh
"${CC:-cc}" -std=c99 -Wall -Wextra -I inc -I checks checks/nc_contract.c checks/contract_main.c libbearssl.a -o .check-build/nc-contract
./.check-build/nc-contract > .check-build/nc-contract.log 2>&1 || { cat .check-build/nc-contract.log; exit 1; }
cat .check-build/nc-contract.log
grep -q '^NC_CONTRACT_COMPLETE cases=[1-9][0-9]* assertions=[1-9][0-9]* der=[1-9][0-9]* chain=[1-9][0-9]* workspace=[1-9][0-9]*$' .check-build/nc-contract.log
sh checks/old.sh
printf '%s\n' NC_TARGET_OK
