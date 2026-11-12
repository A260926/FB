#!/bin/bash
set -euo pipefail
exec mvn -B -ntp -s regress/settings.xml -gs regress/settings.xml -Dmaven.repo.local="$PWD/.build/m2" "$@"
