// Package main demonstrates race report formatting.
//
// This example shows how race reports are formatted to match Go's official
// race detector output format.
package main

import (
	"fmt"
	"unsafe"

	"github.com/kolkov/racedetector/internal/race/detector"
	"github.com/kolkov/racedetector/internal/race/epoch"
	"github.com/kolkov/racedetector/internal/race/goroutine"
)

func main() {
	fmt.Println("=== Race Report Formatting Demo ===")

	// Initialize detector
	d := detector.NewDetector()

	// Simulate two goroutines racing on the same variable
	addr := uintptr(unsafe.Pointer(new(int)))

	// Goroutine 1: Write to variable
	ctx1 := goroutine.Alloc(1)
	ctx1.Epoch = epoch.NewEpoch(1, 10)
	ctx1.C.Set(1, 10)
	d.OnWrite(addr, ctx1)

	// Goroutine 2: Concurrent write without synchronization (RACE!)
	ctx2 := goroutine.Alloc(2)
	ctx2.Epoch = epoch.NewEpoch(2, 20)
	ctx2.C.Set(2, 20)
	d.OnWrite(addr, ctx2) // This will trigger a race report

	fmt.Println("\n=== Demo Complete ===")
	fmt.Printf("Total races detected: %d\n", d.RacesDetected())
	fmt.Println("\nNote: This example demonstrates the basic race report format")
}
