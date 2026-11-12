#!/bin/sh
set -eu
mkdir -p .build
NODE_PATH="$PWD/sanity/node_modules" sanity/node_modules/.bin/esbuild src/index.ts --bundle --platform=node --format=cjs --outfile=.build/index.cjs
