/* Copyright JS Foundation and other contributors, http://js.foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Replay interface fixed by task_support/contract.md: a headless stream of
 * four-byte records is validated, then replayed twice against independent
 * jerry_init/cleanup cycles while the pure model audits responsibility and
 * finalization.
 */
#ifndef TESTS_FUZZ_SCOPE_REPLAY_H
#define TESTS_FUZZ_SCOPE_REPLAY_H

#include <stddef.h>
#include <stdint.h>

struct jerry_scope_report
{
  size_t steps;        /* number of records in the plan */
  size_t created;      /* logical objects created */
  size_t finalized;    /* logical objects seen finalized, by real identity */
  size_t comparisons;  /* responsibility checks executed */
  size_t opened;       /* scopes successfully opened */
  size_t escapes;      /* escapes successfully promoted */
  size_t takes;        /* WRAP/EXTRACT take-ownership branches */
  size_t copies;       /* WRAP/EXTRACT copy-ownership branches */
};

/*
 * Returns 0 on success, 1 when the whole plan is invalid (it is refused
 * before the engine is initialized) and 2 on a model/engine divergence.
 * On entry *out is zeroed; on return it holds the real counters of this
 * call. The function never terminates the process.
 */
int jerry_scope_run (const uint8_t *data, size_t size, struct jerry_scope_report *out);

/*
 * Deterministic fixed-seed driver sharing the exact jerry_scope_run
 * interpretation chain. At least 256 valid plans are replayed, each twice in
 * independent jerry_init/cleanup cycles. When seed_count_p is non-NULL it
 * receives the number of seeds executed.
 */
int jerry_scope_fuzz_seed_drive (size_t *seed_count_p);

#endif /* !TESTS_FUZZ_SCOPE_REPLAY_H */
