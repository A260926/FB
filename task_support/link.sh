#!/bin/bash
set -euo pipefail
cc -std=c11 -D_POSIX_C_SOURCE=200809L -O0 -g -I. task_support/driver.c tests/elf_mapping_regression.c .task-build/libarmdisasm.a -o .task-build/target
