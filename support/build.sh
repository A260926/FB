#!/bin/bash
set -euo pipefail
"${CXX:-c++}" -std=c++11 -O1 -Isrc -Isrc/TheRestOfYourLife -I. "$@"
