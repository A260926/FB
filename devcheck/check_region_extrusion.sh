#!/usr/bin/env bash
set -euo pipefail

# Root directory of the repository (this script lives in devcheck/)
root=$(cd "$(dirname "$0")/.." && pwd)
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT

# Compiles the actual library together with the devcheck driver. No external
# test framework is downloaded. Sources are read with the same encoding as in
# baseline.sh.
find "$root/src" -name '*.java' -print > "$b/sources.txt"
printf '%s\n' "$root/devcheck/RegionExtrusionCheck.java" >> "$b/sources.txt"
javac --release 17 -encoding ISO-8859-1 -d "$b/classes" @"$b/sources.txt"
java -cp "$b/classes" RegionExtrusionCheck
