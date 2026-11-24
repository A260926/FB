// Package main demonstrates stack trace capture in race reports.
//
// This example shows how stack traces are captured and displayed in race reports,
// matching Go's official race detector output format.
package main

import (
	"fmt"
	"unsafe"

	"github.com/kolkov/racedetector/internal/race/detector"
	"github.com/kolkov/racedetector/internal/race/epoch"
	"github.com/kolkov/racedetector/internal/race/goroutine"
)

func main() {
	fmt.Println("=== Stack Trace Capture Demo ===")

	// Simulate a realistic race scenario with function call chain
	simulateRace()

	fmt.Println("\n=== Demo Complete ===")
	fmt.Println("Note: Previous access stack traces require storing stack")
	fmt.Println("      traces in shadow memory (future enhancement)")
}

// simulateRace simulates a data race with a realistic call chain.
func simulateRace() {
	d := detector.NewDetector()

	// Simulate shared variable
	addr := uintptr(unsafe.Pointer(new(int)))

	// Goroutine 1: Write through helper function
	ctx1 := goroutine.Alloc(1)
	ctx1.Epoch = epoch.NewEpoch(1, 10)
	ctx1.C.Set(1, 10)
	writeData(d, addr, ctx1)

	// Goroutine 2: Concurrent write (RACE!)
	ctx2 := goroutine.Alloc(2)
	ctx2.Epoch = epoch.NewEpoch(2, 20)
	ctx2.C.Set(2, 20)
	writeDataThroughHelper(d, addr, ctx2) // This will trigger race report with stack trace
}

// writeData performs a write access (simulating business logic).
func writeData(d *detector.Detector, addr uintptr, ctx *goroutine.RaceContext) {
	d.OnWrite(addr, ctx)
}

// writeDataThroughHelper demonstrates nested function calls in stack trace.
func writeDataThroughHelper(d *detector.Detector, addr uintptr, ctx *goroutine.RaceContext) {
	helperFunction(d, addr, ctx)
}

// helperFunction adds another level to the call stack.
func helperFunction(d *detector.Detector, addr uintptr, ctx *goroutine.RaceContext) {
	d.OnWrite(addr, ctx) // Race detected here - stack trace captured
}
