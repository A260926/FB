// Package main demonstrates end-to-end usage of the Pure-Go Race Detector.
//
// This example shows both race detection (concurrent access) and race-free
// code (properly synchronized access), illustrating how to use the detector
// API in a real program.
//
// Usage:
//
//	go run examples/integration/main.go
//
// Expected Output:
//   - Race reports for the racy counter
//   - No races for the safe counter
//   - Summary report from Fini()
package main

import (
	"fmt"
	"sync"
	"time"
	"unsafe"

	"github.com/kolkov/racedetector/internal/race/api"
)

func main() {
	fmt.Println("Pure-Go Race Detector - Integration Example")
	fmt.Println("============================================")
	fmt.Println()

	// Initialize the race detector
	// This MUST be called at the start of your program
	api.Init()

	// Ensure Fini() is called at program exit to print summary
	defer api.Fini()

	fmt.Println("Running three scenarios:")
	fmt.Println("1. Racy counter (concurrent writes)")
	fmt.Println("2. Safe counter (mutex-protected)")
	fmt.Println("3. Sequential operations (no concurrency)")
	fmt.Println()

	// Scenario 1: Racy Counter (WILL DETECT RACES)
	fmt.Println("[Scenario 1] Racy Counter - Concurrent Writes")
	racyCounterExample()

	// Give some time for output to flush
	time.Sleep(100 * time.Millisecond)

	// Scenario 2: Safe Counter (NO RACES EXPECTED)
	// Note: MVP doesn't track mutex happens-before, so this may still report races
	fmt.Println("[Scenario 2] Safe Counter - Mutex Protected")
	safeCounterExample()

	time.Sleep(100 * time.Millisecond)

	// Scenario 3: Sequential Operations (NO RACES)
	fmt.Println("[Scenario 3] Sequential Operations")
	sequentialExample()

	fmt.Println()
	fmt.Println("All scenarios completed. Race detector summary:")
	// Fini() will be called by defer and print the summary
}

// racyCounterExample demonstrates concurrent writes creating a race.
//
// Two goroutines increment a shared counter without synchronization,
// causing write-write races that the detector will catch.
func racyCounterExample() {
	var counter int
	var wg sync.WaitGroup

	// Goroutine 1: Increment counter 5 times
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 5; i++ {
			// Instrument the write access
			api.RaceWrite(uintptr(unsafe.Pointer(&counter)))

			// Actual write (this is where the race happens)
			counter++

			time.Sleep(10 * time.Millisecond)
		}
	}()

	// Goroutine 2: Increment counter 5 times (CONCURRENT - CREATES RACE)
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 5; i++ {
			// Instrument the write access
			api.RaceWrite(uintptr(unsafe.Pointer(&counter)))

			// Actual write (race with goroutine 1)
			counter++

			time.Sleep(10 * time.Millisecond)
		}
	}()

	// Wait for both goroutines to complete
	wg.Wait()

	fmt.Printf("  Final counter value: %d (expected: 10)\n", counter)
	fmt.Printf("  Note: Value may be incorrect due to race condition\n")
	fmt.Println()
}

// safeCounterExample demonstrates mutex-protected access (NO RACE).
//
// Two goroutines increment a shared counter, but protected by a mutex.
// The detector should NOT report races for this code.
//
// MVP Note: The current implementation doesn't track mutex happens-before
// relationships, so this may still report races. This is expected behavior
// for Phase 1 MVP and will be fixed in Phase 3 (Happens-Before Tracking).
func safeCounterExample() {
	var counter int
	var mu sync.Mutex
	var wg sync.WaitGroup

	// Goroutine 1: Increment counter with mutex protection
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 5; i++ {
			mu.Lock()

			// Instrument the write access
			api.RaceWrite(uintptr(unsafe.Pointer(&counter)))

			// Actual write (protected by mutex)
			counter++

			mu.Unlock()

			time.Sleep(10 * time.Millisecond)
		}
	}()

	// Goroutine 2: Increment counter with mutex protection (SAFE)
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 5; i++ {
			mu.Lock()

			// Instrument the write access
			api.RaceWrite(uintptr(unsafe.Pointer(&counter)))

			// Actual write (protected by same mutex)
			counter++

			mu.Unlock()

			time.Sleep(10 * time.Millisecond)
		}
	}()

	wg.Wait()

	fmt.Printf("  Final counter value: %d (expected: 10)\n", counter)
	fmt.Printf("  Note: MVP doesn't track mutex synchronization yet\n")
	fmt.Println()
}

// sequentialExample demonstrates sequential access (NO RACE).
//
// A single goroutine performs reads and writes sequentially.
// The detector should NOT report any races.
func sequentialExample() {
	var data int

	// Sequential writes (no concurrency)
	for i := 0; i < 5; i++ {
		// Instrument the write
		api.RaceWrite(uintptr(unsafe.Pointer(&data)))
		data = i

		// Instrument the read
		api.RaceRead(uintptr(unsafe.Pointer(&data)))
		_ = data
	}

	fmt.Printf("  Final data value: %d (expected: 4)\n", data)
	fmt.Printf("  No races expected (sequential access)\n")
	fmt.Println()
}

// Note: In a real application, you would typically NOT call RaceRead/RaceWrite manually.
// Instead, you would compile your code with -race flag, and the Go compiler would
// automatically instrument all memory accesses.
//
// This example calls the API directly for demonstration purposes only.
