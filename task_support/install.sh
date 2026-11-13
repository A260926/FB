#!/bin/bash
set -euo pipefail
test -d task_support/vendor/cheerio
test -f task_support/runtime-lock.json
node --version
