#!/bin/sh
set -eu
mkdir -p build
c++ -std=c++11 -O0 -Isrc checks/baseline.cpp src/CodeWriter.cpp src/Engine.cpp src/GLSLGenerator.cpp src/HLSLGenerator.cpp src/HLSLParser.cpp src/HLSLTokenizer.cpp src/HLSLTree.cpp -o build/baseline
./build/baseline
