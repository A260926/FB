// fuzzsession_demo exercises the keyed FuzzController end to end against the
// bundled benign FuzzEcho: one admission, real `go test -fuzz` through the
// controller, nil sink. On success its only stdout line is the session
// summary; anything that goes wrong goes to stderr with a non-zero exit.
package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"

	"github.com/mvrahden/go-test/internal/gotestrunner"
)

func main() {
	dir, err := resolveBenignDir()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}

	session := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{
		Jobs:       1,
		BuildFlags: []string{"-fuzztime=20x", "-parallel=1"},
	}, nil)

	ticket, err := session.Submit("echo", gotestrunner.FuzzTarget{
		Package: ".",
		Dir:     dir,
		Func:    "FuzzEcho",
		Suite:   "Echo",
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}

	result := session.Wait()

	exit := 0
	for _, a := range result.Attempts {
		if code := a.Outcome.EffectiveExitCode(); code > exit {
			exit = code
		}
	}
	if len(result.Attempts) != 1 || result.Attempts[0].Attempt != ticket || exit != 0 {
		fmt.Fprintf(os.Stderr, "fuzzsession_demo: unexpected result: %+v\n", result.Attempts)
		os.Exit(1)
	}

	fmt.Printf("fuzz-session generations=%d exit=%d\n", len(result.Attempts), exit)
}

// resolveBenignDir locates task_support/benign from the working directory, or,
// when the demo binary is run from elsewhere, relative to the executable
// (the build lands in <repo>/.task-build, one level below the module root).
func resolveBenignDir() (string, error) {
	candidates := []string{filepath.Join("task_support", "benign")}
	if exe, err := os.Executable(); err == nil {
		candidates = append(candidates, filepath.Join(filepath.Dir(exe), "..", "task_support", "benign"))
	}
	for _, cand := range candidates {
		dir, err := filepath.Abs(cand)
		if err != nil {
			continue
		}
		if info, err := os.Stat(filepath.Join(dir, "echo_test.go")); err == nil && !info.IsDir() {
			return dir, nil
		}
	}
	return "", fmt.Errorf("fuzzsession_demo: bundled benign module task_support/benign not found")
}
