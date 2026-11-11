#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build
c++ -std=c++11 -O0 -Isrc checks/layer_contract.cpp -o .build/layer_contract
.build/layer_contract
