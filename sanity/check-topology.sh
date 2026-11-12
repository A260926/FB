#!/bin/bash
# Compiles and runs the topology-preservation sanity test
# (sanity/topology.cpp). The test program itself prints the
# grouped PASS lines and FQMS_TOPOLOGY_OK.
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -I. sanity/topology.cpp -o "$b/topology"
"$b/topology"
