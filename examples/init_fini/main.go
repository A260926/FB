package main

import (
	"fmt"
	"time"

	"github.com/kolkov/racedetector/internal/race/api"
)

// This example demonstrates the Init/Fini lifecycle of the race detector.
// It shows how to properly initialize the detector, use it, and get a summary report.

func main() {
	// Initialize the race detector.
	// This must be called before any memory accesses are tracked.
	api.Init()

	// Defer Fini() to ensure the summary report is printed when the program exits.
	defer api.Fini()

	fmt.Println("Race Detector Example: Init/Fini Lifecycle")
	fmt.Println("==========================================")

	// Example 1: Safe concurrent access with synchronization
	fmt.Println("\n1. Safe concurrent access (no race expected):")
	safeExample()

	// Example 2: Using Enable/Disable
	fmt.Println("\n2. Temporarily disabling race detection:")
	disableExample()

	// Example 3: Getting race statistics
	fmt.Println("\n3. Race statistics:")
	fmt.Printf("   Races detected so far: %d\n", api.RacesDetected())

	fmt.Println("\n==========================================")
	fmt.Println("Program completed. See race detector report below:")

	// When main() exits, defer will call api.Fini()
	// which prints the final race detection summary.
}

// safeExample demonstrates safe concurrent access with proper synchronization.
func safeExample() {
	var counter int
	done := make(chan bool)

	// Start goroutine that increments counter
	go func() {
		counter = 1
		done <- true
	}()

	// Wait for goroutine to complete (synchronization point)
	<-done

	// This read is safe - happens-after the write due to channel synchronization
	fmt.Printf("   Counter value: %d (safe)\n", counter)
}

// disableExample shows how to temporarily disable race detection.
func disableExample() {
	fmt.Println("   Disabling race detector...")
	api.Disable()

	// Do some operations while detector is disabled
	x := 42
	_ = x

	time.Sleep(10 * time.Millisecond)

	fmt.Println("   Re-enabling race detector...")
	api.Enable()
}
