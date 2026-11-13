#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh task_support/build.sh
node task_support/baseline.mjs
