#!/bin/sh
set -eu
. ./userfb_baseline/env.sh
go run userfb_baseline/testguard.go ./prog TestUserFBBaselineHints
