#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
./.check-build/baseline
sh checks/old.sh
