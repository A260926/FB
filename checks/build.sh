#!/bin/bash
set -euo pipefail
out=$1; shift
bash checks/prepare.sh
c++ -std=c++17 -O0 -w -DLNLIB_STATIC '-D__declspec(x)=' -Isrc/LNLib/include -Ichecks/vendor/eigen -Ichecks "$@" .checks-build/liblnlib.a -o "$out"
