set -eu
cd "$(dirname "$0")/.."
mkdir -p build-checks
cc -std=c11 -O1 -pthread -Irpmalloc rpmalloc/rpmalloc.c checks/baseline.c -o build-checks/baseline
./build-checks/baseline
