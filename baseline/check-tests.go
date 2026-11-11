// Check go test JSON for actual, non-skipped executions of named tests.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
)

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: check-tests report.jsonl TestName...")
		os.Exit(2)
	}
	f, err := os.Open(os.Args[1])
	if err != nil {
		panic(err)
	}
	defer f.Close()
	runs, passes := map[string]int{}, map[string]int{}
	s := bufio.NewScanner(f)
	s.Buffer(make([]byte, 4096), 4<<20)
	for s.Scan() {
		var e struct{ Action, Test string }
		if err := json.Unmarshal(s.Bytes(), &e); err != nil {
			panic(err)
		}
		if e.Action == "fail" || e.Action == "skip" {
			fmt.Fprintln(os.Stderr, "test failed or skipped:", e.Test)
			os.Exit(1)
		}
		if e.Action == "run" {
			runs[e.Test]++
		}
		if e.Action == "pass" {
			passes[e.Test]++
		}
	}
	if err := s.Err(); err != nil {
		panic(err)
	}
	for _, name := range os.Args[2:] {
		if runs[name] != 1 || passes[name] != 1 {
			fmt.Fprintln(os.Stderr, "missing successful execution:", name)
			os.Exit(1)
		}
	}
	fmt.Printf("TARGET_CONFIRMED tests=%d\n", len(os.Args)-2)
}
