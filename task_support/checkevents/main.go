package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
)

type event struct{ Action, Test, Output string }

func fail(s string) { fmt.Fprintln(os.Stderr, "TARGET_REJECTED", s); os.Exit(1) }
func main() {
	if len(os.Args) != 2 {
		fail("event path required")
	}
	f, e := os.Open(os.Args[1])
	if e != nil {
		panic(e)
	}
	defer f.Close()
	roots := []string{"TestAnalyticRegions", "TestAnalyticIntegrals", "TestAnalyticPipeline"}
	passed := map[string]bool{}
	assertions := map[string]int{}
	packagePass := false
	d := json.NewDecoder(f)
	for {
		var v event
		e = d.Decode(&v)
		if e == io.EOF {
			break
		}
		if e != nil {
			fail(e.Error())
		}
		if v.Action == "fail" || v.Action == "skip" {
			fail(v.Action + " " + v.Test)
		}
		if v.Action == "pass" {
			if v.Test == "" {
				packagePass = true
			} else {
				passed[v.Test] = true
			}
		}
		if v.Action == "output" && strings.Contains(v.Output, "UNIT_ASSERTIONS ") {
			s := strings.Split(v.Output, "UNIT_ASSERTIONS ")
			n, e := strconv.Atoi(strings.TrimSpace(s[len(s)-1]))
			if e != nil || n < 1 {
				fail("empty assertion count")
			}
			assertions[v.Test] += n
		}
	}
	if !packagePass {
		fail("no final package PASS")
	}
	for _, r := range roots {
		if !passed[r] {
			fail("missing group " + r)
		}
		n, total := 0, 0
		for name := range passed {
			if strings.HasPrefix(name, r+"/") && strings.Count(name, "/") == 1 {
				if assertions[name] < 1 {
					fail("no counted assertion " + name)
				}
				n++
				total += assertions[name]
			}
		}
		if n < 2 {
			fail("fewer than two named subtests in " + r)
		}
		fmt.Printf("TARGET_GROUP %s subtests=%d assertions=%d\n", r, n, total)
	}
	fmt.Println("ANALYTIC_CASES_OK")
}
