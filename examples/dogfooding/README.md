# Dogfooding Demo - racedetector Testing Itself

> **Dogfooding** (eating your own dog food) - using your own product to validate it works.

This demo shows the `racedetector` tool testing itself by instrumenting and running example code with race detection enabled.

## 🎯 What This Demo Proves

**Phase 6A MVP Achievements:**
- ✅ Tool builds successfully
- ✅ Code instrumentation works (AST parsing + import injection)
- ✅ Instrumented code compiles
- ✅ `race.Init()` is automatically called
- ✅ Programs run with race detection initialized
- ✅ Both `build` and `run` commands work end-to-end

## 🚀 Running the Demo

### Linux/Mac (Bash)
```bash
cd examples/dogfooding/
bash demo.sh
```

### Windows (Command Prompt)
```cmd
cd examples\dogfooding
demo.bat
```

## 📋 Demo Workflow

The demo script performs the following steps:

1. **Build racedetector tool**
   ```bash
   go build -o racedetector ./cmd/racedetector
   ```

2. **Show tool version**
   ```bash
   ./racedetector --version
   ```

3. **Build example with race detection**
   ```bash
   ./racedetector build -o simple_race simple_race.go
   ```

4. **Run instrumented program**
   ```bash
   ./simple_race
   ```

5. **Test 'run' command**
   ```bash
   ./racedetector run simple_race.go
   ```

6. **Cleanup temporary files**

## 📊 Expected Output

```
========================================
  racedetector Dogfooding Demo
========================================

Step 1: Building racedetector tool...
--------------------------------------
✓ racedetector built successfully

Step 2: Checking tool version...
--------------------------------------
racedetector version 0.1.0-alpha

Step 3: Building example with race detection...
--------------------------------------
Built successfully: simple_race
✓ Example built successfully with race detection

Step 4: Running instrumented program...
--------------------------------------
=== Dogfooding Demo: Simple Race Detection ===

Goroutine 0: counter = 1
Goroutine 1: counter = 2
... (output may vary due to race condition)

Final counter value: 10 (expected 10, but race may cause different value)

=== Demo Complete ===

✓ Program executed successfully

Step 5: Testing 'racedetector run' command...
--------------------------------------
... (same output as step 4)

✓ 'run' command executed successfully

========================================
  Dogfooding Demo Summary
========================================

✓ Tool builds successfully
✓ Code instrumentation works
✓ Instrumented code compiles
✓ race.Init() is called
✓ Programs run with race detection

Note: Current MVP Status (Phase 6A)
  - AST parsing: ✓ Working
  - Import injection: ✓ Working
  - Init() injection: ✓ Working
  - Runtime linking: ✓ Working
  - Build/Run commands: ✓ Working

  Phase 2 (Future):
  - Full AST modification (insert RaceRead/Write calls)
  - Actual race detection in running programs

========================================
  Dogfooding Demo Complete!
========================================
```

## 📝 About simple_race.go

The demo program (`simple_race.go`) contains an **intentional data race**:

```go
var counter int  // Shared variable

// 10 goroutines increment counter without synchronization
for i := 0; i < 10; i++ {
    go func(id int) {
        val := counter    // READ (RACE!)
        counter = val + 1 // WRITE (RACE!)
    }(i)
}
```

### Why This is a Race Condition

Multiple goroutines read and write `counter` without synchronization:
- **Data Race:** Two goroutines access the same variable, at least one is a write, with no synchronization
- **Result:** Non-deterministic final value of `counter` (may not be 10!)

### Phase 6A MVP Status

**Current Implementation (Task A.1 - A.5):**
- ✅ AST parsing and import injection
- ✅ `race.Init()` automatically called
- ✅ Build and run commands working

**Phase 2 (Future):**
- ⏳ Full AST modification to insert `race.RaceRead()` and `race.RaceWrite()` calls
- ⏳ Actual race detection during program execution
- ⏳ Race report generation with stack traces

When Phase 2 is complete, this program will output:
```
==================
WARNING: DATA RACE
Write at 0x00c0000180a0 by goroutine 7:
  main.main.func1()
      simple_race.go:34 +0x3b

Previous read at 0x00c0000180a0 by goroutine 6:
  main.main.func1()
      simple_race.go:29 +0x2a
==================
```

## 🎓 What We Learned

This dogfooding demo validates:

1. **Tool Stability:** racedetector builds and runs without errors
2. **Code Generation:** Instrumented code is valid Go and compiles successfully
3. **Runtime Integration:** Race detector runtime initializes correctly
4. **Command Functionality:** Both `build` and `run` commands work as expected
5. **User Experience:** Tool acts as drop-in replacement for `go build`/`go run`

## 🚀 Next Steps

### Phase 6A Remaining Tasks:
- [ ] Task A.6: Implement 'racedetector test' command (optional)
- [x] Task A.7: Dogfooding demo (**YOU ARE HERE!**)
- [ ] Task A.8: Documentation and examples

### Path B: Runtime Integration
After Phase 6A completion, integrate into official Go runtime for `CGO_ENABLED=0` support.

## 🤝 Contributing

This is a proof-of-concept for a Pure-Go race detector. The dogfooding demo shows the foundation is solid and ready for Phase 2 implementation.

**Feedback welcome!** Open issues on GitHub if you find bugs or have suggestions.

---

*Phase 6A - Task A.7: Dogfooding Demo*
*Status: ✅ Working - Tool successfully tests itself!*
