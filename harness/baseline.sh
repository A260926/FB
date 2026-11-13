#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash harness/build.sh
cc -std=c99 -Isrc harness/baseline.c .build/libgumbo.a -o .build/baseline
.build/baseline
c++ -std=c++11 -DGTEST_USE_OWN_TR1_TUPLE=1 -I. -Isrc -Ithird_party/gtest -Ithird_party/gtest/include tests/parser.cc tests/test_utils.cc third_party/gtest/src/gtest-all.cc third_party/gtest/src/gtest_main.cc .build/libgumbo.a -pthread -o .build/upstream_parser
.build/upstream_parser --gtest_filter='GumboParserTest.Fragment*:GumboParserTest.*Form*:GumboParserTest.*Math*'
