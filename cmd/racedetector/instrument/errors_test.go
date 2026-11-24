// Package instrument - Tests for error handling.
package instrument

import (
	"go/ast"
	"go/parser"
	"go/token"
	"strings"
	"testing"
)

// TestInstrumentationError_Error tests error message formatting.
func TestInstrumentationError_Error(t *testing.T) {
	tests := []struct {
		name     string
		err      *InstrumentationError
		expected string
	}{
		{
			name: "basic error without suggestion",
			err: &InstrumentationError{
				File:    "main.go",
				Line:    42,
				Column:  15,
				Message: "failed to instrument assignment",
			},
			expected: "main.go:42:15: failed to instrument assignment",
		},
		{
			name: "error with suggestion",
			err: &InstrumentationError{
				File:       "test.go",
				Line:       100,
				Column:     5,
				Message:    "cannot instrument blank identifier",
				Suggestion: "Remove blank identifier (_) from assignment",
			},
			expected: "test.go:100:5: cannot instrument blank identifier\n\nSuggestion: Remove blank identifier (_) from assignment",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			result := tt.err.Error()
			if result != tt.expected {
				t.Errorf("Error() = %q, want %q", result, tt.expected)
			}
		})
	}
}

// TestNewInstrumentationError tests error creation from token position.
func TestNewInstrumentationError(t *testing.T) {
	// Parse a simple Go file to get real positions
	src := `package main

func main() {
	x := 42
}
`
	fset := token.NewFileSet()
	file, err := parser.ParseFile(fset, "test.go", src, 0)
	if err != nil {
		t.Fatalf("Failed to parse source: %v", err)
	}

	// Get position of first statement (x := 42)
	mainFunc := file.Decls[0].(*ast.FuncDecl)
	firstStmt := mainFunc.Body.List[0]
	pos := firstStmt.Pos()

	// Create error
	instrErr := NewInstrumentationError(fset, pos, "test error")

	// Verify error has correct file position
	if instrErr.File != "test.go" {
		t.Errorf("File = %q, want %q", instrErr.File, "test.go")
	}
	if instrErr.Line != 4 { // "x := 42" is on line 4
		t.Errorf("Line = %d, want %d", instrErr.Line, 4)
	}
	if instrErr.Column == 0 {
		t.Error("Column should be non-zero")
	}
	if instrErr.Message != "test error" {
		t.Errorf("Message = %q, want %q", instrErr.Message, "test error")
	}
	if instrErr.Suggestion != "" {
		t.Errorf("Suggestion = %q, want empty", instrErr.Suggestion)
	}
}

// TestNewInstrumentationErrorWithSuggestion tests error creation with suggestion.
func TestNewInstrumentationErrorWithSuggestion(t *testing.T) {
	src := `package main
func main() {}
`
	fset := token.NewFileSet()
	file, err := parser.ParseFile(fset, "main.go", src, 0)
	if err != nil {
		t.Fatalf("Failed to parse source: %v", err)
	}

	pos := file.Pos()
	instrErr := NewInstrumentationErrorWithSuggestion(
		fset,
		pos,
		"test error",
		"try this fix",
	)

	// Verify suggestion is populated
	if instrErr.Suggestion != "try this fix" {
		t.Errorf("Suggestion = %q, want %q", instrErr.Suggestion, "try this fix")
	}

	// Verify error message includes suggestion
	errMsg := instrErr.Error()
	if !strings.Contains(errMsg, "Suggestion: try this fix") {
		t.Errorf("Error message doesn't contain suggestion: %q", errMsg)
	}
}

// TestInstrumentationError_Integration tests realistic error scenarios.
func TestInstrumentationError_Integration(t *testing.T) {
	// Simulate instrumenting a file with an error
	src := `package main

const X = 42

func main() {
	y := X + 1
	_ = y
}
`
	fset := token.NewFileSet()
	file, err := parser.ParseFile(fset, "example.go", src, 0)
	if err != nil {
		t.Fatalf("Failed to parse source: %v", err)
	}

	// Simulate an instrumentation error on line 6 (y := X + 1)
	mainFunc := file.Decls[1].(*ast.FuncDecl)
	assignStmt := mainFunc.Body.List[0]
	pos := assignStmt.Pos()

	instrErr := NewInstrumentationErrorWithSuggestion(
		fset,
		pos,
		"failed to instrument assignment: invalid type",
		"Ensure all variables have valid types",
	)

	// Verify error points to correct line
	errMsg := instrErr.Error()
	if !strings.HasPrefix(errMsg, "example.go:6:") {
		t.Errorf("Error should start with 'example.go:6:', got: %q", errMsg)
	}

	// Verify error contains message
	if !strings.Contains(errMsg, "failed to instrument assignment") {
		t.Errorf("Error should contain message, got: %q", errMsg)
	}

	// Verify error contains suggestion
	if !strings.Contains(errMsg, "Suggestion: Ensure all variables have valid types") {
		t.Errorf("Error should contain suggestion, got: %q", errMsg)
	}
}
