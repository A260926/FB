#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
find src -name '*.java' -print > "$b/sources.txt"
printf '%s\n' userfb/HgeomBaseline.java >> "$b/sources.txt"
javac --release 17 -encoding ISO-8859-1 -d "$b/classes" @"$b/sources.txt"
java -cp "$b/classes" HgeomBaseline
