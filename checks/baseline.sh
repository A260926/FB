set -eu
cd "$(dirname "$0")/.."
mkdir -p build-checks
cc -std=c99 -I. checks/baseline.c libdmalloc.a -o build-checks/baseline
DMALLOC_OPTIONS= ./build-checks/baseline
