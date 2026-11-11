package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
)

func main() {
	f, err := os.Open(os.Args[1])
	if err != nil {
		panic(err)
	}
	defer f.Close()
	names := []string{"TestDefragmentRecoveryGroups", "TestDefragmentRecoveryOverlap",
		"TestDefragmentRecoveryHybrid", "TestDefragmentRecoveryReject"}
	run, pass := map[string]bool{}, map[string]bool{}
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		var e struct{ Action, Test string }
		if err := json.Unmarshal(scanner.Bytes(), &e); err != nil {
			panic(err)
		}
		switch e.Action {
		case "run":
			run[e.Test] = true
		case "pass":
			pass[e.Test] = true
		case "skip", "fail":
			panic("target suite contains skipped or failed case: " + e.Test)
		}
	}
	if err := scanner.Err(); err != nil {
		panic(err)
	}
	for _, name := range names {
		if !run[name] || !pass[name] {
			panic("required executed PASS missing: " + name)
		}
	}
	fmt.Println("executed target tests: 4 PASS, 0 SKIP")
}
