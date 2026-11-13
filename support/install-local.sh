#!/usr/bin/env bash
set -euo pipefail
export GEM_HOME="$PWD/.gems"
export GEM_PATH="$GEM_HOME"
gem install --local --no-document --ignore-dependencies vendor/gems/*.gem
