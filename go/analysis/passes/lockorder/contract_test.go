// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package lockorder_test

import (
	"go/ast"
	"go/importer"
	"go/parser"
	"go/token"
	"go/types"
	"os"
	"path/filepath"
	"testing"

	"golang.org/x/tools/go/analysis"
	"golang.org/x/tools/go/analysis/passes/lockorder"
)

// runLockOrder parses and type-checks a sample file under testdata and runs
// the lockorder analyzer through a directly constructed analysis.Pass, using
// only the standard library parser and type checker. The sample program is
// read statically and never executed.
func runLockOrder(t *testing.T, relPath, pkgPath string) (*lockorder.Result, *token.FileSet, []analysis.Diagnostic) {
	t.Helper()
	filename := filepath.Join("testdata", relPath)
	src, err := os.ReadFile(filename)
	if err != nil {
		t.Fatalf("read sample: %v", err)
	}
	fset := token.NewFileSet()
	file, err := parser.ParseFile(fset, filename, src, parser.ParseComments)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	info := &types.Info{
		Types:      make(map[ast.Expr]types.TypeAndValue),
		Defs:       make(map[*ast.Ident]types.Object),
		Uses:       make(map[*ast.Ident]types.Object),
		Selections: make(map[*ast.SelectorExpr]*types.Selection),
	}
	conf := types.Config{Importer: importer.Default()}
	pkg, err := conf.Check(pkgPath, fset, []*ast.File{file}, info)
	if err != nil {
		t.Fatalf("type check: %v", err)
	}
	var diags []analysis.Diagnostic
	pass := &analysis.Pass{
		Analyzer:  lockorder.Analyzer,
		Fset:      fset,
		Files:     []*ast.File{file},
		Pkg:       pkg,
		TypesInfo: info,
		Report:    func(d analysis.Diagnostic) { diags = append(diags, d) },
	}
	out, err := lockorder.Analyzer.Run(pass)
	if err != nil {
		t.Fatalf("analyzer run: %v", err)
	}
	res, ok := out.(*lockorder.Result)
	if !ok {
		t.Fatalf("result is %T, want *lockorder.Result", out)
	}
	return res, fset, diags
}

// TestLockOrderContract drives the analyzer over four hand-written sample
// packages covering aliasing, parameterized summaries, deferred unlocks, and
// cycles. Expected edges and their exact lines/columns are written
// independently of the sample sources.
func TestLockOrderContract(t *testing.T) {
	t.Run("aliases", testAliases)
	t.Run("summaries", testSummaries)
	t.Run("deferred", testDeferred)
	t.Run("cycles", testCycles)
}
