#!/bin/bash
set -euo pipefail
mkdir -p node_modules
for name in async iconv-lite robots-parser safer-buffer urijs; do
  test -f "task_support/vendor/$name/package.json"
  cp -R "task_support/vendor/$name" node_modules/
done
node -e 'require("./lib"); console.log("OFFLINE_RUNTIME_READY")'
