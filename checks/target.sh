set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -I tinyshader tinyshader/tinyshader_unity.c checks/target.c checks/target_main.c -lm -o "$b/check"
"$b/check" | tee "$b/target.log"
for group in lowering control diagnostics; do
 grep -Eq "^GROUP_OK $group [1-9][0-9]*$" "$b/target.log"
done
grep -Eq '^TARGET_OK [1-9][0-9]*$' "$b/target.log"
bash checks/baseline.sh
