# Keyed Fuzzing execution

Add internal/gotestrunner/fuzzcontrol.go and fuzzoutput.go; implement these exported types in package gotestrunner:

```go
type FuzzAttempt struct { Key string; Generation uint64 }
type FuzzEvent struct { Attempt FuzzAttempt; Stream string; Text string; Sequence uint64 }
type FuzzAttemptOutcome struct {
 Attempt FuzzAttempt
 Target FuzzTarget
 Outcome FuzzTargetOutcome
 Superseded bool
}
type FuzzSessionResult struct { Attempts []FuzzAttemptOutcome }
func StartFuzzSession(context.Context, FuzzRunConfig, func(FuzzEvent)) *FuzzController
func (*FuzzController) Submit(string, FuzzTarget) (FuzzAttempt, error)
func (*FuzzController) Cancel(FuzzAttempt) bool
func (*FuzzController) Wait() FuzzSessionResult
```

Keys are nonempty; target Func and Dir must be nonempty. Invalid submissions change nothing. Each accepted key starts at generation 1, increasing per key, including after completion or cancellation. No wraparound: reject exhausted keys. Copy cfg.BuildFlags at construction and the target value at admission. The controller is safe for concurrent callers. Submit after sealing or parent cancellation fails. Wait seals admission (idempotently), waits for all accepted attempts and all their retiring processes/output readers, and returns a detached immutable-by-caller snapshot in global admission order. Concurrent/repeated Wait calls return equivalent independent snapshots. Parent cancellation cancels all admitted work; nothing starts afterwards.

Each key has at most one running process. Replacing a key supersedes its previous current attempt, whether queued or running; record Superseded=true for that previous attempt even if it had already finished. An unfinished old attempt is canceled; a new attempt for that key cannot start until the old process tree has stopped and both streams have drained. Additional replacements may coalesce pending attempts: every accepted ticket still has exactly one outcome, and a superseded attempt that never started is Skipped. Unrelated keys may continue. cfg.Jobs uses the existing resolveFuzzJobs semantics; it limits real processes through retirement, not merely current ticket count. Queue order across eligible distinct keys is admission FIFO; a blocked same-key successor does not prevent unrelated eligible work. Wait sealing must not stop already admitted healthy work.

Cancel only succeeds for the exact current unfinished ticket and only once. It revokes that ticket's output immediately, cancels its own work and returns true. Stale, completed or already-canceled tickets return false and cannot affect another generation. An accepted canceled-but-never-started ticket is Skipped and Canceled. A running canceled ticket is Canceled; its genuine raw outcome remains attached to its own ticket. Replacements mark Superseded but do not rewrite raw metrics into the successor. Rejected submissions are absent from results.

Use the existing go test -fuzz command construction, fixture environment, ManagedProcess, output line splitting/cap and FuzzTargetOutcome interpretation in the actual execution path. Refactor the shared runner as needed. Do not redirect global os.Stdout/os.Stderr, replace the process manager, or launch a second independent execution implementation. A nil sink suppresses live output for this new API. The old RunFuzzTargets still produces its original console output and outcomes.

Every delivered event belongs to an admitted ticket and has Stream=stdout or stderr, Text equal to the existing filtered line payload without the console label or final newline (preserve the existing continuation marker), and Sequence increasing from 1 across both streams of that attempt. Preserve per-stream order, the bounded partial-line behavior and progress/deadline parsing. Invoke the sink serially across the controller, outside its state lock. Once Submit replacement or successful Cancel returns, no callback for that previous ticket may start or remain in flight. Drain obsolete streams without emitting them or updating another attempt's metrics. Sink callbacks must return promptly and must not call controller methods; the controller need not support reentrant callbacks. Wait returns only after callbacks finish. Events emitted before invalidation remain valid; do not retract them.

Tests must use pipe/file/ack barriers for scheduling, not sleeps. A helper may emulate go's output for exact cancellation, drain and queue windows; it must be labeled as such. Also execute the bundled harmless FuzzEcho with the real go tool in the demo and at least one added test. Do not generate failing or crashing fuzz inputs. Existing batch APIs and fixtures retain behavior; the new API is an additive functional feature, not a claim about an upstream vulnerability.

The required target test file is internal/gotestrunner/fuzzcontrol_contract_test.go, package gotestrunner_test, with TestFuzzReplacement, TestFuzzCancellation and TestFuzzAttribution. Each uses task_support/taskcheck.Run and real content assertions. Cover queued coalescing, same-key old drain while a different key is runnable, real process maximum including retirees, stale cancellation, partial stdout/stderr, late progress, parent cancel, concurrent/repeated Wait snapshots, invalid/sealed submission and original RunFuzzTargets compatibility. cmd/fuzzsession_demo/main.go must run one real FuzzEcho through the controller (use -fuzztime=20x and -parallel=1) and print exactly one summary line `fuzz-session generations=1 exit=0` after success; the new API's sink can be nil. No extra dependency, Python or external service.
