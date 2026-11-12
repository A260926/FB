set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -I tinyshader tinyshader/tinyshader_unity.c checks/baseline.c -lm -o "$b/check"
"$b/check"
