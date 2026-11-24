// Package main demonstrates runtime.Callers() for stack trace capture.
//
// This is research code to understand how to capture and format
// stack traces for race reports.
package main

import (
	"fmt"
	"runtime"
)

func main() {
	fmt.Println("=== Stack Trace Capture Research ===")

	// Example call chain: main -> level1 -> level2 -> captureStack
	level1()
}

func level1() {
	fmt.Println("In level1()")
	level2()
}

func level2() {
	fmt.Println("In level2()")
	captureStack()
}

func captureStack() {
	fmt.Println("\n=== Capturing Stack Trace ===")

	// Allocate buffer for program counters (PCs)
	const maxDepth = 32
	pcs := make([]uintptr, maxDepth)

	// Capture stack trace
	// skip=0: include runtime.Callers itself
	// skip=1: start from captureStack
	// skip=2: start from caller of captureStack
	n := runtime.Callers(2, pcs)

	fmt.Printf("Captured %d frames\n\n", n)

	// Get frames from PCs
	frames := runtime.CallersFrames(pcs[:n])

	// Iterate through frames
	frameNum := 0
	for {
		frame, more := frames.Next()

		fmt.Printf("Frame %d:\n", frameNum)
		fmt.Printf("  Function: %s\n", frame.Function)
		fmt.Printf("  File:     %s\n", frame.File)
		fmt.Printf("  Line:     %d\n", frame.Line)
		fmt.Printf("  PC:       0x%x\n\n", frame.PC)

		if !more {
			break
		}
		frameNum++
	}

	// Alternative: FuncForPC approach
	fmt.Println("=== Alternative: FuncForPC ===")
	for i := 0; i < n; i++ {
		pc := pcs[i]
		fn := runtime.FuncForPC(pc)
		if fn != nil {
			file, line := fn.FileLine(pc)
			fmt.Printf("%d. %s\n", i, fn.Name())
			fmt.Printf("   %s:%d\n", file, line)
		}
	}
}
