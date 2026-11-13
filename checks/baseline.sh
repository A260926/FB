#!/bin/sh
set -eu
mkdir -p build
c++ -std=c++11 -O0 -I. checks/baseline.cpp spirv_cross.cpp spirv_parser.cpp spirv_cross_parsed_ir.cpp spirv_cfg.cpp spirv_glsl.cpp -o build/baseline
./build/baseline
