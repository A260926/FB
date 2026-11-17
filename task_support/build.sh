#!/bin/bash
set -euo pipefail
mkdir -p .task-build/classes .task-build/fixtures
find src task_support/vendor/annotations -name '*.java' ! -name module-info.java | sort > .task-build/sources.txt
test -s .task-build/sources.txt
javac --release 21 -proc:none -encoding UTF-8 -d .task-build/classes @.task-build/sources.txt
javac --release 17 -proc:none -g:none -encoding UTF-8 -d .task-build/fixtures task_support/fixtures/*.java
c++ -std=c++11 -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
