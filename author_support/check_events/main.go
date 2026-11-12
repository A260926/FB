package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
)

func main() {
	f, err := os.Open(os.Args[1])
	if err != nil {
		panic(err)
	}
	defer f.Close()
	wanted := map[string]int{"TestEffectiveRedirectLive": 0, "TestEffectiveRedirectCache": 0, "TestEffectiveRedirectAbort": 0, "TestEffectiveRedirectIsolation": 0}
	d := json.NewDecoder(f)
	for {
		var e struct{ Action, Test string }
		err = d.Decode(&e)
		if err == io.EOF {
			break
		}
		if err != nil {
			panic(err)
		}
		if e.Action == "skip" || e.Action == "fail" {
			fmt.Fprintln(os.Stderr, "rejected event", e.Action, e.Test)
			os.Exit(1)
		}
		if _, ok := wanted[e.Test]; ok && e.Action == "pass" {
			wanted[e.Test]++
		}
	}
	for name, n := range wanted {
		if n != 1 {
			fmt.Fprintln(os.Stderr, "expected one PASS", name, "got", n)
			os.Exit(1)
		}
	}
	fmt.Println("TARGET_TESTS_OK 4")
}
