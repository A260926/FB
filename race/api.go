// Package race provides the public API for the Pure-Go race detector.
//
// See doc.go for detailed documentation and examples.
package race

import internal "github.com/kolkov/racedetector/internal/race/api"

// Init initializes the race detector runtime.
//
// This function must be called before any other race detector operations.
// The racedetector tool automatically inserts this call at the beginning
// of the main() function.
//
// For manual instrumentation, call Init() at program startup:
//
//	func main() {
//		race.Init()
//		defer race.Fini()
//		// ... rest of program
//	}
//
// Init is safe to call multiple times (subsequent calls are no-ops).
func Init() {
	internal.Init()
}

// Fini finalizes the race detector and prints a summary report.
//
// This function should be called at program exit to ensure all race
// reports are printed and resources are cleaned up. The racedetector
// tool automatically handles this.
//
// For manual instrumentation, use defer:
//
//	func main() {
//		race.Init()
//		defer race.Fini()  // Ensures cleanup on exit
//		// ... rest of program
//	}
//
// The summary includes:
//   - Total number of races detected
//   - Goroutine statistics
//   - Memory usage statistics
func Fini() {
	internal.Fini()
}

// RaceRead records a memory read operation at the given address.
//
// This function is automatically inserted by the racedetector tool before
// each memory read operation. Manual calls are typically not needed.
//
// Parameters:
//   - addr: The memory address being read (use unsafe.Pointer conversion)
//
// Example (automatic instrumentation):
//
//	// Original code:
//	y := x
//
//	// Instrumented code:
//	race.RaceRead(uintptr(unsafe.Pointer(&x)))
//	y := x
//
// The race detector checks if this read conflicts with any concurrent
// writes to the same address that are not properly synchronized.
//
//nolint:revive // RaceRead naming matches Go's official race detector API
func RaceRead(addr uintptr) {
	internal.RaceRead(addr)
}

// RaceWrite records a memory write operation at the given address.
//
// This function is automatically inserted by the racedetector tool before
// each memory write operation. Manual calls are typically not needed.
//
// Parameters:
//   - addr: The memory address being written (use unsafe.Pointer conversion)
//
// Example (automatic instrumentation):
//
//	// Original code:
//	x = 42
//
//	// Instrumented code:
//	race.RaceWrite(uintptr(unsafe.Pointer(&x)))
//	x = 42
//
// The race detector checks if this write conflicts with any concurrent
// reads or writes to the same address that are not properly synchronized.
//
//nolint:revive // RaceWrite naming matches Go's official race detector API
func RaceWrite(addr uintptr) {
	internal.RaceWrite(addr)
}

// RaceAcquire records the acquisition of a synchronization object.
//
// This function establishes a happens-before relationship, indicating that
// all memory operations before a corresponding RaceRelease call are visible
// to operations after this RaceAcquire call.
//
// Typically used for:
//   - sync.Mutex.Lock()
//   - sync.RWMutex.Lock() / RLock()
//   - Receiving from a channel
//   - sync.WaitGroup.Wait()
//
// Parameters:
//   - addr: The address of the synchronization object (e.g., &mutex)
//
// Example (automatic instrumentation):
//
//	// Original code:
//	mu.Lock()
//
//	// Instrumented code:
//	race.RaceAcquire(uintptr(unsafe.Pointer(&mu)))
//	mu.Lock()
//
// This ensures that the race detector understands the synchronization
// and does not report false positives for properly protected code.
//
//nolint:revive // RaceAcquire naming matches Go's official race detector API
func RaceAcquire(addr uintptr) {
	internal.RaceAcquire(addr)
}

// RaceRelease records the release of a synchronization object.
//
// This function establishes a happens-before relationship, indicating that
// all memory operations before this RaceRelease call are visible to
// operations after a corresponding RaceAcquire call.
//
// Typically used for:
//   - sync.Mutex.Unlock()
//   - sync.RWMutex.Unlock() / RUnlock()
//   - Sending to a channel
//   - sync.WaitGroup.Done()
//
// Parameters:
//   - addr: The address of the synchronization object (e.g., &mutex)
//
// Example (automatic instrumentation):
//
//	// Original code:
//	mu.Unlock()
//
//	// Instrumented code:
//	race.RaceRelease(uintptr(unsafe.Pointer(&mu)))
//	mu.Unlock()
//
// This ensures that the race detector understands the synchronization
// and does not report false positives for properly protected code.
//
//nolint:revive // RaceRelease naming matches Go's official race detector API
func RaceRelease(addr uintptr) {
	internal.RaceRelease(addr)
}

// TODO: Additional API functions will be added when implemented in internal API:
// - RaceChannelSend(addr uintptr)
// - RaceChannelRecv(addr uintptr)
// - RaceChannelClose(addr uintptr)
// - RaceWaitGroupAdd(addr uintptr, delta int32)
// - RaceWaitGroupDone(addr uintptr)
// - RaceWaitGroupWait(addr uintptr)
//
// These are already implemented in internal/race/api but need to be exposed
// for standalone tool compatibility when needed.
