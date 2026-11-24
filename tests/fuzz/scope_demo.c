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
 * End-to-end demonstration: one fixed plan that creates an object, wraps it
 * with a take-ownership exception wrapper, extracts the payload, promotes it
 * across scopes, then closes everything and forces collection. Both replays
 * must agree on object identity and on the single finalization event.
 */
#include "tests/fuzz/scope_model.h"
#include "tests/fuzz/scope_replay.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size);

/* OPEN, NEW t0, WRAP t0->t1 (exception take), EXTRACT? no: ESCAPE t1 path */
static const uint8_t demo_plan[] =
{
  SCOPE_OP_OPEN, 0, 0, 0,          /* outer escapable scope            */
  SCOPE_OP_OPEN, 0, 0, 0,          /* inner escapable scope            */
  SCOPE_OP_NEW, 0, 0, 0,           /* t0 = new object in inner scope   */
  SCOPE_OP_WRAP, 0, 1, SCOPE_BRANCH_EXCEPTION_TAKE, /* t1 = wrapper   */
  SCOPE_OP_EXTRACT, 1, 2, SCOPE_BRANCH_EXCEPTION_COPY, /* t2 = payload */
  SCOPE_OP_ESCAPE, 2, 0, 0,        /* promote payload to outer scope   */
  SCOPE_OP_GC, 0, 0, 0,            /* inner closure not yet, GC audits */
  SCOPE_OP_CLOSE, 0, 0, 0,         /* close inner scope                */
  SCOPE_OP_GC, 0, 0, 0,            /* payload survives via outer scope */
  SCOPE_OP_CLOSE, 0, 0, 0,         /* close outer scope                */
  SCOPE_OP_GC, 0, 0, 0,            /* object finalized exactly once    */
};

int
main (void)
{
  struct jerry_scope_report report;
  int rc = jerry_scope_run (demo_plan, sizeof (demo_plan), &report);

  assert (rc == 0);
  assert (report.steps == sizeof (demo_plan) / 4u);
  assert (report.created == 1);
  assert (report.finalized == 1);
  assert (report.opened == 2);
  assert (report.escapes == 1);
  assert (report.takes == 1);
  assert (report.copies == 1);
  assert (report.comparisons > 0);

  /* The same interpretation chain backs the libFuzzer-style entry point. */
  assert (LLVMFuzzerTestOneInput (demo_plan, sizeof (demo_plan)) == 0);

  printf ("SCOPE_DEMO steps=%zu created=%zu finalized=%zu escapes=%zu takes=%zu copies=%zu\n",
          report.steps, report.created, report.finalized, report.escapes,
          report.takes, report.copies);
  puts ("SCOPE_DEMO_OK");
  return 0;
} /* main */
