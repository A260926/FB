#!/bin/sh
set -eu
. ./baseline/env.sh
go run baseline/testguard.go ./prog TestHintsCheckConstArg TestHintsCheckDataArg TestHintsCompressedImage TestHintsShrinkExpand TestHintsCall TestHintsData TestInplaceIntersect TestHintsLimiter TestIntTypeUselessHintsRange
go run baseline/testguard.go ./pkg/fuzzer TestDeflake
go run baseline/testguard.go ./pkg/fuzzer/queue TestPlainQueue TestPrioQueue TestGlobFiles TestTee
go run baseline/testguard.go ./pkg/image TestCompress TestEncode
