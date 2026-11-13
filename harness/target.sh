#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s tests/ondemand/fork_contract.cpp
bash harness/build.sh
c++ -std=c++17 -O1 -Iinclude tests/ondemand/fork_contract.cpp .build/simdjson.o -pthread -o .build/fork_contract
.build/fork_contract
c++ -std=c++17 -O1 -g -DSIMDJSON_DEVELOPMENT_CHECKS=1 -Iinclude -Isrc -c src/simdjson.cpp -o .build/simdjson_checks.o
c++ -std=c++17 -O1 -DSIMDJSON_DEVELOPMENT_CHECKS=1 -Iinclude tests/ondemand/fork_contract.cpp .build/simdjson_checks.o -pthread -o .build/fork_contract_checks
.build/fork_contract_checks
c++ -std=c++17 -O1 -Iinclude harness/baseline.cpp .build/simdjson.o -pthread -o .build/baseline
.build/baseline
