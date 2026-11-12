// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package lockorder

import (
	_ "embed"
	"reflect"

	"golang.org/x/tools/go/analysis"
	"golang.org/x/tools/internal/analysis/analyzerutil"
)

//go:embed doc.go
var doc string

// Analyzer is the lockorder analyzer.
//
// It reports potential cycles in the acquisition order of sync.Mutex locks
// across the functions of a single package. Diagnostics carry each real
// acquisition point and the intra-package call sites traversed to reach it.
// The analysis is purely static: it builds on go/types information and the
// go/cfg control-flow graph and never executes the analyzed program.
var Analyzer = &analysis.Analyzer{
	Name:       "lockorder",
	Doc:        analyzerutil.MustExtractDoc(doc, "lockorder"),
	URL:        "https://pkg.go.dev/golang.org/x/tools/go/analysis/passes/lockorder",
	Run:        run,
	ResultType: reflect.TypeFor[*Result](),
}

func run(pass *analysis.Pass) (any, error) {
	e := newEngine(pass)
	return e.run(), nil
}
