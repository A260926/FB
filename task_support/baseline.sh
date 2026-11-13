#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/build.sh
b="${TASK_BUILD_DIR:-.build}"
c++ -std=c++11 -I. -ISource/C++/Core -ISource/C++/Codecs -ISource/C++/Crypto -ISource/C++/MetaData task_support/baseline.cpp "$b/libap4.a" -o "$b/baseline"
"$b/baseline"
