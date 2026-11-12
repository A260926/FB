// A deterministic, harmless producer using the real go-fuzz-dep IPC loop.
// Build with -tags=gofuzz. Counters stand in for instrumented blocks.
package main

import dep "github.com/dvyukov/go-fuzz/go-fuzz-dep"

var calls = map[byte]int{}

func target(data []byte) int {
	if len(data) == 0 {
		return 0
	}
	calls[data[0]]++
	switch data[0] {
	case 's':
		dep.CoverTab[1] = 1
		if len(data) > 1 {
			dep.CoverTab[2] = 1
		}
	case 'f':
		if calls['f'] != 3 {
			dep.CoverTab[3] = 1
		}
	case 'n':
		dep.CoverTab[4] = 1
		return -1
	case 'N':
		// Negative result only on the third observation.
		dep.CoverTab[5] = 1
		if calls['N'] == 3 {
			return -1
		}
	case 'c':
		if calls['c'] == 2 {
			panic("local toy replay stopped")
		}
		dep.CoverTab[6] = 1
	case 'g':
		dep.CoverTab[6] = 1
	case 'G':
		// Same block as 'g' but a different counter bucket.
		dep.CoverTab[6] = 3
	}
	return 0
}

func main() { dep.Main([]func([]byte) int{target}) }
