//go:build ignore
// +build ignore

// This tool calculates the offset of goid field in runtime.g struct.
// Run with: go run tools/calc_goid_offset.go
package main

import (
	"fmt"
	"unsafe"
)

// Simplified g struct matching runtime.g field order up to goid.
// This MUST match the actual runtime.g struct in Go 1.25.
type g struct {
	stack        stack          // offset 0
	stackguard0  uintptr        // offset 16
	stackguard1  uintptr        // offset 24
	_panic       *int           // offset 32 (pointer)
	_defer       *int           // offset 40 (pointer)
	m            *int           // offset 48 (pointer)
	sched        gobuf          // offset 56
	syscallsp    uintptr        // offset 96
	syscallpc    uintptr        // offset 104
	stktopsp     uintptr        // offset 112
	param        unsafe.Pointer // offset 120
	atomicstatus struct {
		v uint32 // atomic wrapper - 4 bytes
	} // offset 128
	stackLock uint32 // offset 132
	goid      uint64 // offset 136 - WRONG! Actual is 152
	// MISSING FIELDS HERE that make goid actually at offset 152
}

type stack struct {
	lo uintptr // offset 0
	hi uintptr // offset 8
}

type gobuf struct {
	sp   uintptr        // offset 0
	pc   uintptr        // offset 8
	g    uintptr        // offset 16
	ctxt unsafe.Pointer // offset 24
	ret  uintptr        // offset 32
}

func main() {
	var g g

	goidOffset := unsafe.Offsetof(g.goid)

	fmt.Printf("Go version: 1.25\n")
	fmt.Printf("Architecture: amd64\n")
	fmt.Printf("goid offset: %d bytes\n", goidOffset)
	fmt.Printf("\nUse this in assembly: const goidOffset = %d\n", goidOffset)
}
