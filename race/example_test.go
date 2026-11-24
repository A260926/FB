package race_test

import (
	"fmt"
	"sync"
	"unsafe"

	"github.com/kolkov/racedetector/race"
)

// Example demonstrates basic usage of the race detector API.
// Normally, instrumentation is automatic via the racedetector tool.
func Example() {
	race.Init()
	defer race.Fini()

	var counter int

	// Manual instrumentation (automatic when using racedetector tool)
	race.RaceWrite(uintptr(unsafe.Pointer(&counter)))
	counter = 42

	race.RaceRead(uintptr(unsafe.Pointer(&counter)))
	fmt.Println(counter)

	// Output:
	// 42
}

// Example_mutexProtected demonstrates race-free code with mutex protection.
func Example_mutexProtected() {
	race.Init()
	defer race.Fini()

	var (
		counter int
		mu      sync.Mutex
	)

	// Acquire synchronization
	race.RaceAcquire(uintptr(unsafe.Pointer(&mu)))
	mu.Lock()

	race.RaceWrite(uintptr(unsafe.Pointer(&counter)))
	counter = 42

	// Release synchronization
	race.RaceRelease(uintptr(unsafe.Pointer(&mu)))
	mu.Unlock()

	// No race detected - mutex protects access
	fmt.Println("No race detected")

	// Output:
	// No race detected
}

// Example_automaticInstrumentation shows how the racedetector tool works.
func Example_automaticInstrumentation() {
	// When using: racedetector build myprogram.go
	//
	// Original code:
	//   var x int
	//   x = 42
	//
	// Becomes:
	//   var x int
	//   race.RaceWrite(uintptr(unsafe.Pointer(&x)))
	//   x = 42
	//
	// The racedetector tool automatically:
	// 1. Imports github.com/kolkov/racedetector/race
	// 2. Calls race.Init() at program start
	// 3. Inserts race.RaceRead/RaceWrite calls
	// 4. Tracks synchronization primitives

	fmt.Println("Use: racedetector build myprogram.go")

	// Output:
	// Use: racedetector build myprogram.go
}
