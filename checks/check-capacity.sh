#!/bin/bash
# Builds the Voro++ library (including the capacity solver) and then
# compiles and runs the capacity regression test.
set -e
cd "$(dirname "$0")/.."

make -C src libvoro++.a

${CXX:-g++} -Wall -ansi -pedantic -O3 -Isrc \
	-o tests/capacity_test tests/capacity_test.cc -Lsrc -lvoro++

./tests/capacity_test
