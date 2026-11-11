#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
NODE_OPTIONS='--import=tsx/esm' node node_modules/ava/entrypoints/cli.mjs test/pagination.ts --tap
