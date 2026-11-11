#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
.author-build/baseline
.author-build/baseline .author-build/input.wav
.author-build/soundstretch .author-build/input.wav .author-build/output.wav -tempo=20 -pitch=0 > .author-build/soundstretch.log 2>&1
.author-build/baseline --check-wav .author-build/output.wav
