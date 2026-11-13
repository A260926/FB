#!/bin/bash
set -euo pipefail
out=$1; shift
c++ -std=c++14 -O0 -I. -Iinclude -Ihlslang -Ihlslang/MachineIndependent -Ihlslang/OSDependent/Mac -Ichecks/generated hlslang/GLSLCodeGen/*.cpp hlslang/MachineIndependent/*.cpp hlslang/MachineIndependent/preprocessor/*.cpp hlslang/OSDependent/Mac/ossource.cpp checks/generated/*.cpp "$@" -o "$out"
