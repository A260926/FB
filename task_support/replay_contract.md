# Isolated native replay batches

Add src/include/deepstate/Replay.h (C/C++ compatible) and src/lib/Replay_UNIX.c. Expose:

```c
enum DeepState_ReplayState { DeepState_ReplayComplete=0, DeepState_ReplayCanceled=1 };
struct DeepState_ReplayCase {
 uint64_t id;
 struct DeepState_TestInfo *test;
 const uint8_t *data;
 size_t size;
};
struct DeepState_ReplayConfig {
 uint64_t run_id;
 unsigned jobs;
 size_t log_limit;
 int cancel_fd;
 const char *output_dir;
};
struct DeepState_ReplayResult {
 uint64_t run_id, id;
 enum DeepState_ReplayState state;
 enum DeepState_TestRunResult result;
 size_t consumed, log_bytes;
 bool log_truncated;
};
int DeepState_ReplayBatch(const struct DeepState_ReplayCase *cases, size_t count,
 const struct DeepState_ReplayConfig *config, struct DeepState_ReplayResult *results);
```

This is a native Unix (Linux/macOS) Fuzzing regression replay entry, additive to the existing saved-file/fuzzer entrypoints. It is called from a single-threaded, quiescent process; ordinary stdio must have no pending buffered output. The caller keeps inputs/test metadata valid and unchanged until return. DeepState framework stream buffers may have pending content: the new children must not inherit it as their own log. It is not an OS security sandbox and need not isolate arbitrary application files or hostile callback behavior. All examples/tests are cooperative, finite, benign API use; do not run existing failing/crashing samples or a random campaign.

Validate the whole batch before forking or changing results/output: count=1..64, jobs=1..32, nonzero run_id and unique nonzero case IDs, nonnull case/test/test_func and results/config, size<=DeepState_InputSize, data nonnull for nonempty size, log_limit<=1MiB, cancel_fd=-1 or a valid readable pipe, nonempty output_dir whose parent exists. The final output directory must not already exist (including a symlink); never overwrite another run. Empty input is legal for a test consuming zero bytes. Reject invalid batches with 2; results bytes and existing paths remain unchanged. No caller DeepState globals/options/current-run pointer/stream buffers or input arrays may be modified in the parent. Existing APIs preserve their behavior.

Launch cases in input order with at most jobs real children. Each child owns its current-run state and fresh input cursor; initialize its input from exactly that case and disable random generation. Refactor src/lib/DeepState.c to provide a private child-execution path using DeepState_Begin and the existing DeepState_RunTestNoFork/setjmp semantics (not a direct call that bypasses Pass/Abandon handling). Reset inherited DeepState streams in the child without flushing inherited content. Suppress the framework's automatic output_test_dir writes in this path; preserve the actual returned test result and consumed cursor. Never send pointers such as reason/test metadata over IPC or read another child's shared singleton as the result. Prefix capture must use actual DeepState_InputIndex, not supplied size or test exit code. Valid callbacks consume no more than their supplied input. Abandon via a normal unsatisfied assumption is a handled outcome; no failure/crash fixture is required.

cancel_fd carries native-endian pairs of uint64_t (run_id,case_id), exactly 16 bytes per command, written by a local controller. Reads may split anywhere. Ignore complete commands from another run or an unknown/already-terminal case. EOF at a record boundary disables control; EOF with a partial record is an infrastructure error. Do not close the caller's fd or change its open-file status flags. In each poll iteration drain currently readable complete control records before harvesting waitpid results; a matching cancel parsed before the child was reaped wins even if that child has just exited. Once reaped, its result is final. A canceled queued case must never start. A canceled running case stops its own process group, closes/reaps/drains its resources, and cannot be mistaken for a normal pass. Do not signal unrelated processes. Handle EINTR/short reads/writes; no sleeps to make a race likely.

Redirect each child's stdout and stderr to one private pipe, continuously drained by the parent while other children/control are active. Keep at most log_limit bytes per case, but count every drained byte and continue discarding excess so a large normal log never blocks the child. Report log_truncated iff log_bytes>log_limit. Per-case byte order is the pipe's order; completion order must not change case identity. Close unrelated inherited runner pipe ends in children. Reap every direct child, release every mapping and owned fd before return; canceled descendants retaining a runner log pipe must not keep the call stuck. The caller's control fd remains open. A zero-byte cap still drains/log-counts. Polling for readiness or finite cleanup deadlines is allowed; tests synchronize with pipes/acks, not elapsed timing.

Generate output only in a private staging directory beside output_dir. Once all cases are terminal and all children/resources are settled, publish that directory as output_dir with one rename. Never expose a partial final directory. On infrastructure/validation/IO error return 2, preserve caller results and do not publish final output; remove only staging data created by this invocation. On a complete batch, return 1 if an original test result is Fail/Crash, otherwise 0, including benign abandonment and cancellation. These return codes do not require constructing a failing/crashing input.

Publish manifest.tsv with ASCII header `deepstate-replay-v1\t<RUN_ID>\n`, then input-order rows `<ID>\t<complete|canceled>\t<RESULT_NUMBER>\t<CONSUMED>\t<LOG_BYTES>\t<0|1>\n`. Escapes here denote actual tab/newline. Every case has `<ID>.log`, the retained merged bytes (possibly empty). Only a completed Pass has `<ID>.pass`, containing exactly its consumed input prefix, including empty when consumed=0. Canceled cases use result=DeepState_TestRunAbandon and consumed=0 and never get .pass; their bounded captured logs stay associated with their own ID. A normal Abandon preserves actual consumption and has no .pass. No PID, pointer, timestamp, random filename or current-global snapshot appears in the manifest. Caller results match the published manifest. This is a per-batch publication, not a persistent database/recovery service.

Required checks/replay_tests.c registers isolation, cancel, artifacts groups with task_support/check.h. Exercise simultaneous cases consuming different prefix lengths, Pass/normal Abandon, inherited partial DeepState streams, zero/overflow log caps, queued/running cancellation including split/stale commands, completion/cancel boundary, descriptor/child cleanup, and a second run after cancellation with the same IDs but another run_id. Verify saved bytes and manifest, not only counters. Keep libc printf interposition in production; the supplied driver reporting bridge only preserves generic test output. Use ordinary byte writes or dprintf for explicit sentinels/artifacts. Add examples/replay_batch.c: accept a new output path, run at least two actual benign DeepState tests via the batch API and write `<output_dir>/transcript.txt` after success; include `replay-batch normal=2` in that file. Do not add Python or third-party dependencies. The supplied target builds all new headers/sources, runs a fixed normal API consumer, the example and registered checks, then the existing saved-file baseline.
