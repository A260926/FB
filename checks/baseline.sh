#!/bin/bash
set -euo pipefail
bash checks/link.sh .task-build/baseline checks/baseline.cpp
.task-build/baseline
