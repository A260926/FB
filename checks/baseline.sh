set -eu
cd "$(dirname "$0")/.."
c++ -std=c++11 -fsanitize=address -fno-omit-frame-pointer -Iinclude -Ibuild-checks/src checks/baseline.cpp build-checks/src/libfoonathan_memory-0.7.4.a -o build-checks/baseline
./build-checks/baseline
