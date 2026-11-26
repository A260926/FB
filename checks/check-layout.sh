#!/bin/sh
#
# Layout checks for explicit cbuffer : packoffset support.
#
# Builds and runs the standalone C++ layout/expression check, then runs the
# pre-existing baseline check (which also guards the old no-packoffset output).
#
set -eu

# Run from the repository root even when invoked from elsewhere.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR/.."

mkdir -p build

c++ -std=c++11 -O0 -Wall -Wno-deprecated-declarations -Isrc \
    checks/layout_check.cpp \
    src/CBufferLayout.cpp \
    src/CodeWriter.cpp \
    src/Engine.cpp \
    src/GLSLGenerator.cpp \
    src/HLSLGenerator.cpp \
    src/HLSLParser.cpp \
    src/HLSLTokenizer.cpp \
    src/HLSLTree.cpp \
    -o build/layout_check

./build/layout_check

bash checks/baseline.sh
