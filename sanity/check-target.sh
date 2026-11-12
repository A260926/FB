#!/bin/sh
# Sanity checks for the composed (declarative shadow DOM) view.
#
#   1. rebuilds dist/ from src/ (tsdown + tsc, same as `npm run compile`)
#   2. runs sanity/composed-contract.test.cjs and verifies that the
#      composed-structure and composed-requery cases actually ran
#   3. runs sanity/baseline.cjs (classic physical-tree behaviour)
#   4. runs the existing text encoding tests (test/tests/textcontent-encode.js)
#
# Dependencies come from `npm ci --prefix sanity`; the build tools and the
# library's own dependencies are exposed to the repo build through a
# node_modules symlink.
set -eu

SANITY_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SANITY_DIR/.." && pwd)

if [ ! -d "$SANITY_DIR/node_modules" ]; then
	echo "error: sanity/node_modules is missing." >&2
	echo 'run: npm ci --prefix sanity --ignore-scripts --cache "$(mktemp -d)" --no-audit --no-fund' >&2
	exit 1
fi

# tsdown/tsc and the bundled deps (entities, css-select) resolve through the
# repo's node_modules; provide it from the sanity install if absent.
if [ ! -e "$REPO_ROOT/node_modules" ]; then
	ln -s sanity/node_modules "$REPO_ROOT/node_modules"
fi

echo '==> rebuild dist/ from src/'
cd "$REPO_ROOT"
./node_modules/.bin/tsdown >/dev/null
./node_modules/.bin/tsc --emitDeclarationOnly --outDir dist

echo '==> composed contract tests (sanity/composed-contract.test.cjs)'
TAP_FILE="${TMPDIR:-/tmp}/composed-contract.$$.tap"
if ! node --test --test-reporter=tap "$SANITY_DIR/composed-contract.test.cjs" >"$TAP_FILE" 2>&1; then
	cat "$TAP_FILE"
	rm -f "$TAP_FILE"
	echo 'error: composed contract tests failed' >&2
	exit 1
fi
cat "$TAP_FILE"
# Confirm the two top-level cases actually ran (and passed).
for CASE in composed-structure composed-requery; do
	if ! grep -q "^ok [0-9][0-9]* - $CASE\$" "$TAP_FILE"; then
		rm -f "$TAP_FILE"
		echo "error: top-level case '$CASE' did not run" >&2
		exit 1
	fi
done
rm -f "$TAP_FILE"

echo '==> baseline physical-tree behaviour (sanity/baseline.cjs)'
node "$SANITY_DIR/baseline.cjs"

echo '==> existing text encoding tests (test/tests/textcontent-encode.js)'
cd "$SANITY_DIR"
TEST_TARGET=dist NODE_PATH="$SANITY_DIR/node_modules" \
	./node_modules/.bin/mocha --no-config --require should --reporter spec \
	"$REPO_ROOT/test/tests/textcontent-encode.js"

echo 'sanity checks passed'
