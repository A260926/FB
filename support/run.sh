#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build/classes
javac -encoding UTF-8 -d .build/classes -sourcepath gdx/src:support/vendor support/Current.java
java -cp .build/classes Current
if [ "${1:-baseline}" = target ]; then
 javac -encoding UTF-8 -d .build/classes -cp .build/classes -sourcepath gdx/src:support/vendor support/Target.java
 java -cp .build/classes Target
fi
