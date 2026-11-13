#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s test/lexbor/selectors/lang_contract.c
cmake -S . -B .build -DLEXBOR_BUILD_SHARED=OFF -DLEXBOR_BUILD_STATIC=ON -DLEXBOR_BUILD_EXAMPLES=OFF -DLEXBOR_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build .build --parallel 4
cc -std=c99 -Wall -Wextra -Isource test/lexbor/selectors/lang_contract.c .build/liblexbor_static.a -lm -o .build/lang_contract
.build/lang_contract
ctest --test-dir .build -R selectors --output-on-failure --no-tests=error
