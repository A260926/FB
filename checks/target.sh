set -eu
cd "$(dirname "$0")/.."
mkdir -p build
test -f checks/audit_contract.c
cc -std=c99 -DNDEBUG -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -g -I. tlsf.c checks/audit_contract.c -o build/audit-contract
./build/audit-contract
bash checks/baseline.sh
