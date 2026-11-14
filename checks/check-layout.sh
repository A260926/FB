#!/bin/sh
set -eu
mkdir -p build
c++ -std=c++11 -O0 -Isrc checks/check-layout.cpp src/CodeWriter.cpp src/ConstantBufferLayout.cpp src/Engine.cpp src/GLSLGenerator.cpp src/HLSLGenerator.cpp src/HLSLParser.cpp src/HLSLTokenizer.cpp src/HLSLTree.cpp src/MSLGenerator.cpp -o build/check-layout
./build/check-layout
bash checks/baseline.sh
