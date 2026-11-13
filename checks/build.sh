#!/bin/bash
set -euo pipefail
out=$1; shift
c++ -std=c++17 -O0 -I. spirv_cross.cpp spirv_parser.cpp spirv_cross_parsed_ir.cpp spirv_cfg.cpp "$@" -o "$out"
