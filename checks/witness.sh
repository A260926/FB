#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh checks/run.sh checks/baseline.c contract
