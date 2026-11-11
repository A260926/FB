#!/bin/sh
set -eu
. ./baseline/env.sh
go run baseline/testguard.go ./prog TestBaselineHints
