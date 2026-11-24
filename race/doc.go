// Package race provides a Pure-Go race detector runtime API without CGO dependency.
//
// This package enables data race detection in Go programs compiled with CGO_ENABLED=0,
// serving as a drop-in replacement for Go's official race detector. The race detector
// implements the FastTrack algorithm (PLDI 2009) for efficient race detection.
//
// # Quick Start
//
// The race package is automatically injected by the racedetector tool:
//
//	$ racedetector build myprogram.go
//	$ ./myprogram
//
// For manual instrumentation in advanced scenarios:
//
//	package main
//
//	import (
//		"github.com/kolkov/racedetector/race"
//		"unsafe"
//	)
//
//	var counter int
//
//	func main() {
//		race.Init()
//		defer race.Fini()
//
//		// Manual instrumentation (normally done by racedetector tool)
//		race.RaceWrite(uintptr(unsafe.Pointer(&counter)))
//		counter = 42
//	}
//
// # API Overview
//
// The package provides functions for:
//   - Initialization and finalization: [Init], [Fini]
//   - Memory access tracking: [RaceRead], [RaceWrite]
//   - Synchronization primitives: [RaceAcquire], [RaceRelease]
//   - Version information: [GetInfo], [Version]
//
// # How It Works
//
// The racedetector tool instruments your code by inserting race detection calls
// before every memory access and synchronization operation:
//
//	// Original code:
//	x = 42
//
//	// Instrumented code:
//	race.RaceWrite(uintptr(unsafe.Pointer(&x)))
//	x = 42
//
// The race detector uses vector clocks to track happens-before relationships
// and detect unsynchronized concurrent accesses to shared memory. When a race
// is detected, a detailed report is printed showing:
//   - The conflicting memory accesses (read/write or write/write)
//   - Goroutine IDs involved in the race
//   - Stack traces showing where the accesses occurred
//   - File:line locations for debugging
//
// # Performance Characteristics
//
// The Pure-Go race detector is designed for production use with minimal overhead:
//
//	Runtime overhead:  5-15x slowdown (typical for race detection, v0.2.0 target: <10x)
//	Memory overhead:   Adaptive epoch/vector clock representation
//	Scalability:       Tested with 1000+ concurrent goroutines
//	False positives:   Minimal (skips constants, literals, built-ins)
//
// # Compatibility
//
// Platform support:
//   - Operating systems: Linux, macOS, Windows
//   - Go version: 1.19 or later
//   - CGO requirement: None (works with CGO_ENABLED=0)
//   - Architecture: amd64, arm64
//
// # Examples
//
// See package-level examples in the documentation:
//   - [Example] - Basic race detection usage
//   - [Example_mutexProtected] - Race-free code with mutex
//   - [Example_automaticInstrumentation] - How the tool works
//
// # Links
//
// Project repository:
// https://github.com/kolkov/racedetector
//
// Documentation:
// https://pkg.go.dev/github.com/kolkov/racedetector/race
//
// FastTrack algorithm paper (PLDI 2009):
// https://users.soe.ucsc.edu/~cormac/papers/pldi09.pdf
//
// Installation guide:
// https://github.com/kolkov/racedetector/blob/main/docs/INSTALLATION.md
//
// Usage guide:
// https://github.com/kolkov/racedetector/blob/main/docs/USAGE_GUIDE.md
package race
