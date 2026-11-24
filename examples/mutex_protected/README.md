# Mutex-Protected Counter Example

This example demonstrates **CORRECT** concurrent programming using `sync.Mutex` to protect shared variables.

## What This Demonstrates

**Synchronization Pattern:** Mutex-based locking
- Multiple goroutines increment a shared counter
- All accesses are protected by `sync.Mutex`
- **Result:** NO data races (correct synchronization)

## Code Highlights

```go
var counter int       // Shared variable
var mu sync.Mutex    // Protects counter

// In each goroutine:
mu.Lock()
counter++  // Safe: protected by mutex
mu.Unlock()
```

## Expected Behavior

**When run with race detector:**
- ✅ NO race conditions detected
- ✅ Final counter value is always correct (1000)
- ✅ Program completes successfully

**Phase 6A MVP (current):**
- Program runs successfully
- Counter value is correct (proof of proper synchronization)
- Race detector runtime initializes

**Phase 2 (future):**
- Race detector will confirm: "No races detected"
- Happens-before tracking will validate mutex synchronization

## Running the Example

### With racedetector

```bash
# Run with race detection
racedetector run main.go

# Build and run separately
racedetector build -o mutex_demo main.go
./mutex_demo
```

### With standard Go race detector (for comparison)

```bash
go run -race main.go
```

**Expected output (standard Go):**
```
=== Mutex-Protected Counter Demo ===

Starting 10 goroutines, each incrementing counter 100 times

Goroutine  0: completed 100 increments (counter at 234)
Goroutine  1: completed 100 increments (counter at 456)
...
Goroutine  9: completed 100 increments (counter at 1000)

=== Results ===
Final counter value: 1000
Expected value:      1000
Time elapsed:        2.5ms
✓ SUCCESS: Counter value is correct (no data race)

=== Race Detection Analysis ===
This program uses sync.Mutex to protect all accesses to 'counter'.
Result: NO DATA RACES (all accesses are synchronized)

Key synchronization points:
  1. mu.Lock() before counter++
  2. mu.Unlock() after counter++
  3. All reads also protected by mutex

The race detector should NOT report any issues.
=== Demo Complete ===
```

**Important:** Standard Go race detector should NOT report any warnings (all accesses are properly synchronized).

## Performance

**Single-threaded equivalent:** ~5µs
**Mutex-protected (10 goroutines):** ~2-3ms
**Overhead:** ~500x (due to lock contention, NOT race detector)

**Note:** This overhead is from mutex synchronization, not from race detection.

## Learning Points

### Why This is Correct

1. **Exclusive access:** Only one goroutine can hold the mutex at a time
2. **Atomic increment:** `counter++` is protected by lock
3. **Memory barriers:** Mutex provides happens-before relationship
4. **No data races:** All accesses are serialized by mutex

### Common Mistakes (Not in This Example)

```go
// ❌ WRONG: Reading without lock
value := counter  // DATA RACE!

// ✅ CORRECT: Reading with lock
mu.Lock()
value := counter
mu.Unlock()
```

```go
// ❌ WRONG: Forgetting to unlock
mu.Lock()
counter++
// Missing mu.Unlock() - DEADLOCK!

// ✅ CORRECT: Use defer
mu.Lock()
defer mu.Unlock()  // Guaranteed unlock
counter++
```

## Comparison with Unsafe Code

See `examples/dogfooding/simple_race.go` for a similar program **without** mutex protection, which DOES have a data race.

**Key difference:**
- **This example:** `mu.Lock()` before every access → NO RACE
- **simple_race.go:** No synchronization → DATA RACE

## Related Examples

- `examples/channel_sync/` - Channel-based synchronization
- `examples/dogfooding/simple_race.go` - Example WITH a data race (what NOT to do)
- `examples/integration/` - Integration test suite

## Technical Details

### Synchronization Mechanism

**Mutex Lock/Unlock provides:**
1. **Mutual exclusion** - Only one goroutine in critical section
2. **Memory ordering** - Happens-before relationship between unlock and next lock
3. **Visibility** - Changes visible to next lock holder

**Happens-Before Chain:**
```
goroutine 1: Lock → counter++ → Unlock
                                   ↓ (happens-before)
goroutine 2:                    Lock → counter++ → Unlock
                                                      ↓
goroutine 3:                                       Lock → ...
```

### Race Detector Behavior

**What race detector tracks:**
1. All memory accesses (reads and writes)
2. Synchronization events (Lock/Unlock)
3. Happens-before relationships

**Why no race is detected:**
- Every access to `counter` is inside a Lock/Unlock pair
- Mutex creates happens-before relationships
- Race detector validates: all accesses are properly ordered

## Best Practices Demonstrated

1. **Use defer for unlock:** Ensures unlock happens even on panic
2. **Keep critical sections small:** Only `counter++` is locked
3. **Document shared state:** Comments explain what mutex protects
4. **Verify correctness:** Check final value matches expected

## Further Reading

- [Go Memory Model](https://go.dev/ref/mem) - Happens-before guarantees
- [sync.Mutex Documentation](https://pkg.go.dev/sync#Mutex)
- [Effective Go: Concurrency](https://go.dev/doc/effective_go#concurrency)

---

*This example demonstrates CORRECT synchronization with sync.Mutex*
*No data races - safe for production use*
