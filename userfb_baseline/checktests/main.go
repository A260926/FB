package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"strings"
)

func main() {
	baseline := len(os.Args) == 2 && os.Args[1] == "--baseline"
	if len(os.Args) > 1 && !baseline {
		fmt.Fprintln(os.Stderr, "usage: checktests [--baseline]")
		os.Exit(2)
	}
	wanted := []string{"TestCustomDispatchUserFB", "TestCustomContinuationUserFB"}
	args := []string{"test", "-v", "-json", "-count=1"}
	marker := "CUSTOM_DISPATCH_TARGET_TESTS_PASS"
	if baseline {
		wanted = []string{"TestStruct_fuzzing1", "TestGetUint32"}
		args = append(args, "./...")
		marker = "BASELINE_GO_OLD_TESTS_PASS"
	} else {
		args = append(args, "-run", "^TestCustom(Dispatch|Continuation)UserFB$", ".")
	}
	cmd := exec.Command("go", args...)
	output, err := cmd.CombinedOutput()
	os.Stdout.Write(output)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	ran, passed := map[string]bool{}, map[string]bool{}
	scanner := bufio.NewScanner(bytes.NewReader(output))
	scanner.Buffer(make([]byte, 4096), 8*1024*1024)
	for scanner.Scan() {
		var event struct{ Action, Test string }
		if json.Unmarshal(scanner.Bytes(), &event) != nil {
			continue
		}
		if event.Action == "run" {
			ran[event.Test] = true
		}
		if event.Action == "pass" {
			passed[event.Test] = true
		}
	}
	if err := scanner.Err(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	var missing []string
	for _, name := range wanted {
		if !ran[name] || !passed[name] {
			missing = append(missing, name)
		}
	}
	if len(missing) != 0 {
		fmt.Fprintln(os.Stderr, "Required tests did not run and PASS: "+strings.Join(missing, ","))
		os.Exit(1)
	}
	fmt.Println(marker)
}
