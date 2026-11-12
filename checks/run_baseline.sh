#!/bin/sh
set -eu
test -x .build/baseline
.build/baseline
