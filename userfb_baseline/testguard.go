// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

// A test-process guard. It contains no hint generation implementation.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strings"
)

type event struct {
	Action string
	Test   string
	Output string
}

func run(pkg string, tests []string) error {
	pattern := "^(" + strings.Join(tests, "|") + ")$"
	cmd := exec.Command("go", "test", "-json", "-short", "-count=1", "-timeout=90s", "-run", pattern, pkg)
	cmd.Stderr = os.Stderr
	pipe, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	if err := cmd.Start(); err != nil {
		return err
	}
	ran, passed := make(map[string]bool), make(map[string]bool)
	skipped := false
	var decodeErr error
	scanner := bufio.NewScanner(pipe)
	scanner.Buffer(make([]byte, 4096), 8<<20)
	for scanner.Scan() {
		var e event
		if err := json.Unmarshal(scanner.Bytes(), &e); err != nil {
			decodeErr = err
			continue
		}
		if e.Output != "" {
			fmt.Print(e.Output)
		}
		if e.Action == "run" {
			ran[e.Test] = true
		}
		if e.Action == "pass" {
			passed[e.Test] = true
		}
		if e.Action == "skip" && e.Test != "" {
			skipped = true
		}
	}
	scanErr := scanner.Err()
	_, drainErr := io.Copy(io.Discard, pipe)
	processErr := cmd.Wait()
	if scanErr != nil {
		return scanErr
	}
	if drainErr != nil {
		return drainErr
	}
	if decodeErr != nil {
		return decodeErr
	}
	if processErr != nil {
		return processErr
	}
	if skipped {
		return fmt.Errorf("skipped test in %s", pkg)
	}
	for _, name := range tests {
		if !ran[name] || !passed[name] {
			return fmt.Errorf("required test did not run and pass: %s %s", pkg, name)
		}
	}
	fmt.Printf("GUARD verified %s: %d required tests ran and passed\n", pkg, len(tests))
	return nil
}

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: testguard package TestName...")
		os.Exit(2)
	}
	if err := run(os.Args[1], os.Args[2:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
