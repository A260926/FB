// This checks test-run evidence; it contains no geometry implementation.
package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strings"
)

func main() {
	if len(os.Args) < 3 {
		panic("usage: checkrun LOG TEST...")
	}
	f, err := os.Open(os.Args[1])
	if err != nil {
		panic(err)
	}
	defer f.Close()
	want := map[string]bool{}
	for _, name := range os.Args[2:] {
		want[name] = false
	}
	d := json.NewDecoder(f)
	packagePassed := false
	for {
		var e struct{ Action, Test, Package string }
		err := d.Decode(&e)
		if err == io.EOF {
			break
		}
		if err != nil {
			panic(err)
		}
		if e.Action == "fail" {
			panic("test failure: " + e.Test)
		}
		if e.Action == "skip" && strings.HasPrefix(e.Test, "TestCSG") {
			panic("CSG test skipped: " + e.Test)
		}
		if e.Action == "pass" {
			if _, ok := want[e.Test]; ok {
				want[e.Test] = true
			}
			if e.Test == "" && e.Package == "github.com/fogleman/pt/pt" {
				packagePassed = true
			}
		}
	}
	if !packagePassed {
		panic("no package pass event")
	}
	for _, name := range os.Args[2:] {
		if !want[name] {
			panic("missing real pass event: " + name)
		}
		fmt.Println("EXECUTED_PASS " + name)
	}
}
