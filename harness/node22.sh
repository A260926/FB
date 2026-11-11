#!/bin/sh
set -eu
# Resolve a published, exact Node version independently of the caller's PATH.
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/.build/node22-bootstrap"
: > "$root/.build/node22-bootstrap/user.npmrc"
: > "$root/.build/node22-bootstrap/global.npmrc"
exec npm --userconfig="$root/.build/node22-bootstrap/user.npmrc" \
 --globalconfig="$root/.build/node22-bootstrap/global.npmrc" \
 --registry=https://registry.npmjs.org \
 --cache="$root/.build/node22-bootstrap/cache" \
 exec --yes --package=node@22.23.2 -- node "$@"
