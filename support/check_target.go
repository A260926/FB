package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
)

func main() {
	if _, err := os.Stat("capture_session_test.go"); err != nil {
		fmt.Fprintln(os.Stderr, "required new file capture_session_test.go is missing")
		os.Exit(1)
	}
	wanted := []string{"TestCaptureSessionResume", "TestCaptureSessionDelivery", "TestCaptureSessionStop", "TestTrimHeader", "TestEventLog"}
	seen := make(map[string]int)
	bad := false
	cmd := exec.Command("go", "test", "-mod=readonly", "-json", "-race", "-count=1", "-timeout=45s", "-run", "^(TestCaptureSessionResume|TestCaptureSessionDelivery|TestCaptureSessionStop|TestTrimHeader|TestEventLog)$", ".")
	cmd.Stderr = os.Stderr
	out, err := cmd.StdoutPipe()
	if err != nil {
		panic(err)
	}
	if err = cmd.Start(); err != nil {
		panic(err)
	}
	scanner := bufio.NewScanner(out)
	scanner.Buffer(make([]byte, 4096), 4<<20)
	for scanner.Scan() {
		var ev struct{ Action, Test, Output string }
		if err := json.Unmarshal(scanner.Bytes(), &ev); err != nil {
			bad = true
			continue
		}
		if ev.Output != "" {
			fmt.Print(ev.Output)
		}
		if ev.Action == "skip" || ev.Action == "fail" {
			bad = true
		}
		if ev.Action == "pass" {
			seen[ev.Test]++
		}
	}
	scanErr := scanner.Err()
	waitErr := cmd.Wait()
	if scanErr != nil || waitErr != nil {
		bad = true
	}
	for _, name := range wanted {
		if seen[name] != 1 {
			fmt.Fprintf(os.Stderr, "required test %s: actual passes=%d\n", name, seen[name])
			bad = true
		}
	}
	if bad {
		fmt.Fprintln(os.Stderr, "target checks failed or required tests were not executed")
		os.Exit(1)
	}
	fmt.Println("TARGET_OK required=5 skips=0")
}
