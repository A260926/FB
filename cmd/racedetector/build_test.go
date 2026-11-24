// build_test.go tests the 'racedetector build' command.
package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestParseBuildArgs_SimpleFile tests parsing a single source file.
func TestParseBuildArgs_SimpleFile(t *testing.T) {
	args := []string{"main.go"}

	config, err := parseBuildArgs(args)
	if err != nil {
		t.Fatalf("parseBuildArgs() error: %v", err)
	}

	if len(config.sourceFiles) != 1 {
		t.Errorf("Expected 1 source file, got %d", len(config.sourceFiles))
	}

	if config.sourceFiles[0] != "main.go" {
		t.Errorf("Expected main.go, got %s", config.sourceFiles[0])
	}

	if config.outputFile != "" {
		t.Errorf("Expected no output file, got %s", config.outputFile)
	}
}

// TestParseBuildArgs_MultipleFiles tests parsing multiple source files.
func TestParseBuildArgs_MultipleFiles(t *testing.T) {
	args := []string{"main.go", "helper.go", "utils.go"}

	config, err := parseBuildArgs(args)
	if err != nil {
		t.Fatalf("parseBuildArgs() error: %v", err)
	}

	if len(config.sourceFiles) != 3 {
		t.Errorf("Expected 3 source files, got %d", len(config.sourceFiles))
	}
}

// TestParseBuildArgs_OutputFlag tests -o flag parsing.
func TestParseBuildArgs_OutputFlag(t *testing.T) {
	tests := []struct {
		name   string
		args   []string
		output string
	}{
		{
			name:   "dash o space",
			args:   []string{"-o", "myapp", "main.go"},
			output: "myapp",
		},
		{
			name:   "dash o equals",
			args:   []string{"-o=myapp", "main.go"},
			output: "myapp",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			config, err := parseBuildArgs(tt.args)
			if err != nil {
				t.Fatalf("parseBuildArgs() error: %v", err)
			}

			if config.outputFile != tt.output {
				t.Errorf("Expected output %q, got %q", tt.output, config.outputFile)
			}
		})
	}
}

// TestParseBuildArgs_BuildFlags tests go build flag parsing.
func TestParseBuildArgs_BuildFlags(t *testing.T) {
	args := []string{
		"-ldflags", "-s -w",
		"-tags", "production",
		"main.go",
	}

	config, err := parseBuildArgs(args)
	if err != nil {
		t.Fatalf("parseBuildArgs() error: %v", err)
	}

	// Should have 4 build flags
	if len(config.buildFlags) != 4 {
		t.Errorf("Expected 4 build flags, got %d: %v", len(config.buildFlags), config.buildFlags)
	}

	// Check flags are preserved in order
	expected := []string{"-ldflags", "-s -w", "-tags", "production"}
	for i, flag := range expected {
		if config.buildFlags[i] != flag {
			t.Errorf("Flag %d: expected %q, got %q", i, flag, config.buildFlags[i])
		}
	}
}

// TestParseBuildArgs_CurrentDirectory tests "." as source.
func TestParseBuildArgs_CurrentDirectory(t *testing.T) {
	args := []string{"."}

	config, err := parseBuildArgs(args)
	if err != nil {
		t.Fatalf("parseBuildArgs() error: %v", err)
	}

	if len(config.sourceFiles) != 1 || config.sourceFiles[0] != "." {
		t.Errorf("Expected [\".\"], got %v", config.sourceFiles)
	}
}

// TestParseBuildArgs_NoArgs tests default behavior with no arguments.
func TestParseBuildArgs_NoArgs(t *testing.T) {
	args := []string{}

	config, err := parseBuildArgs(args)
	if err != nil {
		t.Fatalf("parseBuildArgs() error: %v", err)
	}

	// Should default to current directory
	if len(config.sourceFiles) != 1 || config.sourceFiles[0] != "." {
		t.Errorf("Expected default [\".\"], got %v", config.sourceFiles)
	}
}

// TestParseBuildArgs_ComplexCommand tests complex real-world command.
func TestParseBuildArgs_ComplexCommand(t *testing.T) {
	args := []string{
		"-o", "myapp",
		"-ldflags", "-s -w",
		"-tags", "production,linux",
		"-gcflags", "-N -l",
		"main.go",
		"server.go",
	}

	config, err := parseBuildArgs(args)
	if err != nil {
		t.Fatalf("parseBuildArgs() error: %v", err)
	}

	// Check output
	if config.outputFile != "myapp" {
		t.Errorf("Expected output 'myapp', got %q", config.outputFile)
	}

	// Check source files
	if len(config.sourceFiles) != 2 {
		t.Errorf("Expected 2 source files, got %d", len(config.sourceFiles))
	}

	// Check build flags
	expectedFlags := []string{"-ldflags", "-s -w", "-tags", "production,linux", "-gcflags", "-N -l"}
	if len(config.buildFlags) != len(expectedFlags) {
		t.Errorf("Expected %d build flags, got %d", len(expectedFlags), len(config.buildFlags))
	}
}

// TestCreateWorkspace tests workspace creation.
func TestCreateWorkspace(t *testing.T) {
	ws, err := createWorkspace()
	if err != nil {
		t.Fatalf("createWorkspace() error: %v", err)
	}
	defer ws.cleanup()

	// Check workspace directory exists
	if _, err := os.Stat(ws.dir); os.IsNotExist(err) {
		t.Errorf("Workspace directory %s does not exist", ws.dir)
	}

	// Check src subdirectory exists
	if _, err := os.Stat(ws.srcDir); os.IsNotExist(err) {
		t.Errorf("Workspace src directory %s does not exist", ws.srcDir)
	}

	// Check directory is in temp
	if !strings.Contains(ws.dir, "racedetector-build-") {
		t.Errorf("Workspace directory name doesn't match pattern: %s", ws.dir)
	}
}

// TestWorkspaceCleanup tests workspace cleanup.
func TestWorkspaceCleanup(t *testing.T) {
	ws, err := createWorkspace()
	if err != nil {
		t.Fatalf("createWorkspace() error: %v", err)
	}

	dir := ws.dir

	// Cleanup
	ws.cleanup()

	// Directory should be gone
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Errorf("Workspace directory %s still exists after cleanup", dir)
	}
}

// TestCollectGoFiles tests Go file collection.
func TestCollectGoFiles(t *testing.T) {
	// Create temp test directory
	tempDir, err := os.MkdirTemp("", "test-collect-*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	// Create test files
	testFiles := []string{
		"main.go",
		"server.go",
		"utils.go",
		"main_test.go", // Should be excluded in build
		"README.md",    // Not a .go file
	}

	for _, name := range testFiles {
		path := filepath.Join(tempDir, name)
		if err := os.WriteFile(path, []byte("package main"), 0644); err != nil {
			t.Fatalf("Failed to create test file %s: %v", name, err)
		}
	}

	// Test collecting from directory
	files, err := collectGoFiles([]string{tempDir}, "")
	if err != nil {
		t.Fatalf("collectGoFiles() error: %v", err)
	}

	// Should find 3 .go files (excluding _test.go)
	if len(files) != 3 {
		t.Errorf("Expected 3 .go files, got %d: %v", len(files), files)
	}

	// Check files are .go and not _test.go
	for _, file := range files {
		if !strings.HasSuffix(file, ".go") {
			t.Errorf("Non-.go file found: %s", file)
		}
		if strings.HasSuffix(file, "_test.go") {
			t.Errorf("Test file should be excluded: %s", file)
		}
	}
}

// TestCollectGoFiles_SingleFile tests collecting a single file.
func TestCollectGoFiles_SingleFile(t *testing.T) {
	// Create temp file
	tempDir, err := os.MkdirTemp("", "test-single-*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	testFile := filepath.Join(tempDir, "main.go")
	if err := os.WriteFile(testFile, []byte("package main"), 0644); err != nil {
		t.Fatalf("Failed to create test file: %v", err)
	}

	// Collect single file
	files, err := collectGoFiles([]string{testFile}, "")
	if err != nil {
		t.Fatalf("collectGoFiles() error: %v", err)
	}

	if len(files) != 1 {
		t.Errorf("Expected 1 file, got %d", len(files))
	}

	if files[0] != testFile {
		t.Errorf("Expected %s, got %s", testFile, files[0])
	}
}

// TestCollectGoFiles_EmptyDirectory tests empty directory handling.
func TestCollectGoFiles_EmptyDirectory(t *testing.T) {
	// Create empty temp directory
	tempDir, err := os.MkdirTemp("", "test-empty-*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	// Should return empty list, not error
	files, err := collectGoFiles([]string{tempDir}, "")
	if err != nil {
		t.Fatalf("collectGoFiles() error: %v", err)
	}

	if len(files) != 0 {
		t.Errorf("Expected 0 files from empty directory, got %d", len(files))
	}
}

// TestCollectGoFiles_NonExistent tests non-existent path handling.
func TestCollectGoFiles_NonExistent(t *testing.T) {
	_, err := collectGoFiles([]string{"/nonexistent/path/file.go"}, "")
	if err == nil {
		t.Error("Expected error for non-existent path, got nil")
	}
}

// TestNeedsValue tests flag value detection.
func TestNeedsValue(t *testing.T) {
	tests := []struct {
		flag     string
		expected bool
	}{
		{"-ldflags", true},
		{"-gcflags", true},
		{"-tags", true},
		{"-o", false}, // Handled separately
		{"-v", false},
		{"-x", false},
		{"-ldflags=-s -w", false}, // Already has =
		{"-race", false},
	}

	for _, tt := range tests {
		t.Run(tt.flag, func(t *testing.T) {
			result := needsValue(tt.flag)
			if result != tt.expected {
				t.Errorf("needsValue(%q) = %v, want %v", tt.flag, result, tt.expected)
			}
		})
	}
}

// TestInstrumentSources tests source file instrumentation.
func TestInstrumentSources(t *testing.T) {
	// Create temp directory with test source
	tempDir, err := os.MkdirTemp("", "test-instrument-*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	// Create test source file
	testSource := `package main

func main() {
	x := 42
	println(x)
}
`
	testFile := filepath.Join(tempDir, "main.go")
	if err := os.WriteFile(testFile, []byte(testSource), 0644); err != nil {
		t.Fatalf("Failed to create test file: %v", err)
	}

	// Create workspace
	ws, err := createWorkspace()
	if err != nil {
		t.Fatalf("createWorkspace() error: %v", err)
	}
	defer ws.cleanup()

	// Create config
	config := &buildConfig{
		sourceFiles: []string{testFile},
		workDir:     tempDir,
	}

	// Instrument sources
	if err := instrumentSources(config, ws); err != nil {
		t.Fatalf("instrumentSources() error: %v", err)
	}

	// Check instrumented file was created
	instrumentedPath := filepath.Join(ws.srcDir, "main.go")
	content, err := os.ReadFile(instrumentedPath)
	if err != nil {
		t.Fatalf("Failed to read instrumented file: %v", err)
	}

	contentStr := string(content)

	// Should have race import (public API for standalone tool)
	if !strings.Contains(contentStr, "github.com/kolkov/racedetector/race") {
		t.Error("Instrumented file missing race detector import")
	}

	// Should have unsafe import
	if !strings.Contains(contentStr, "unsafe") {
		t.Error("Instrumented file missing unsafe import")
	}
}

// TestInstrumentSources_NoFiles tests error handling for no source files.
func TestInstrumentSources_NoFiles(t *testing.T) {
	ws, err := createWorkspace()
	if err != nil {
		t.Fatalf("createWorkspace() error: %v", err)
	}
	defer ws.cleanup()

	config := &buildConfig{
		sourceFiles: []string{},
		workDir:     ".",
	}

	err = instrumentSources(config, ws)
	if err == nil {
		t.Error("Expected error for no source files, got nil")
	}

	if !strings.Contains(err.Error(), "no Go source files") {
		t.Errorf("Expected 'no Go source files' error, got: %v", err)
	}
}

// BenchmarkParseBuildArgs benchmarks argument parsing.
func BenchmarkParseBuildArgs(b *testing.B) {
	args := []string{"-o", "myapp", "-ldflags", "-s -w", "main.go", "server.go"}

	for i := 0; i < b.N; i++ {
		_, _ = parseBuildArgs(args)
	}
}

// BenchmarkCreateWorkspace benchmarks workspace creation.
func BenchmarkCreateWorkspace(b *testing.B) {
	for i := 0; i < b.N; i++ {
		ws, err := createWorkspace()
		if err != nil {
			b.Fatal(err)
		}
		ws.cleanup()
	}
}
