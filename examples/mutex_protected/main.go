// Package main demonstrates CORRECT mutex-protected concurrent access.
//
// This program shows how to properly synchronize access to shared variables
// using sync.Mutex. When run with the race detector, it should NOT report
// any races because all accesses are properly synchronized.
//
// Usage:
//
//	racedetector run main.go
//
// Expected: No data races detected (all accesses are mutex-protected)
package main

import (
	"fmt"
	"sync"
	"time"
)

func main() {
	fmt.Println("=== Mutex-Protected Counter Demo ===")
	fmt.Println()

	// Shared variable protected by mutex
	var counter int
	var mu sync.Mutex
	var wg sync.WaitGroup

	const numGoroutines = 10
	const incrementsPerGoroutine = 100

	fmt.Printf("Starting %d goroutines, each incrementing counter %d times\n",
		numGoroutines, incrementsPerGoroutine)
	fmt.Println()

	startTime := time.Now()

	// Launch goroutines that safely increment counter
	for i := 0; i < numGoroutines; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()

			for j := 0; j < incrementsPerGoroutine; j++ {
				// CORRECT: Acquire mutex before accessing shared variable
				mu.Lock()
				counter++ // Safe: protected by mutex
				mu.Unlock()
			}

			// Safe to read under lock
			mu.Lock()
			currentValue := counter
			mu.Unlock()

			fmt.Printf("Goroutine %2d: completed %d increments (counter at %d)\n",
				id, incrementsPerGoroutine, currentValue)
		}(i)
	}

	// Wait for all goroutines to complete
	wg.Wait()

	elapsed := time.Since(startTime)

	fmt.Println()
	fmt.Println("=== Results ===")
	fmt.Printf("Final counter value: %d\n", counter)
	fmt.Printf("Expected value:      %d\n", numGoroutines*incrementsPerGoroutine)
	fmt.Printf("Time elapsed:        %v\n", elapsed)

	// Verify correctness
	expectedValue := numGoroutines * incrementsPerGoroutine
	if counter == expectedValue {
		fmt.Println("✓ SUCCESS: Counter value is correct (no data race)")
	} else {
		fmt.Printf("✗ FAILURE: Counter value is incorrect (expected %d, got %d)\n",
			expectedValue, counter)
		fmt.Println("  This should NEVER happen with proper mutex protection!")
	}

	fmt.Println()
	fmt.Println("=== Race Detection Analysis ===")
	fmt.Println("This program uses sync.Mutex to protect all accesses to 'counter'.")
	fmt.Println("Result: NO DATA RACES (all accesses are synchronized)")
	fmt.Println()
	fmt.Println("Key synchronization points:")
	fmt.Println("  1. mu.Lock() before counter++")
	fmt.Println("  2. mu.Unlock() after counter++")
	fmt.Println("  3. All reads also protected by mutex")
	fmt.Println()
	fmt.Println("The race detector should NOT report any issues.")
	fmt.Println("=== Demo Complete ===")
}
