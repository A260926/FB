set -eu
cd "$(dirname "$0")/.."
test -f test/freelist_guard_contract.c
test -f checks/guard.sh
bash checks/guard.sh
bash checks/baseline.sh
