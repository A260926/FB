// Package main demonstrates a simple data race for dogfooding test.
//
// This program contains an intentional data race that should be
// detected when running with the racedetector tool.
package main

import (
	"fmt"
	"sync"
	"time"
)

func main() {
	fmt.Println("=== Dogfooding Demo: Simple Race Detection ===")
	fmt.Println()

	// Shared variable (intentional race)
	var counter int
	var wg sync.WaitGroup

	// Launch 10 goroutines that increment counter
	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()

			// Read counter (RACE!)
			val := counter

			// Small delay to increase race probability
			time.Sleep(time.Microsecond)

			// Write counter (RACE!)
			counter = val + 1

			fmt.Printf("Goroutine %d: counter = %d\n", id, counter)
		}(i)
	}

	// Wait for all goroutines
	wg.Wait()

	fmt.Println()
	fmt.Printf("Final counter value: %d (expected 10, but race may cause different value)\n", counter)
	fmt.Println()
	fmt.Println("=== Demo Complete ===")
}
