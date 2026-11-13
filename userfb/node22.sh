#!/bin/sh
set -eu
# Resolve a published, exact Node version independently of the caller's PATH.
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/.userfb-build/node22-bootstrap"
: > "$root/.userfb-build/node22-bootstrap/user.npmrc"
: > "$root/.userfb-build/node22-bootstrap/global.npmrc"
exec npm --userconfig="$root/.userfb-build/node22-bootstrap/user.npmrc" \
 --globalconfig="$root/.userfb-build/node22-bootstrap/global.npmrc" \
 --registry=https://registry.npmjs.org \
 --cache="$root/.userfb-build/node22-bootstrap/cache" \
 exec --yes --package=node@22.23.2 -- node "$@"
