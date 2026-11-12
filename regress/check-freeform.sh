#!/bin/bash
#
# regress/check-freeform.sh
#
# Builds and runs the free-form curve regression (regress/freeform.cpp, which
# has its own main and prints the PASS ... / TINYOBJ_FREEFORM_OK lines itself)
# with real_t = double. The direct invocation through regress/build-run.sh
# uses the default float real_t, so together the two entry points cover both
# supported precisions.
set -euo pipefail

b=$(mktemp -d "${TMPDIR:-/tmp}/tinyobj.XXXXXX")
trap 'rm -rf "$b"' EXIT

c++ -std=c++11 -O1 -DTINYOBJLOADER_USE_DOUBLE -I. regress/freeform.cpp \
    -o "$b/freeform"
"$b/freeform"
