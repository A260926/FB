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
	expected := []string{"github.com/headzoo/surf/browser:TestNavigationReplay", "github.com/headzoo/surf/jar:TestHistoryReplayStorage"}
	seen := map[string]map[string]bool{}
	for _, k := range expected {
		seen[k] = map[string]bool{}
	}
	s := bufio.NewScanner(f)
	s.Buffer(make([]byte, 4096), 4<<20)
	for s.Scan() {
		var e struct{ Action, Package, Test string }
		if err := json.Unmarshal(s.Bytes(), &e); err != nil {
			panic(err)
		}
		key := e.Package + ":" + e.Test
		if actions, ok := seen[key]; ok {
			actions[e.Action] = true
		}
		if e.Action == "fail" || e.Action == "skip" {
			panic("target did not pass: " + key)
		}
	}
	if err := s.Err(); err != nil {
		panic(err)
	}
	for _, key := range expected {
		if !seen[key]["run"] || !seen[key]["pass"] {
			panic("target did not run and pass: " + key)
		}
		fmt.Println("TARGET_PASS", key)
	}
}
