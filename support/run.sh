#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .support-build
mode=${1:-baseline}
case "$mode" in baseline) driver=support/baseline.cc;; target) driver=tests/adaptive_refinement.cc; test -f "$driver"; test -f libsrc/AdaptiveRefiner.cc;; *) exit 2;; esac
extra=
if [ "$mode" = target ]; then extra=libsrc/AdaptiveRefiner.cc; fi
${CXX:-c++} -std=c++11 -O2 -Wno-unknown-pragmas -Iinclude "$driver" libsrc/TriMesh_*.cc libsrc/subdiv.cc libsrc/KDtree.cc libsrc/remove.cc libsrc/reorder_verts.cc $extra -o .support-build/check
.support-build/check
if [ "$mode" = target ]; then sh support/run.sh baseline; fi
