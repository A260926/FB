set -eu
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=c99 -Wall -Wextra -I. tlsf.c checks/baseline.c -o build/baseline
./build/baseline
