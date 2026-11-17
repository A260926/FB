#!/bin/bash
set -euo pipefail
test "$#" -eq 2
test -s "$1"
mkdir -p "$2"
java -cp .task-build/classes org.jetbrains.java.decompiler.main.decompiler.ConsoleDecompiler -dgs=1 -log=WARN "$1" "$2"
