#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
node node_modules/typescript/bin/tsc -p tsconfig.json
