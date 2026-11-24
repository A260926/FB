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
 * Real assertions for the scope/exception model. Registered with the
 * task_support/check.h harness in four groups: scopes, exceptions, replay,
 * diagnostics.
 */
#include "../../task_support/check.h"

#include "tests/fuzz/scope_model.h"
#include "tests/fuzz/scope_replay.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Small plan builder.                                                */
/* ------------------------------------------------------------------ */
typedef struct
{
  uint8_t bytes[512];
  size_t length;
} plan_builder_t;

static void
plan_init (plan_builder_t *plan_p)
{
  plan_p->length = 0;
} /* plan_init */

static void
plan_emit (plan_builder_t *plan_p, uint8_t op, uint8_t a, uint8_t b, uint8_t flags)
{
  size_t offset = plan_p->length;
  plan_p->bytes[offset] = op;
  plan_p->bytes[offset + 1u] = a;
  plan_p->bytes[offset + 2u] = b;
  plan_p->bytes[offset + 3u] = flags;
  plan_p->length += 4u;
} /* plan_emit */

/* ------------------------------------------------------------------ */
/* scopes group: nesting, escape responsibility, storage thresholds.  */
/* ------------------------------------------------------------------ */
static int
test_scopes_nested (void)
{
  plan_builder_t plan;
  struct jerry_scope_report report;

  plan_init (&plan);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);

  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 0);
  TASK_CHECK (report.steps == 8);
  TASK_CHECK (report.opened == 2);
  TASK_CHECK (report.created == 1);
  TASK_CHECK (report.finalized == 1);
  TASK_CHECK (report.escapes == 0);
  TASK_CHECK (report.comparisons > 0);
  return 0;
} /* test_scopes_nested */

static int
test_scopes_escape_promotion (void)
{
  plan_builder_t plan;
  struct jerry_scope_report report;

  plan_init (&plan);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);   /* outer scope */
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);   /* inner scope */
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_ESCAPE, 0, 0, 0); /* inner -> outer */
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);     /* still alive */
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);  /* inner cannot kill it */
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);     /* outer still holds it */
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);     /* finalized exactly once */

  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 0);
  TASK_CHECK (report.escapes == 1);
  TASK_CHECK (report.created == 1);
  TASK_CHECK (report.finalized == 1);
  return 0;
} /* test_scopes_escape_promotion */

static int
test_scopes_escape_twice (void)
{
  plan_builder_t plan;
  struct jerry_scope_report report;
  scope_model_t model;

  plan_init (&plan);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_NEW, 1, 0, 0);
  plan_emit (&plan, SCOPE_OP_ESCAPE, 0, 0, 0); /* first succeeds */
  plan_emit (&plan, SCOPE_OP_ESCAPE, 1, 0, 0); /* twice: model unchanged */
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);

  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 0);
  TASK_CHECK (report.escapes == 1);
  TASK_CHECK (report.created == 2);
  TASK_CHECK (report.finalized == 2);

  /* Model-level prediction of the two escape outcomes. */
  TASK_CHECK (scope_model_precheck (&model, plan.bytes, plan.length) == 0);
  TASK_CHECK (scope_model_apply (&model, &model.steps[0]) == 0);
  TASK_CHECK (scope_model_apply (&model, &model.steps[1]) == 0);
  TASK_CHECK (scope_model_apply (&model, &model.steps[2]) == 0);
  TASK_CHECK (scope_model_apply (&model, &model.steps[3]) == 0);
  TASK_CHECK (scope_model_apply (&model, &model.steps[4]) == 0);
  TASK_CHECK (model.last_escape == SCOPE_ESCAPE_OK);
  TASK_CHECK (scope_model_apply (&model, &model.steps[5]) == 0);
  TASK_CHECK (model.last_escape == SCOPE_ESCAPE_TWICE);
  /* The rejected escape left the second ticket in its original scope. */
  TASK_CHECK (model.ticket_scope[1] == 2);
  return 0;
} /* test_scopes_escape_twice */

static int
test_scopes_dynamic_thresholds (void)
{
  plan_builder_t plan;
  struct jerry_scope_report report;
  int level;
  int ticket;

  plan_init (&plan);
  /* Cross the 20-entry scope pool: 23 nested open scopes. */
  for (level = 0; level < 23; level++)
  {
    plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  }
  /* Cross the 20-handle prelist of one scope: 25 registrations. */
  for (ticket = 0; ticket < 25; ticket++)
  {
    plan_emit (&plan, SCOPE_OP_NEW, (uint8_t) ticket, 0, 0);
  }
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);
  for (level = 0; level < 23; level++)
  {
    plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  }
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);

  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 0);
  TASK_CHECK (report.opened == 23);
  TASK_CHECK (report.created == 25);
  TASK_CHECK (report.finalized == 25);
  return 0;
} /* test_scopes_dynamic_thresholds */

static int
test_scopes_reopen_after_close (void)
{
  plan_builder_t plan;
  struct jerry_scope_report report;

  plan_init (&plan);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);
  /* Slot 0 is dead again, reopening must let it be reused. */
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);

  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 0);
  TASK_CHECK (report.opened == 2);
  TASK_CHECK (report.created == 2);
  TASK_CHECK (report.finalized == 2);
  return 0;
} /* test_scopes_reopen_after_close */

/* ------------------------------------------------------------------ */
/* exceptions group: take/copy branches and identity preservation.    */
/* ------------------------------------------------------------------ */
static int
test_exceptions_wrap_branches (void)
{
  plan_builder_t plan;
  struct jerry_scope_report report;
  uint8_t flags;

  plan_init (&plan);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);
  for (flags = 0; flags < 4; flags++)
  {
    plan_emit (&plan, SCOPE_OP_WRAP, 0, (uint8_t) (flags + 1u), flags);
  }
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);

  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 0);
  TASK_CHECK (report.takes == 2);
  TASK_CHECK (report.copies == 2);
  TASK_CHECK (report.created == 1);
  TASK_CHECK (report.finalized == 1);
  return 0;
} /* test_exceptions_wrap_branches */

static int
test_exceptions_extract_branches (void)
{
  plan_builder_t plan;
  struct jerry_scope_report report;

  plan_init (&plan);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_WRAP, 0, 1, SCOPE_BRANCH_EXCEPTION_TAKE);
  plan_emit (&plan, SCOPE_OP_EXTRACT, 1, 2, SCOPE_BRANCH_EXCEPTION_COPY);
  plan_emit (&plan, SCOPE_OP_EXTRACT, 1, 3, SCOPE_BRANCH_EXCEPTION_TAKE);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);

  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 0);
  TASK_CHECK (report.takes == 2); /* wrap take + extract take */
  TASK_CHECK (report.copies == 1);
  TASK_CHECK (report.finalized == 1);
  return 0;
} /* test_exceptions_extract_branches */

static int
test_exceptions_identity_preserved (void)
{
  plan_builder_t plan;
  struct jerry_scope_report report;

  plan_init (&plan);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);
  /* exception -> abort -> exception conversions keep the same identity. */
  plan_emit (&plan, SCOPE_OP_WRAP, 0, 1, SCOPE_BRANCH_ABORT_TAKE);
  plan_emit (&plan, SCOPE_OP_WRAP, 1, 2, SCOPE_BRANCH_EXCEPTION_COPY);
  plan_emit (&plan, SCOPE_OP_WRAP, 2, 3, SCOPE_BRANCH_ABORT_COPY);
  plan_emit (&plan, SCOPE_OP_EXTRACT, 3, 4, SCOPE_BRANCH_EXCEPTION_COPY);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);

  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 0);
  TASK_CHECK (report.created == 1);
  TASK_CHECK (report.finalized == 1);
  TASK_CHECK (report.takes == 1);
  TASK_CHECK (report.copies == 3);
  return 0;
} /* test_exceptions_identity_preserved */

static int
test_exceptions_extract_normal_rejected (void)
{
  plan_builder_t plan;
  struct jerry_scope_report report;

  plan_init (&plan);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_EXTRACT, 0, 1, SCOPE_BRANCH_EXCEPTION_COPY);

  memset (&report, 0xAA, sizeof (report));
  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 1);
  /* Refused plans zero the report and never reach the engine. */
  TASK_CHECK (report.steps == 0 && report.created == 0 && report.finalized == 0);
  TASK_CHECK (report.opened == 0 && report.escapes == 0);
  TASK_CHECK (report.takes == 0 && report.copies == 0 && report.comparisons == 0);
  return 0;
} /* test_exceptions_extract_normal_rejected */

/* ------------------------------------------------------------------ */
/* replay group: double cycles, fixed seeds, contract sample plan.    */
/* ------------------------------------------------------------------ */
static int
test_replay_double_cycle_matches (void)
{
  plan_builder_t plan;
  struct jerry_scope_report report;

  plan_init (&plan);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_COPY, 0, 1, 0);
  plan_emit (&plan, SCOPE_OP_WRAP, 0, 2, SCOPE_BRANCH_ABORT_TAKE);
  plan_emit (&plan, SCOPE_OP_EXTRACT, 2, 3, SCOPE_BRANCH_EXCEPTION_TAKE);
  plan_emit (&plan, SCOPE_OP_ESCAPE, 3, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);

  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 0);
  TASK_CHECK (report.created == 1);
  TASK_CHECK (report.finalized == 1);
  TASK_CHECK (report.escapes == 1);
  return 0;
} /* test_replay_double_cycle_matches */

static int
test_replay_empty_plan (void)
{
  struct jerry_scope_report report;
  uint8_t empty[1] = { 0 };

  /* Every new init/cleanup cycle, including the empty plan, must finish at
   * root with no registered root value. */
  TASK_CHECK (jerry_scope_run (empty, 0, &report) == 0);
  TASK_CHECK (report.steps == 0);
  TASK_CHECK (report.created == 0 && report.finalized == 0);
  TASK_CHECK (report.opened == 0 && report.escapes == 0);
  TASK_CHECK (report.takes == 0 && report.copies == 0);
  return 0;
} /* test_replay_empty_plan */

static int
test_replay_fixed_seeds (void)
{
  size_t seed_count = 0;

  /* 256 deterministic valid plans, each replayed twice in its own
   * jerry_init/cleanup cycle. */
  TASK_CHECK (jerry_scope_fuzz_seed_drive (&seed_count) == 0);
  TASK_CHECK (seed_count == 256);
  return 0;
} /* test_replay_fixed_seeds */

static int
test_replay_contract_sample (void)
{
  /* The exact plan consumed by task_support/api_consumer.c. */
  static const uint8_t plan[] =
  {
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 3, 0, 1, 1,
    5, 1, 0, 0, 6, 0, 0, 0, 6, 0, 0, 0, 7, 0, 0, 0,
  };
  struct jerry_scope_report report;

  TASK_CHECK (jerry_scope_run (plan, sizeof (plan), &report) == 0);
  TASK_CHECK (report.steps == 8);
  TASK_CHECK (report.created == 1);
  TASK_CHECK (report.finalized == 1);
  TASK_CHECK (report.comparisons > 0);
  TASK_CHECK (report.opened == 2);
  TASK_CHECK (report.escapes == 1);
  TASK_CHECK (report.takes == 1);
  TASK_CHECK (report.copies == 0);
  return 0;
} /* test_replay_contract_sample */

/* ------------------------------------------------------------------ */
/* diagnostics group: whole-plan refusal and first-difference data.   */
/* ------------------------------------------------------------------ */
static int
test_diagnostics_truncated (void)
{
  static const uint8_t truncated[] = { SCOPE_OP_OPEN, 0, 0 };
  struct jerry_scope_report report;
  scope_model_t model;

  TASK_CHECK (jerry_scope_run (truncated, sizeof (truncated), &report) == 1);
  TASK_CHECK (report.steps == 0);
  TASK_CHECK (scope_model_precheck (&model, truncated, sizeof (truncated)) == 1);
  TASK_CHECK (model.diff.valid == 1);
  TASK_CHECK (model.diff.step == 0);
  TASK_CHECK (model.diff.reason != NULL);
  return 0;
} /* test_diagnostics_truncated */

static int
test_diagnostics_unknown_op (void)
{
  static const uint8_t bad_op[] = { 8, 0, 0, 0 };
  struct jerry_scope_report report;
  scope_model_t model;

  TASK_CHECK (jerry_scope_run (bad_op, sizeof (bad_op), &report) == 1);
  TASK_CHECK (scope_model_precheck (&model, bad_op, sizeof (bad_op)) == 1);
  TASK_CHECK (model.diff.valid == 1);
  TASK_CHECK (model.diff.step == 0);
  TASK_CHECK (model.diff.op == 8);
  return 0;
} /* test_diagnostics_unknown_op */

static int
test_diagnostics_invalid_ticket (void)
{
  static const uint8_t bad_ref[] = { SCOPE_OP_COPY, 7, 9, 0 };
  scope_model_t model;

  TASK_CHECK (scope_model_precheck (&model, bad_ref, sizeof (bad_ref)) == 1);
  TASK_CHECK (model.diff.valid == 1);
  TASK_CHECK (model.diff.step == 0);
  TASK_CHECK (model.diff.ticket == 7);
  TASK_CHECK (model.diff.ticket_scope == -1);
  TASK_CHECK (model.diff.reason != NULL);
  return 0;
} /* test_diagnostics_invalid_ticket */

static int
test_diagnostics_dead_ticket (void)
{
  plan_builder_t plan;
  scope_model_t model;
  struct jerry_scope_report report;

  plan_init (&plan);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);   /* step 0 */
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);    /* step 1 */
  plan_emit (&plan, SCOPE_OP_CLOSE, 0, 0, 0);  /* step 2: ticket 0 dies */
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);     /* step 3 */
  plan_emit (&plan, SCOPE_OP_COPY, 0, 1, 0);   /* step 4: dead reference */

  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 1);
  TASK_CHECK (scope_model_precheck (&model, plan.bytes, plan.length) == 1);
  TASK_CHECK (model.diff.valid == 1);
  TASK_CHECK (model.diff.step == 4);
  TASK_CHECK (model.diff.depth == 0);
  TASK_CHECK (model.diff.ticket == 0);
  TASK_CHECK (model.diff.ticket_scope == -1);
  return 0;
} /* test_diagnostics_dead_ticket */

static int
test_diagnostics_unbalanced (void)
{
  plan_builder_t plan;
  scope_model_t model;
  struct jerry_scope_report report;

  plan_init (&plan);
  plan_emit (&plan, SCOPE_OP_OPEN, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_NEW, 0, 0, 0);
  plan_emit (&plan, SCOPE_OP_GC, 0, 0, 0);

  TASK_CHECK (jerry_scope_run (plan.bytes, plan.length, &report) == 1);
  TASK_CHECK (scope_model_precheck (&model, plan.bytes, plan.length) == 1);
  TASK_CHECK (model.diff.valid == 1);
  TASK_CHECK (model.diff.depth == 1);
  TASK_CHECK (model.diff.reason != NULL);
  return 0;
} /* test_diagnostics_unbalanced */

static int
test_diagnostics_model_compare (void)
{
  scope_model_t left;
  scope_model_t right;
  scope_diff_t diff;
  scope_record_t open_record = { SCOPE_OP_OPEN, 0, 0, 0 };
  scope_record_t new_record = { SCOPE_OP_NEW, 0, 0, 0 };

  scope_model_init (&left);
  scope_model_init (&right);

  TASK_CHECK (scope_model_apply (&left, &open_record) == 0);
  TASK_CHECK (scope_model_apply (&right, &open_record) == 0);
  TASK_CHECK (scope_model_apply (&left, &new_record) == 0);
  TASK_CHECK (scope_model_apply (&right, &new_record) == 0);
  scope_model_snapshot (&left, &new_record, SCOPE_ESCAPE_NONE);
  scope_model_snapshot (&right, &new_record, SCOPE_ESCAPE_NONE);

  /* Identical histories compare cleanly. */
  TASK_CHECK (scope_model_compare (&left, &right, &diff) == 0);
  TASK_CHECK (diff.valid == 0);

  /* A divergent extra step is reported with its position and depth. */
  TASK_CHECK (scope_model_apply (&right, &open_record) == 0);
  scope_model_snapshot (&right, &open_record, SCOPE_ESCAPE_NONE);
  TASK_CHECK (scope_model_compare (&left, &right, &diff) != 0);
  TASK_CHECK (diff.valid == 1);
  TASK_CHECK (diff.step == 1);
  TASK_CHECK (diff.depth == 2);
  return 0;
} /* test_diagnostics_model_compare */

static int
test_diagnostics_gc_invariant (void)
{
  scope_model_t model;
  scope_record_t record;

  scope_model_init (&model);
  record.op = SCOPE_OP_OPEN; record.a = 0; record.b = 0; record.flags = 0;
  TASK_CHECK (scope_model_apply (&model, &record) == 0);
  record.op = SCOPE_OP_NEW; record.a = 0;
  TASK_CHECK (scope_model_apply (&model, &record) == 0);

  /* The object still has responsibility: claiming it was finalized fails. */
  scope_model_note_finalized (&model, 0);
  TASK_CHECK (scope_model_check_gc (&model, 2) != 0);
  TASK_CHECK (model.diff.valid == 1);
  TASK_CHECK (model.diff.identity == 0);
  TASK_CHECK (model.diff.step == 2);
  return 0;
} /* test_diagnostics_gc_invariant */

const struct task_case task_cases[] =
{
  /* scopes */
  { "scopes", "nested_open_close", test_scopes_nested },
  { "scopes", "escape_promotion", test_scopes_escape_promotion },
  { "scopes", "escape_twice_model", test_scopes_escape_twice },
  { "scopes", "dynamic_pool_thresholds", test_scopes_dynamic_thresholds },
  { "scopes", "reopen_after_close", test_scopes_reopen_after_close },
  /* exceptions */
  { "exceptions", "wrap_take_copy_branches", test_exceptions_wrap_branches },
  { "exceptions", "extract_take_copy_branches", test_exceptions_extract_branches },
  { "exceptions", "identity_through_wrapper_flips", test_exceptions_identity_preserved },
  { "exceptions", "extract_normal_refused", test_exceptions_extract_normal_rejected },
  /* replay */
  { "replay", "double_cycle_matches", test_replay_double_cycle_matches },
  { "replay", "empty_plan_clean_cycle", test_replay_empty_plan },
  { "replay", "fixed_seed_drive_256", test_replay_fixed_seeds },
  { "replay", "contract_sample_plan", test_replay_contract_sample },
  /* diagnostics */
  { "diagnostics", "truncated_plan_refused", test_diagnostics_truncated },
  { "diagnostics", "unknown_op_refused", test_diagnostics_unknown_op },
  { "diagnostics", "invalid_ticket_refused", test_diagnostics_invalid_ticket },
  { "diagnostics", "dead_ticket_refused", test_diagnostics_dead_ticket },
  { "diagnostics", "unbalanced_plan_refused", test_diagnostics_unbalanced },
  { "diagnostics", "first_difference_compare", test_diagnostics_model_compare },
  { "diagnostics", "gc_invariant_difference", test_diagnostics_gc_invariant },
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
