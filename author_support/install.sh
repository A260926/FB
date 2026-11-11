#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export npm_config_userconfig="$PWD/author_support/npm-user.conf"
export npm_config_globalconfig="$PWD/author_support/npm-global.conf"
export npm_config_cache="$PWD/.author-cache/npm"
export npm_config_registry=https://registry.npmjs.org
export npm_config_update_notifier=false
node --version
npm --version
node author_support/lock-check.mjs save
npm ci --ignore-scripts --no-audit --no-fund
node author_support/lock-check.mjs check
node node_modules/typescript/bin/tsc --noEmit
printf '%s\n' 'INSTALL_OK locked dependencies and TypeScript'
