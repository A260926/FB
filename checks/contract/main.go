package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
)

// This runner checks that every requested future test actually ran and passed.
// It contains no upload implementation or behavioural assertions.
func main() {
	if _, err := os.Stat("remote_upload_test.go"); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	names := []string{"TestRemoteUploadWire", "TestRemoteUploadPrepare", "TestRemoteUploadFailure", "TestRemoteUploadIsolation"}
	pattern := "^(TestRemoteUploadWire|TestRemoteUploadPrepare|TestRemoteUploadFailure|TestRemoteUploadIsolation)$"
	cmd := exec.Command("sh", "checks/go.sh", "test", "-mod=readonly", "-count=1", "-json", "-run", pattern, ".")
	cmd.Stderr = os.Stderr
	pipe, err := cmd.StdoutPipe()
	if err != nil {
		panic(err)
	}
	if err := cmd.Start(); err != nil {
		panic(err)
	}
	passed, skipped := map[string]bool{}, false
	scan := bufio.NewScanner(pipe)
	scan.Buffer(make([]byte, 4096), 1024*1024)
	for scan.Scan() {
		line := scan.Bytes()
		fmt.Println(string(line))
		var event struct{ Action, Package, Test string }
		if err := json.Unmarshal(line, &event); err != nil {
			panic(err)
		}
		if event.Package == "github.com/sclevine/agouti" {
			if event.Action == "pass" {
				passed[event.Test] = true
			}
			if event.Action == "skip" {
				skipped = true
			}
		}
	}
	scanErr, runErr := scan.Err(), cmd.Wait()
	if scanErr != nil || runErr != nil || skipped {
		fmt.Fprintln(os.Stderr, "contract failed:", scanErr, runErr, "skip:", skipped)
		os.Exit(1)
	}
	for _, name := range names {
		if !passed[name] {
			fmt.Fprintln(os.Stderr, "missing passing test:", name)
			os.Exit(1)
		}
	}
	fmt.Println("CONTRACT all four named tests passed without skips")
}
