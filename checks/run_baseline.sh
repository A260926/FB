#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
.build/baseline
.build/baseline .build/input.wav
.build/soundstretch .build/input.wav .build/output.wav -tempo=20 -pitch=0 > .build/soundstretch.log 2>&1
.build/baseline --check-wav .build/output.wav
