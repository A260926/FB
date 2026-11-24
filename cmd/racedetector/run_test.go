// run_test.go tests the 'racedetector run' command.
package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestParseRunArgs_SimpleFile tests parsing a single source file.
func TestParseRunArgs_SimpleFile(t *testing.T) {
	args := []string{"main.go"}

	config, programArgs, err := parseRunArgs(args)
	if err != nil {
		t.Fatalf("parseRunArgs() error: %v", err)
	}

	if len(config.sourceFiles) != 1 {
		t.Errorf("Expected 1 source file, got %d", len(config.sourceFiles))
	}

	if config.sourceFiles[0] != "main.go" {
		t.Errorf("Expected main.go, got %s", config.sourceFiles[0])
	}

	if len(programArgs) != 0 {
		t.Errorf("Expected no program args, got %v", programArgs)
	}
}

// TestParseRunArgs_FileWithArgs tests source file + program arguments.
func TestParseRunArgs_FileWithArgs(t *testing.T) {
	args := []string{"main.go", "arg1", "arg2", "--flag=value"}

	config, programArgs, err := parseRunArgs(args)
	if err != nil {
		t.Fatalf("parseRunArgs() error: %v", err)
	}

	// Should have 1 source file
	if len(config.sourceFiles) != 1 || config.sourceFiles[0] != "main.go" {
		t.Errorf("Expected [main.go], got %v", config.sourceFiles)
	}

	// Should have 3 program args
	expectedArgs := []string{"arg1", "arg2", "--flag=value"}
	if len(programArgs) != len(expectedArgs) {
		t.Errorf("Expected %d program args, got %d", len(expectedArgs), len(programArgs))
	}

	for i, expected := range expectedArgs {
		if programArgs[i] != expected {
			t.Errorf("Arg %d: expected %q, got %q", i, expected, programArgs[i])
		}
	}
}

// TestParseRunArgs_MultipleFiles tests multiple source files.
func TestParseRunArgs_MultipleFiles(t *testing.T) {
	args := []string{"main.go", "helper.go", "utils.go"}

	config, programArgs, err := parseRunArgs(args)
	if err != nil {
		t.Fatalf("parseRunArgs() error: %v", err)
	}

	if len(config.sourceFiles) != 3 {
		t.Errorf("Expected 3 source files, got %d", len(config.sourceFiles))
	}

	if len(programArgs) != 0 {
		t.Errorf("Expected no program args, got %v", programArgs)
	}
}

// TestParseRunArgs_MultipleFilesWithArgs tests multiple files + args.
func TestParseRunArgs_MultipleFilesWithArgs(t *testing.T) {
	args := []string{"main.go", "helper.go", "arg1", "--flag"}

	config, programArgs, err := parseRunArgs(args)
	if err != nil {
		t.Fatalf("parseRunArgs() error: %v", err)
	}

	// Should have 2 source files
	if len(config.sourceFiles) != 2 {
		t.Errorf("Expected 2 source files, got %d", len(config.sourceFiles))
	}

	// Should have 2 program args
	expectedArgs := []string{"arg1", "--flag"}
	if len(programArgs) != len(expectedArgs) {
		t.Errorf("Expected %d program args, got %d: %v", len(expectedArgs), len(programArgs), programArgs)
	}
}

// TestParseRunArgs_BuildFlags tests build flags before source files.
func TestParseRunArgs_BuildFlags(t *testing.T) {
	args := []string{"-tags", "debug", "main.go", "arg1"}

	config, programArgs, err := parseRunArgs(args)
	if err != nil {
		t.Fatalf("parseRunArgs() error: %v", err)
	}

	// Should have build flags
	if len(config.buildFlags) != 2 {
		t.Errorf("Expected 2 build flags, got %d: %v", len(config.buildFlags), config.buildFlags)
	}

	// Should have 1 source file
	if len(config.sourceFiles) != 1 || config.sourceFiles[0] != "main.go" {
		t.Errorf("Expected [main.go], got %v", config.sourceFiles)
	}

	// Should have 1 program arg
	if len(programArgs) != 1 || programArgs[0] != "arg1" {
		t.Errorf("Expected [arg1], got %v", programArgs)
	}
}

// TestParseRunArgs_NoFiles tests error when no files specified.
func TestParseRunArgs_NoFiles(t *testing.T) {
	args := []string{}

	_, _, err := parseRunArgs(args)
	if err == nil {
		t.Error("Expected error for no files, got nil")
	}

	if !strings.Contains(err.Error(), "no source files") {
		t.Errorf("Expected 'no source files' error, got: %v", err)
	}
}

// TestParseRunArgs_NoGoFiles tests error when only non-.go files.
func TestParseRunArgs_NoGoFiles(t *testing.T) {
	args := []string{"README.md", "config.json"}

	_, _, err := parseRunArgs(args)
	if err == nil {
		t.Error("Expected error for no .go files, got nil")
	}

	if !strings.Contains(err.Error(), "no Go source files") {
		t.Errorf("Expected 'no Go source files' error, got: %v", err)
	}
}

// TestBuildTemporary tests temporary binary creation.
func TestBuildTemporary(t *testing.T) {
	// Create a simple test source file
	tempDir, err := os.MkdirTemp("", "test-build-temp-*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	testSource := `package main

func main() {
	println("Hello from racedetector!")
}
`
	testFile := filepath.Join(tempDir, "main.go")
	if err := os.WriteFile(testFile, []byte(testSource), 0644); err != nil {
		t.Fatalf("Failed to create test file: %v", err)
	}

	// Create config
	config := &buildConfig{
		sourceFiles: []string{testFile},
		workDir:     tempDir,
	}

	// Build temporary binary
	binaryPath, err := buildTemporary(config)
	if err != nil {
		t.Fatalf("buildTemporary() error: %v", err)
	}
	defer os.Remove(binaryPath)

	// Verify binary was created
	if _, err := os.Stat(binaryPath); os.IsNotExist(err) {
		t.Errorf("Binary not created at %s", binaryPath)
	}

	// Verify it's in temp directory
	if !strings.Contains(binaryPath, "racedetector-run-") {
		t.Errorf("Binary path doesn't match pattern: %s", binaryPath)
	}

	// Binary should be executable (has .exe on Windows)
	if !strings.HasSuffix(binaryPath, ".exe") && !strings.HasSuffix(binaryPath, "") {
		t.Logf("Binary path: %s", binaryPath)
	}
}

// TestExecuteBinary_Success tests successful binary execution.
func TestExecuteBinary_Success(t *testing.T) {
	// Create a simple program that exits with 0
	tempDir, err := os.MkdirTemp("", "test-exec-*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	testSource := `package main

func main() {
	// Exit successfully
}
`
	testFile := filepath.Join(tempDir, "main.go")
	if err := os.WriteFile(testFile, []byte(testSource), 0644); err != nil {
		t.Fatalf("Failed to create test file: %v", err)
	}

	config := &buildConfig{
		sourceFiles: []string{testFile},
		workDir:     tempDir,
	}

	binaryPath, err := buildTemporary(config)
	if err != nil {
		t.Fatalf("buildTemporary() error: %v", err)
	}
	defer os.Remove(binaryPath)

	// Execute binary
	exitCode := executeBinary(binaryPath, []string{})

	if exitCode != 0 {
		t.Errorf("Expected exit code 0, got %d", exitCode)
	}
}

// TestExecuteBinary_WithArgs tests binary execution with arguments.
func TestExecuteBinary_WithArgs(t *testing.T) {
	// Create a program that prints its arguments
	tempDir, err := os.MkdirTemp("", "test-exec-args-*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	testSource := `package main

import "os"

func main() {
	// Just access args to ensure they're passed
	_ = os.Args
}
`
	testFile := filepath.Join(tempDir, "main.go")
	if err := os.WriteFile(testFile, []byte(testSource), 0644); err != nil {
		t.Fatalf("Failed to create test file: %v", err)
	}

	config := &buildConfig{
		sourceFiles: []string{testFile},
		workDir:     tempDir,
	}

	binaryPath, err := buildTemporary(config)
	if err != nil {
		t.Fatalf("buildTemporary() error: %v", err)
	}
	defer os.Remove(binaryPath)

	// Execute with arguments
	args := []string{"arg1", "arg2", "--flag=value"}
	exitCode := executeBinary(binaryPath, args)

	if exitCode != 0 {
		t.Errorf("Expected exit code 0, got %d", exitCode)
	}
}

// TestExecuteBinary_NonZeroExit tests binary with non-zero exit code.
func TestExecuteBinary_NonZeroExit(t *testing.T) {
	// Create a program that exits with code 42
	tempDir, err := os.MkdirTemp("", "test-exec-exit-*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	testSource := `package main

import "os"

func main() {
	os.Exit(42)
}
`
	testFile := filepath.Join(tempDir, "main.go")
	if err := os.WriteFile(testFile, []byte(testSource), 0644); err != nil {
		t.Fatalf("Failed to create test file: %v", err)
	}

	config := &buildConfig{
		sourceFiles: []string{testFile},
		workDir:     tempDir,
	}

	binaryPath, err := buildTemporary(config)
	if err != nil {
		t.Fatalf("buildTemporary() error: %v", err)
	}
	defer os.Remove(binaryPath)

	// Execute binary
	exitCode := executeBinary(binaryPath, []string{})

	if exitCode != 42 {
		t.Errorf("Expected exit code 42, got %d", exitCode)
	}
}

// BenchmarkParseRunArgs benchmarks argument parsing.
func BenchmarkParseRunArgs(b *testing.B) {
	args := []string{"main.go", "helper.go", "arg1", "arg2", "--flag=value"}

	for i := 0; i < b.N; i++ {
		_, _, _ = parseRunArgs(args)
	}
}

// BenchmarkBuildTemporary benchmarks temporary build.
func BenchmarkBuildTemporary(b *testing.B) {
	// Create test source once
	tempDir, err := os.MkdirTemp("", "bench-build-*")
	if err != nil {
		b.Fatal(err)
	}
	defer os.RemoveAll(tempDir)

	testSource := `package main
func main() { println("test") }
`
	testFile := filepath.Join(tempDir, "main.go")
	if err := os.WriteFile(testFile, []byte(testSource), 0644); err != nil {
		b.Fatal(err)
	}

	b.ResetTimer()

	for i := 0; i < b.N; i++ {
		config := &buildConfig{
			sourceFiles: []string{testFile},
			workDir:     tempDir,
		}

		binaryPath, err := buildTemporary(config)
		if err != nil {
			b.Fatal(err)
		}
		os.Remove(binaryPath)
	}
}
