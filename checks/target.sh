set -eu
cd "$(dirname "$0")/.."
bash checks/install.sh
test -f checks/strict_contract.c
cc -std=c99 -I. checks/strict_contract.c libdmalloc.a -o build-checks/strict-contract
DMALLOC_OPTIONS= ./build-checks/strict-contract
bash checks/baseline.sh
