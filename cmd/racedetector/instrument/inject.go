// Package instrument - Import injection functionality.
//
// This file implements import injection logic for adding the race detector
// runtime and unsafe package imports to instrumented files.
package instrument

import (
	"go/ast"
	"go/token"
	"strconv"
)

// injectImports adds required imports to the AST file.
//
// This function injects two imports needed for race detection:
//  1. import race "github.com/kolkov/racedetector/internal/race/api"
//  2. import "unsafe"
//
// The function handles several edge cases:
//   - No imports section: Creates new import section
//   - Imports already exist: Skips injection (no duplicates)
//   - Import path exists with different alias: Skips injection
//   - Grouped imports: Adds to existing import group
//   - Single imports: Converts to grouped imports
//
// Parameters:
//   - file: AST file to modify (modified in place)
//
// Returns:
//   - error: Injection error, or nil on success
//
// Algorithm:
//  1. Scan existing imports to detect duplicates
//  2. If race package import missing, add it
//  3. If unsafe import missing, add it
//  4. Handle both grouped and single import styles
//
// Example Transformations:
//
//	// No imports → Add both
//	package main              package main
//	                          import race "github.com/.../api"
//	func main() {}      →     import "unsafe"
//	                          func main() {}
//
//	// Has imports → Add to group
//	package main              package main
//	import "fmt"              import (
//	                              "fmt"
//	func main() {}      →         race "github.com/.../api"
//	                              "unsafe"
//	                          )
//	                          func main() {}
//
// Thread Safety: NOT thread-safe (modifies AST in place).
//
//nolint:unparam // error return is for future error handling if needed
func injectImports(file *ast.File) error {
	// Step 1: Check which imports are already present.
	hasRaceImport := false
	hasUnsafeImport := false

	for _, imp := range file.Imports {
		// Import path is stored with quotes: "path"
		// We need to unquote it for comparison.
		path, err := strconv.Unquote(imp.Path.Value)
		if err != nil {
			// This shouldn't happen for valid Go files, but handle gracefully.
			continue
		}

		// Check if race package import exists (with any alias or no alias).
		if path == RacePackageImportPath {
			hasRaceImport = true
		}

		// Check if unsafe import exists.
		if path == "unsafe" {
			hasUnsafeImport = true
		}
	}

	// Step 2: If both imports already exist, nothing to do.
	if hasRaceImport && hasUnsafeImport {
		return nil
	}

	// Step 3: Add missing imports to the AST.
	// We'll add imports to the first declaration group (or create one).
	//
	// Go AST structure:
	//   file.Decls = []ast.Decl{
	//       &ast.GenDecl{Tok: token.IMPORT, Specs: []ast.Spec{...}},  // Import block
	//       &ast.FuncDecl{...},  // Function declarations
	//       ...
	//   }

	// Find or create the import declaration block.
	var importDecl *ast.GenDecl

	// Scan declarations for existing import block.
	for _, decl := range file.Decls {
		genDecl, ok := decl.(*ast.GenDecl)
		if !ok {
			continue
		}
		if genDecl.Tok == token.IMPORT {
			importDecl = genDecl
			break
		}
	}

	// If no import block exists, create one.
	if importDecl == nil {
		importDecl = &ast.GenDecl{
			Tok:    token.IMPORT,
			Lparen: 1, // Non-zero Lparen means grouped import: import (...)
		}
		// Insert at the beginning of declarations (after package).
		file.Decls = append([]ast.Decl{importDecl}, file.Decls...)
	}

	// Step 4: Add missing race package import.
	if !hasRaceImport {
		raceImport := &ast.ImportSpec{
			Name: &ast.Ident{Name: RacePackageAlias}, // Alias: race
			Path: &ast.BasicLit{
				Kind:  token.STRING,
				Value: strconv.Quote(RacePackageImportPath), // "github.com/.../api"
			},
		}
		importDecl.Specs = append(importDecl.Specs, raceImport)
	}

	// Step 5: Add missing unsafe import.
	if !hasUnsafeImport {
		unsafeImport := &ast.ImportSpec{
			Path: &ast.BasicLit{
				Kind:  token.STRING,
				Value: strconv.Quote("unsafe"),
			},
		}
		importDecl.Specs = append(importDecl.Specs, unsafeImport)
	}

	// Step 6: Ensure import block uses grouped syntax: import (...)
	// If we added imports and Lparen was 0 (single import style),
	// set it to non-zero to trigger grouped output.
	if importDecl.Lparen == 0 && len(importDecl.Specs) > 1 {
		importDecl.Lparen = 1
	}

	// Step 7: Update file.Imports to include new imports.
	// This is necessary for AST consistency and potential downstream tools.
	file.Imports = nil
	for _, decl := range file.Decls {
		genDecl, ok := decl.(*ast.GenDecl)
		if !ok || genDecl.Tok != token.IMPORT {
			continue
		}
		for _, spec := range genDecl.Specs {
			impSpec, ok := spec.(*ast.ImportSpec)
			if !ok {
				continue
			}
			file.Imports = append(file.Imports, impSpec)
		}
	}

	return nil
}
