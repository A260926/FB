/*
 * Copyright (c) 2019 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Isolated native replay batches.
 *
 * A batch runs a fixed set of benign, already-generated inputs against real
 * DeepState tests, one forked child per running case, and publishes a private
 * set of artifacts (manifest, per-case log and passing input prefix) as one
 * directory.  This is a Fuzzing regression replay entry point, layered on top
 * of the existing saved-file/fuzzer entrypoints; it is not an OS sandbox. */

#ifndef SRC_INCLUDE_DEEPSTATE_REPLAY_H_
#define SRC_INCLUDE_DEEPSTATE_REPLAY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <deepstate/DeepState.h>

DEEPSTATE_BEGIN_EXTERN_C

enum DeepState_ReplayState {
  /* The case's test ran to a terminal result. */
  DeepState_ReplayComplete = 0,
  /* The controller canceled the case; it is never reported as a pass. */
  DeepState_ReplayCanceled = 1
};

struct DeepState_ReplayCase {
  /* Nonzero, batch-unique case identifier chosen by the caller. */
  uint64_t id;
  struct DeepState_TestInfo *test;
  /* Exactly `size` bytes of ordinary (non-fault) input.  May be NULL when
   * `size` is zero. */
  const uint8_t *data;
  size_t size;
};

struct DeepState_ReplayConfig {
  /* Nonzero batch identifier; cancel commands from other batches are stale. */
  uint64_t run_id;
  /* Maximum number of simultaneous real children (1..32). */
  unsigned jobs;
  /* Maximum number of log bytes retained per case (<= 1 MiB). */
  size_t log_limit;
  /* -1 to disable cancellation, otherwise a readable pipe carrying pairs of
   * native-endian uint64_t (run_id, case_id). */
  int cancel_fd;
  /* Must not exist yet; published atomically when the whole batch is done. */
  const char *output_dir;
};

struct DeepState_ReplayResult {
  uint64_t run_id;
  uint64_t id;
  enum DeepState_ReplayState state;
  enum DeepState_TestRunResult result;
  size_t consumed;
  size_t log_bytes;
  bool log_truncated;
};

/* Run every case in `cases` (up to `jobs` at a time, in input order) and
 * publish `config->output_dir` if the whole batch completes.
 *
 * Returns 0 when every case finished and no original test result was
 * Fail/Crash, 1 when the batch finished but some original result was
 * Fail/Crash, and 2 for an invalid batch or an infrastructure/IO error.  On a
 * 2 return the caller's `results` are untouched and no output directory is
 * published.
 *
 * `cases`, `results`, `config`, and everything they point at (including the
 * per-case inputs and test metadata) must stay valid until this call returns.
 * The call must be made from a single-threaded, quiescent process; ordinary
 * stdio must not have pending buffered output. */
extern int DeepState_ReplayBatch(
    const struct DeepState_ReplayCase *cases, size_t count,
    const struct DeepState_ReplayConfig *config,
    struct DeepState_ReplayResult *results);

DEEPSTATE_END_EXTERN_C

#endif  /* SRC_INCLUDE_DEEPSTATE_REPLAY_H_ */
