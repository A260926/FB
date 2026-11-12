#!/bin/bash
set -euo pipefail
node checks/target.mjs
bash checks/baseline.sh
