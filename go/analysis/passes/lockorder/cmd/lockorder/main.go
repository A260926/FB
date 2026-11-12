// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// The lockorder command runs the lockorder analyzer.
package main

import (
	"golang.org/x/tools/go/analysis/passes/lockorder"
	"golang.org/x/tools/go/analysis/singlechecker"
)

func main() { singlechecker.Main(lockorder.Analyzer) }
