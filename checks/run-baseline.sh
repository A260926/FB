#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
for opt in 0 2; do
 cc -std=c99 -O"$opt" -I. checks/baseline.c -lm -o "$build/baseline"
 "$build/baseline"
done
