package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
)

func die(message string) {
	fmt.Fprintln(os.Stderr, message)
	os.Exit(1)
}

func main() {
	if len(os.Args) != 3 || (os.Args[1] != "old" && os.Args[1] != "target") {
		die("usage: testcheck old|target results.jsonl")
	}
	mode := os.Args[1]
	required := []string{"TestRunResponse", "TestRunResponse_noResponse", "TestQueryIframe", "TestNavigateCancelled", "TestNavigateWithoutWaitingForLoad"}
	if mode == "target" {
		required = append(required, "TestFrameResponseForm", "TestFrameResponseIsolation", "TestFrameResponseExitPaths")
	}
	f, err := os.Open(os.Args[2])
	if err != nil {
		die(err.Error())
	}
	defer f.Close()
	actions := map[string]map[string]bool{}
	scan := bufio.NewScanner(f)
	scan.Buffer(make([]byte, 4096), 16*1024*1024)
	for scan.Scan() {
		var row struct{ Action, Test string }
		if len(scan.Bytes()) == 0 {
			continue
		}
		if err := json.Unmarshal(scan.Bytes(), &row); err != nil {
			die(err.Error())
		}
		if row.Action == "fail" || row.Action == "skip" {
			die("failed or skipped test: " + row.Test)
		}
		if actions[row.Test] == nil {
			actions[row.Test] = map[string]bool{}
		}
		actions[row.Test][row.Action] = true
	}
	if err := scan.Err(); err != nil {
		die(err.Error())
	}
	for _, name := range required {
		if !actions[name]["run"] || !actions[name]["pass"] {
			die("test did not run and pass: " + name)
		}
	}
	fmt.Printf("CHECKED %s required_tests=%d pass_no_skip=true\n", mode, len(required))
}
