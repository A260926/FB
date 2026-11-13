set -eu
cd "$(dirname "$0")/.."
bash checks/install.sh
test -f test/guard_orientation.c
cc -std=c11 -pthread -Iinclude test/guard_orientation.c build-checks/libmimalloc.a -o build-checks/guard-orientation
./build-checks/guard-orientation
bash checks/baseline.sh
