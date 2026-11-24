// Package instrument - Custom error types for instrumentation.
//
// This file defines professional error handling for the instrumentation engine.
// Errors include file position (file:line:column) and helpful suggestions.
//
// Example output:
//
//	main.go:42:15: failed to instrument assignment: invalid syntax
//
//	Suggestion: Ensure all variables in the assignment have valid types
package instrument

import (
	"fmt"
	"go/token"
)

// InstrumentationError represents an error during instrumentation with context.
//
// This error type provides detailed information about where instrumentation failed,
// including file position and helpful suggestions for resolving the issue.
//
// Fields:
//   - File: Source file path where error occurred
//   - Line: Line number (1-indexed)
//   - Column: Column number (1-indexed)
//   - Message: Human-readable error description
//   - Suggestion: Optional hint for fixing the error
//
// Example:
//
//	err := &InstrumentationError{
//	    File:       "main.go",
//	    Line:       42,
//	    Column:     15,
//	    Message:    "failed to instrument assignment: invalid syntax",
//	    Suggestion: "Ensure all variables in the assignment have valid types",
//	}
//	fmt.Println(err) // Output: main.go:42:15: failed to instrument assignment: invalid syntax
//
// Thread Safety: Immutable after creation, safe for concurrent use.
type InstrumentationError struct {
	File       string // Source file path
	Line       int    // Line number (1-indexed)
	Column     int    // Column number (1-indexed)
	Message    string // Error message
	Suggestion string // Optional suggestion for fixing (empty if none)
}

// Error implements the error interface.
//
// Format: file:line:column: message
//
// If Suggestion is non-empty, it's appended on a new line with "Suggestion: " prefix.
//
// Returns:
//   - string: Formatted error message
//
// Thread Safety: Safe for concurrent use (read-only).
func (e *InstrumentationError) Error() string {
	result := fmt.Sprintf("%s:%d:%d: %s", e.File, e.Line, e.Column, e.Message)
	if e.Suggestion != "" {
		result += fmt.Sprintf("\n\nSuggestion: %s", e.Suggestion)
	}
	return result
}

// NewInstrumentationError creates an error with file position from AST node.
//
// This helper extracts file:line:column information from a token.Pos
// using the provided FileSet. This ensures errors point to the exact
// location in the source code where instrumentation failed.
//
// Parameters:
//   - fset: File set containing position information
//   - pos: Token position (from AST node.Pos())
//   - msg: Error message describing what went wrong
//
// Returns:
//   - *InstrumentationError: Error with file position populated
//
// Example:
//
//	fset := token.NewFileSet()
//	file, _ := parser.ParseFile(fset, "main.go", src, 0)
//	for _, stmt := range file.Body {
//	    if err := instrumentStmt(stmt); err != nil {
//	        return NewInstrumentationError(fset, stmt.Pos(), "failed to instrument statement")
//	    }
//	}
//
// Thread Safety: Safe for concurrent use (fset is read-only).
func NewInstrumentationError(fset *token.FileSet, pos token.Pos, msg string) *InstrumentationError {
	position := fset.Position(pos)
	return &InstrumentationError{
		File:    position.Filename,
		Line:    position.Line,
		Column:  position.Column,
		Message: msg,
	}
}

// NewInstrumentationErrorWithSuggestion creates an error with suggestion.
//
// This variant includes a helpful suggestion for resolving the error.
// Use this when you can provide actionable guidance to the user.
//
// Parameters:
//   - fset: File set containing position information
//   - pos: Token position (from AST node.Pos())
//   - msg: Error message describing what went wrong
//   - suggestion: Helpful hint for fixing the error
//
// Returns:
//   - *InstrumentationError: Error with file position and suggestion
//
// Example:
//
//	return NewInstrumentationErrorWithSuggestion(
//	    fset,
//	    stmt.Pos(),
//	    "cannot instrument blank identifier",
//	    "Remove blank identifier (_) from assignment or use a named variable",
//	)
//
// Thread Safety: Safe for concurrent use (fset is read-only).
func NewInstrumentationErrorWithSuggestion(fset *token.FileSet, pos token.Pos, msg, suggestion string) *InstrumentationError {
	err := NewInstrumentationError(fset, pos, msg)
	err.Suggestion = suggestion
	return err
}
