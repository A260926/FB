// A strict go test JSON runner: zero matches, skips, and missing target files fail.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"strings"
)

func main() {
	required := []string{"TestNackRetryCommit", "TestNackWindowIdentity", "TestNackRebindCompletion", "TestNackRetrySaturation"}
	if _, err := os.Stat("pkg/nack/retry_lifecycle_test.go"); err != nil {
		fmt.Fprintln(os.Stderr, "missing required new test file: pkg/nack/retry_lifecycle_test.go")
		os.Exit(1)
	}
	args := []string{"test", "-mod=readonly", "-race", "-json", "-count=1", "-timeout=60s", "-run", "^(" + strings.Join(required, "|") + "|TestReceivedBuffer|TestGeneratorInterceptor.*|TestResponderInterceptor.*)$", "./pkg/nack"}
	cmd := exec.Command("go", args...)
	cmd.Stderr = os.Stderr
	out, err := cmd.StdoutPipe()
	if err != nil {
		panic(err)
	}
	if err = cmd.Start(); err != nil {
		panic(err)
	}
	run, pass := map[string]bool{}, map[string]bool{}
	skipped := false
	scan := bufio.NewScanner(out)
	scan.Buffer(make([]byte, 4096), 1024*1024)
	for scan.Scan() {
		var e struct{ Action, Test, Output string }
		if err := json.Unmarshal(scan.Bytes(), &e); err != nil {
			fmt.Fprintln(os.Stderr, err)
			continue
		}
		if e.Output != "" {
			fmt.Print(e.Output)
		}
		if e.Action == "run" {
			run[e.Test] = true
		}
		if e.Action == "pass" {
			pass[e.Test] = true
		}
		if e.Action == "skip" {
			skipped = true
		}
	}
	processErr := cmd.Wait()
	if scan.Err() != nil || processErr != nil || skipped {
		fmt.Fprintln(os.Stderr, "target test process failed or skipped", processErr, scan.Err())
		os.Exit(1)
	}
	for _, name := range required {
		if !run[name] || !pass[name] {
			fmt.Fprintln(os.Stderr, "required test did not execute and pass:", name)
			os.Exit(1)
		}
	}
	fmt.Println("TARGET_OK: four required tests executed; no skipped tests")
}
