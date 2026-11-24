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
 * libFuzzer entry point. It contains no interpretation logic of its own:
 * every byte stream goes through the same jerry_scope_run chain used by the
 * deterministic driver and the tests. Truncated, illegal or unbalanced plans
 * are refused (return code 1) without ever touching the engine, so only a
 * genuine model/engine divergence (return code 2) fails the run.
 */
#include "tests/fuzz/scope_replay.h"

#include <stdlib.h>

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  struct jerry_scope_report report;
  int rc = jerry_scope_run (data, size, &report);

  if (rc == 2)
  {
    /* Real divergence: jerry_scope_run has already printed the first
     * difference, including step, depth, ticket responsibility and identity. */
    abort ();
  }
  /* rc == 0: replayed cleanly; rc == 1: whole plan refused up front. */
  return 0;
} /* LLVMFuzzerTestOneInput */
