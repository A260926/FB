package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
)

func main() {
	if len(os.Args) < 3 {
		panic("usage: check log names...")
	}
	f, e := os.Open(os.Args[1])
	if e != nil {
		panic(e)
	}
	defer f.Close()
	run, pass := map[string]int{}, map[string]int{}
	packagePass := false
	s := bufio.NewScanner(f)
	s.Buffer(make([]byte, 4096), 4<<20)
	for s.Scan() {
		var v struct{ Action, Test, Package string }
		if json.Unmarshal(s.Bytes(), &v) != nil {
			panic("invalid test JSON")
		}
		if v.Action == "skip" || v.Action == "fail" {
			panic("skip/fail forbidden")
		}
		if v.Action == "run" {
			run[v.Test]++
		}
		if v.Action == "pass" {
			if v.Test == "" {
				packagePass = true
			} else {
				pass[v.Test]++
			}
		}
	}
	if s.Err() != nil {
		panic(s.Err())
	}
	if !packagePass {
		panic("missing package completion")
	}
	for _, n := range os.Args[2:] {
		if run[n] != 1 || pass[n] != 1 {
			panic("missing unique RUN/PASS: " + n)
		}
	}
	fmt.Printf("TARGET_REGEN_COMPLETE groups=%d\n", len(os.Args)-2)
}
