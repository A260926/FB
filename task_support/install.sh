#!/bin/bash
set -euo pipefail
test -f vendor/modules.txt
go version
