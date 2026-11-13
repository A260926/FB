#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s checks/target.c
sh checks/run.sh checks/target.c
sh checks/baseline.sh
sh checks/witness.sh
