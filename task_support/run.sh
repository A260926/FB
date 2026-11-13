#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
case "${1:-baseline}" in
 baseline) c++ -std=c++17 -O0 -pthread -I. task_support/baseline.cpp -o .task-build/baseline; exec .task-build/baseline;;
 target) for f in linked_collection.h linked_collection.cpp linked_collection_test.cpp; do test -s "$f" || { echo "missing or empty $f" >&2; exit 2; }; done
 c++ -std=c++17 -O0 -pthread -I. task_support/target_main.cpp linked_collection.cpp linked_collection_test.cpp -o .task-build/target
 .task-build/target
 sh task_support/run.sh baseline;;
 *) exit 2;;
esac
