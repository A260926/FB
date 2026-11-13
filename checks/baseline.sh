set -eu
cd "$(dirname "$0")/.."
cc -std=c11 -pthread -Iinclude checks/baseline.c build-checks/libmimalloc.a -o build-checks/baseline
./build-checks/baseline
