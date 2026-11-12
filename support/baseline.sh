#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
mkdir "$b/db"
cc -O1 -pthread -Ilibraries/liblmdb support/baseline.c libraries/liblmdb/{mdb,midl,module}.c -o "$b/baseline"
"$b/baseline" "$b/db"
