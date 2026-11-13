#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s task_support/target_cases.cpp
bash task_support/baseline.sh
b="${TASK_BUILD_DIR:-.build}"
c++ -std=c++11 -I. -ISource/C++/Core -ISource/C++/Codecs -ISource/C++/Crypto -ISource/C++/MetaData task_support/target_cases.cpp task_support/driver.cpp "$b/libap4.a" -o "$b/target_cases"
"$b/target_cases"
