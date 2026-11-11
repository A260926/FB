#!/bin/sh
set -eu
. ./userfb_baseline/env.sh
go run userfb_baseline/testguard.go ./prog TestHintsCheckConstArg TestHintsCheckDataArg TestHintsCompressedImage TestHintsShrinkExpand TestHintsCall TestHintsData TestInplaceIntersect TestHintsLimiter TestIntTypeUselessHintsRange
go run userfb_baseline/testguard.go ./pkg/fuzzer TestDeflake
go run userfb_baseline/testguard.go ./pkg/fuzzer/queue TestPlainQueue TestPrioQueue TestGlobFiles TestTee
go run userfb_baseline/testguard.go ./pkg/image TestCompress TestEncode
