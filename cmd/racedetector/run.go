// run.go implements the 'racedetector run' command.
package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

// runCommand implements the 'racedetector run' command.
//
// This command instruments Go source files, builds them temporarily,
// and immediately executes the resulting binary with race detection.
// It acts as a drop-in replacement for 'go run'.
//
// Flow:
//  1. Parse arguments (source files + program arguments)
//  2. Build instrumented binary to temp location
//  3. Execute binary with program arguments
//  4. Forward stdin/stdout/stderr
//  5. Return program's exit code
//
// Example:
//
//	racedetector run main.go
//	racedetector run main.go arg1 arg2
//	racedetector run main.go --program-flag=value
func runCommand(args []string) {
	// Parse arguments: separate source files from program args
	config, programArgs, err := parseRunArgs(args)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	// Build instrumented binary to temporary location
	tempBinary, err := buildTemporary(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Build failed: %v\n", err)
		os.Exit(1)
	}
	defer func() { _ = os.Remove(tempBinary) }() // Best effort cleanup

	// Execute the binary with program arguments
	exitCode := executeBinary(tempBinary, programArgs)
	os.Exit(exitCode)
}

// parseRunArgs separates source files from program arguments.
//
// The 'go run' command format is:
//
//	go run [build flags] [-exec xprog] package [arguments...]
//
// For MVP, we support:
//
//	racedetector run file.go [arguments...]
//	racedetector run file1.go file2.go [arguments...]
//
// Build flags (if any) come before source files.
// Everything after source files are program arguments.
//
// Returns:
//   - buildConfig for compilation
//   - programArgs to pass to executable
//   - error if parsing fails
func parseRunArgs(args []string) (*buildConfig, []string, error) {
	if len(args) == 0 {
		return nil, nil, fmt.Errorf("no source files specified")
	}

	// Find where source files end and program args begin
	// Strategy: first non-.go file after seeing at least one .go file
	var sourceFiles []string
	var programArgs []string
	var buildFlags []string

	sawGoFile := false
	inProgramArgs := false

	for i := 0; i < len(args); i++ {
		arg := args[i]

		// If we're already in program args, collect everything
		if inProgramArgs {
			programArgs = append(programArgs, arg)
			continue
		}

		// Build flags come before source files
		if !sawGoFile && (arg == "-o" || arg == "-ldflags" || arg == "-gcflags" ||
			arg == "-tags" || arg == "-buildmode") {
			buildFlags = append(buildFlags, arg)
			// These flags take a value
			if i+1 < len(args) {
				i++
				buildFlags = append(buildFlags, args[i])
			}
			continue
		}

		// Is it a .go file?
		if filepath.Ext(arg) == ".go" {
			sourceFiles = append(sourceFiles, arg)
			sawGoFile = true
			continue
		}

		// Not a .go file and we've seen .go files → program args start here
		if sawGoFile {
			inProgramArgs = true
			programArgs = append(programArgs, arg)
			continue
		}

		// Not a .go file and haven't seen .go files → could be build flag
		buildFlags = append(buildFlags, arg)
	}

	if len(sourceFiles) == 0 {
		return nil, nil, fmt.Errorf("no Go source files specified")
	}

	// Get current working directory
	cwd, err := os.Getwd()
	if err != nil {
		return nil, nil, fmt.Errorf("failed to get working directory: %w", err)
	}

	config := &buildConfig{
		sourceFiles: sourceFiles,
		buildFlags:  buildFlags,
		workDir:     cwd,
		outputFile:  "", // Will be set by buildTemporary
	}

	return config, programArgs, nil
}

// buildTemporary builds the instrumented code to a temporary binary.
//
// This creates a unique temporary executable that will be deleted after run.
//
// Returns:
//   - Path to temporary binary
//   - Error if build fails
func buildTemporary(config *buildConfig) (string, error) {
	// Create temporary binary name
	tempBinary, err := os.CreateTemp("", "racedetector-run-*.exe")
	if err != nil {
		return "", fmt.Errorf("failed to create temp file: %w", err)
	}
	tempPath := tempBinary.Name()
	_ = tempBinary.Close() // Ignore close error on temp file

	// Set output path in config
	config.outputFile = tempPath

	// Reuse build command logic
	// Validate runtime is available
	if err := buildValidateRuntime(); err != nil {
		return "", err
	}

	// Create workspace
	workspace, err := createWorkspace()
	if err != nil {
		_ = os.Remove(tempPath) // Cleanup on error, ignore removal errors
		return "", fmt.Errorf("failed to create workspace: %w", err)
	}
	defer workspace.cleanup()

	// Instrument sources
	if err := instrumentSources(config, workspace); err != nil {
		_ = os.Remove(tempPath) // Cleanup on error, ignore removal errors
		return "", fmt.Errorf("failed to instrument sources: %w", err)
	}

	// Setup runtime linking
	if err := workspace.setupRuntimeLinking(); err != nil {
		_ = os.Remove(tempPath) // Cleanup on error, ignore removal errors
		return "", fmt.Errorf("failed to setup runtime: %w", err)
	}

	// Build
	if err := workspace.build(config); err != nil {
		_ = os.Remove(tempPath) // Cleanup on error, ignore removal errors
		return "", fmt.Errorf("build failed: %w", err)
	}

	return tempPath, nil
}

// buildValidateRuntime validates runtime availability (shared with build command).
func buildValidateRuntime() error {
	// Import from runtime package
	if err := validateRuntimeAvailable(); err != nil {
		fmt.Fprintf(os.Stderr, "Error: Race detector runtime not found\n")
		fmt.Fprintf(os.Stderr, "%v\n", err)
		fmt.Fprintf(os.Stderr, "\nPlease ensure the runtime is installed:\n")
		fmt.Fprintf(os.Stderr, "  go get github.com/kolkov/racedetector/internal/race/api\n")
		return err
	}
	return nil
}

// validateRuntimeAvailable is an alias to avoid import cycle
// (build.go uses runtime package directly)
func validateRuntimeAvailable() error {
	// This would normally import runtime.ValidateRuntimeAvailable()
	// For now, assume it's available (same logic as build command)
	return nil
}

// executeBinary runs the instrumented binary with given arguments.
//
// This forwards stdin/stdout/stderr to the child process and
// returns the process exit code.
//
// Returns:
//   - Exit code of the process (0 = success)
func executeBinary(binaryPath string, args []string) int {
	// Create command
	cmd := exec.Command(binaryPath, args...)

	// Forward streams
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr

	// Run and wait
	if err := cmd.Run(); err != nil {
		// Check if it's an exit error using errors.As (errorlint compliant)
		var exitErr *exec.ExitError
		if errors.As(err, &exitErr) {
			return exitErr.ExitCode()
		}
		// Other error (failed to start, etc.)
		fmt.Fprintf(os.Stderr, "Error executing binary: %v\n", err)
		return 1
	}

	return 0
}
