#!/bin/sh
# Robust buffer access checks.
#
# Builds the SPIRV-Cross GLSL backend together with the self-contained C++ test driver. The driver
# generates valid SPIR-V with an embedded word constructor (no external SPIR-V tools required), compiles
# it with and without Options::robust_buffer_access, and executes the generated guard fragments on a
# bounded CPU model with sentinel memory. Run from the repository root.
set -eu

# Run from the repository root (the same convention as checks/baseline.sh).
set -eu

mkdir -p build 2>/dev/null || true
CXX=${CXX:-c++}

$CXX -std=c++11 -O0 -I. -Ichecks/robust \
	checks/robust/check-robust.cpp \
	spirv_cross.cpp spirv_parser.cpp spirv_cross_parsed_ir.cpp spirv_cfg.cpp spirv_glsl.cpp \
	-o build/check-robust

# The driver shells out to the system compiler for each translated fragment; tell it where the bounded
# model header lives.
ROBUST_INCLUDE="$(pwd)/checks/robust" TMPDIR="${TMPDIR:-/tmp}" ./build/check-robust
