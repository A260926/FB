# Channel-Based Synchronization Example

This example demonstrates **CORRECT** concurrent programming using Go channels for communication and synchronization.

## What This Demonstrates

**Synchronization Pattern:** Channel-based communication
- Channels provide built-in synchronization
- No explicit locks needed (channels handle it internally)
- **Result:** NO data races (channels are thread-safe)

## Three Patterns Demonstrated

### 1. Producer-Consumer
- One goroutine produces values
- Another goroutine consumes values
- Channel synchronizes communication

### 2. Worker Pool
- Multiple workers process jobs concurrently
- Jobs distributed via channel
- Results collected via another channel

### 3. Fan-Out/Fan-In
- Fan-out: Distribute work to multiple workers
- Fan-in: Collect results from multiple workers
- Uses channels for both distribution and collection

## Code Highlights

```go
// Producer sends values
ch := make(chan int)
go func() {
    ch <- 42  // Safe: channel handles synchronization
}()

// Consumer receives values
val := <-ch   // Safe: channel synchronization
```

## Expected Behavior

**When run with race detector:**
- ✅ NO race conditions detected
- ✅ All values received correctly
- ✅ All goroutines complete successfully

**Phase 6A MVP (current):**
- Program runs successfully
- All patterns complete without errors
- Race detector runtime initializes

**Phase 2 (future):**
- Race detector will confirm: "No races detected"
- Channel synchronization tracking will validate happens-before relationships

## Running the Example

### With racedetector

```bash
# Run with race detection
racedetector run main.go

# Build and run separately
racedetector build -o channel_demo main.go
./channel_demo
```

### With standard Go race detector (for comparison)

```bash
go run -race main.go
```

**Expected output:**
```
=== Channel-Based Synchronization Demo ===

--- Demo 1: Producer-Consumer ---
Producer: Starting to send values...
Consumer: Starting to receive values...
Producer: Sent 1
Consumer: Received 1 (sum = 1)
Producer: Sent 2
Consumer: Received 2 (sum = 3)
...
Producer: Sent 10
Consumer: Received 10 (sum = 55)
Producer: Finished, channel closed
Consumer: Final sum = 55 (expected 55)
✓ SUCCESS: Correct result (no race)

--- Demo 2: Worker Pool ---
Worker 1: Started
Worker 2: Started
Worker 3: Started
Dispatcher: Sending jobs...
Worker 1: Processing job 1
...
Collector: Received 10 results (expected 10)
✓ SUCCESS: All results collected (no race)

--- Demo 3: Fan-Out/Fan-In ---
Fan-out worker 0: Started
Fan-out worker 1: Started
Fan-out worker 2: Started
Input: Sending values...
Fan-out worker 0: 1 -> 1
...
Output: Collected 5 results (expected 5)
✓ SUCCESS: All results received (no race)

=== Demo Complete ===
```

**Important:** Standard Go race detector should NOT report any warnings (all communication is via channels).

## Why Channels are Safe

### Built-in Synchronization

Channels provide:
1. **Mutual exclusion:** Channel operations are atomic
2. **Happens-before:** Send happens before receive
3. **Memory barriers:** Channel ensures visibility

### Happens-Before Guarantees

```
Goroutine A:        Goroutine B:
ch <- value     →   value := <-ch
(happens-before)
```

**Go Memory Model guarantees:**
- A send on a channel happens before the corresponding receive
- The closing of a channel happens before a receive that returns zero value
- A receive from unbuffered channel happens before the send completes

## Common Patterns

### Pattern 1: Unbuffered Channel (Rendezvous)

```go
ch := make(chan int)  // Unbuffered

// Sender blocks until receiver ready
go func() { ch <- 42 }()

// Receiver blocks until sender ready
val := <-ch
```

**Synchronization:** Both goroutines rendezvous (meet) at the channel operation.

### Pattern 2: Buffered Channel (Queue)

```go
ch := make(chan int, 10)  // Buffer of 10

// Sender can send up to 10 without blocking
for i := 0; i < 10; i++ {
    ch <- i
}

// Receiver can receive all buffered values
for i := 0; i < 10; i++ {
    val := <-ch
}
```

**Synchronization:** Buffer decouples sender and receiver (up to buffer size).

### Pattern 3: Select with Multiple Channels

```go
select {
case val := <-ch1:
    // Handle ch1
case val := <-ch2:
    // Handle ch2
case <-time.After(1 * time.Second):
    // Timeout
}
```

**Synchronization:** Waits on multiple channels simultaneously.

## Common Mistakes (Not in This Example)

### Mistake 1: Shared State Without Synchronization

```go
// ❌ WRONG: Shared variable without synchronization
var counter int
go func() { counter++ }()  // DATA RACE!
counter++                   // DATA RACE!

// ✅ CORRECT: Use channel to communicate
ch := make(chan int)
go func() { ch <- 1 }()
val := <-ch
```

### Mistake 2: Closing Channel Multiple Times

```go
// ❌ WRONG: Closing channel from multiple goroutines
go func() { close(ch) }()  // PANIC if both execute!
go func() { close(ch) }()

// ✅ CORRECT: Close from one goroutine after all senders done
wg.Wait()      // Wait for all senders
close(ch)      // Safe: all senders finished
```

### Mistake 3: Sending After Close

```go
// ❌ WRONG: Send after close
close(ch)
ch <- 42  // PANIC: send on closed channel

// ✅ CORRECT: Close after all sends
ch <- 42
close(ch)
```

## Performance

**Unbuffered channel:** ~50-100ns per operation
**Buffered channel:** ~30-50ns per operation (when buffer not full)
**Mutex Lock/Unlock:** ~20-30ns per operation

**Trade-off:** Channels are slightly slower but provide cleaner code and built-in synchronization.

## When to Use Channels vs Mutexes

**Use Channels When:**
- Communicating data between goroutines
- Coordinating goroutine lifecycles
- Implementing pipelines or workflows

**Use Mutexes When:**
- Protecting shared state (simple counters, caches)
- Short critical sections
- Performance is critical (mutexes are faster)

**Rule of Thumb:** "Share memory by communicating, don't communicate by sharing memory."

## Related Examples

- `examples/mutex_protected/` - Mutex-based synchronization (comparison)
- `examples/dogfooding/simple_race.go` - Example WITH a data race (what NOT to do)
- `examples/integration/` - Integration test suite

## Technical Details

### Channel Implementation

Channels are implemented using:
1. **Lock:** Internal mutex protects channel state
2. **Queues:** Waiting senders and receivers
3. **Buffer:** Ring buffer for buffered channels

**Race detector sees:**
- All channel operations (send, receive, close)
- Happens-before relationships created by channels
- No direct memory access (channel handles it)

### Race Detector Behavior

**What race detector tracks:**
1. Channel send/receive operations
2. Happens-before relationships via channels
3. Memory accesses (none directly in channel code)

**Why no race is detected:**
- All communication goes through channels
- Channels create happens-before relationships
- No shared memory accessed without synchronization

## Best Practices Demonstrated

1. **Close channels from sender:** Receiver can detect closure with `range` or `val, ok := <-ch`
2. **Use buffered channels for decoupling:** Prevents blocking when appropriate
3. **Use `select` for timeouts:** Avoid hanging on blocked channel operations
4. **Document channel ownership:** Who sends, who receives, who closes

## Further Reading

- [Go Concurrency Patterns](https://go.dev/talks/2012/concurrency.slide)
- [Go Memory Model - Channels](https://go.dev/ref/mem#tmp_7)
- [Effective Go: Channels](https://go.dev/doc/effective_go#channels)
- [Advanced Go Concurrency Patterns](https://go.dev/talks/2013/advconc.slide)

---

*This example demonstrates CORRECT synchronization with Go channels*
*No data races - channels are thread-safe by design*
