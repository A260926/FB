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
 * Pure logical model of the two JerryScript cleanup chains
 * (jerry-ext handle scopes and the jerry-core exception/abort value
 * ownership rules).  The model only stores tickets, logical object
 * identities, value labels and scope ownership: it must never include
 * jerryscript.h, read engine internals or hold a jerry_value_t.
 */
#ifndef TESTS_FUZZ_SCOPE_MODEL_H
#define TESTS_FUZZ_SCOPE_MODEL_H

#include <stddef.h>
#include <stdint.h>

/* Plan limits fixed by task_support/contract.md. */
#define SCOPE_PLAN_MAX_STEPS 4096
#define SCOPE_PLAN_TICKET_COUNT 256
#define SCOPE_PLAN_MAX_IDENTITIES 128
#define SCOPE_PLAN_MAX_DEPTH 64

/* Operation codes of the four-byte (op, a, b, flags) records. */
typedef enum
{
  SCOPE_OP_OPEN = 0,
  SCOPE_OP_NEW = 1,
  SCOPE_OP_COPY = 2,
  SCOPE_OP_WRAP = 3,
  SCOPE_OP_EXTRACT = 4,
  SCOPE_OP_ESCAPE = 5,
  SCOPE_OP_CLOSE = 6,
  SCOPE_OP_GC = 7,
  SCOPE_OP__COUNT
} scope_op_t;

/* WRAP/EXTRACT ownership branches; take and copy stay distinct. */
typedef enum
{
  SCOPE_BRANCH_EXCEPTION_COPY = 0,
  SCOPE_BRANCH_EXCEPTION_TAKE = 1,
  SCOPE_BRANCH_ABORT_COPY = 2,
  SCOPE_BRANCH_ABORT_TAKE = 3
} scope_branch_t;

/* Ordinary value vs. exception wrapper vs. abort wrapper. */
typedef enum
{
  SCOPE_LABEL_NORMAL = 0,
  SCOPE_LABEL_EXCEPTION = 1,
  SCOPE_LABEL_ABORT = 2
} scope_label_t;

/* Result of an ESCAPE step as predicted by the model. */
typedef enum
{
  SCOPE_ESCAPE_NONE = 0,
  SCOPE_ESCAPE_OK = 1,
  SCOPE_ESCAPE_TWICE = 2
} scope_escape_result_t;

/*
 * One decoded plan record. Unused fields are validated to be zero by the
 * pre-check so the model and the engine see exactly the same intent.
 */
typedef struct
{
  uint8_t op;
  uint8_t a;
  uint8_t b;
  uint8_t flags;
} scope_record_t;

/*
 * Immutable per-step snapshot of the model state used to compare two
 * independent replays.  Only logical data is recorded, never engine
 * addresses.
 */
typedef struct
{
  size_t step;                  /* record index, zero based */
  uint8_t op;                   /* operation executed */
  int depth;                    /* live explicit scope depth after the step */
  scope_escape_result_t escape; /* ESCAPE outcome, NONE for other ops */
  uint32_t live_owners;         /* tickets currently owning a value */
  uint32_t alive_mask[(SCOPE_PLAN_MAX_IDENTITIES + 31) / 32];
  uint32_t finalized_mask[(SCOPE_PLAN_MAX_IDENTITIES + 31) / 32];
  uint32_t final_count_mask[(SCOPE_PLAN_MAX_IDENTITIES + 31) / 32];
} scope_step_state_t;

/* First-difference diagnostics: position, depth, responsibility, identity. */
typedef struct
{
  int valid;
  size_t step;
  uint8_t op;
  int depth;
  uint8_t ticket;
  int ticket_scope;
  uint8_t identity;
  scope_label_t label;
  const char *reason;
} scope_diff_t;

/*
 * Logical model of one replay.  Tickets are the 0..255 handle slots,
 * identities are the 0..127 logical objects, and each owned ticket records
 * the scope depth responsible for releasing it (root scope is depth 0).
 */
typedef struct
{
  scope_record_t steps[SCOPE_PLAN_MAX_STEPS];
  size_t step_count;

  /* Ticket bookkeeping. */
  uint8_t ticket_identity[SCOPE_PLAN_TICKET_COUNT];
  uint8_t ticket_label[SCOPE_PLAN_TICKET_COUNT];
  int8_t ticket_scope[SCOPE_PLAN_TICKET_COUNT]; /* -1: unused/free slot */

  /* Per-scope bookkeeping; index 0 is the root scope. */
  uint8_t scope_escaped[SCOPE_PLAN_MAX_DEPTH + 1];
  int depth;
  int max_depth;

  /* Identity bookkeeping. */
  uint8_t identity_refs[SCOPE_PLAN_MAX_IDENTITIES]; /* live owning tickets */
  uint8_t identity_final_count[SCOPE_PLAN_MAX_IDENTITIES];
  uint8_t identity_created;
  uint32_t alive_mask[(SCOPE_PLAN_MAX_IDENTITIES + 31) / 32];
  uint32_t finalized_mask[(SCOPE_PLAN_MAX_IDENTITIES + 31) / 32];

  /* Trailing event log, compared between the two replays. */
  scope_step_state_t trace[SCOPE_PLAN_MAX_STEPS];
  size_t trace_count;
  uint16_t gc_events[SCOPE_PLAN_MAX_STEPS];
  size_t gc_event_count;
  scope_escape_result_t last_escape;

  scope_diff_t diff;
  uint32_t comparisons;
} scope_model_t;

/* Lifecycle. */
void scope_model_init (scope_model_t *model_p);

/*
 * Whole-plan validation pass. Runs purely on decoded records and never
 * touches the engine: rejects truncation, invalid slots/flags, references to
 * dead tickets, re-use of live destination slots and unbalanced scopes.
 * Returns 1 (and fills the model diff) when the plan must be refused.
 */
int scope_model_precheck (scope_model_t *model_p, const uint8_t *data, size_t size);

/* Apply one validated record to the logical model. */
int scope_model_apply (scope_model_t *model_p, const scope_record_t *record_p);

/* Engine-side finalization hook: record that a logical identity died once. */
void scope_model_note_finalized (scope_model_t *model_p, uint8_t identity);

/* GC invariant: zero-responsibility identities finalized exactly once. */
int scope_model_check_gc (scope_model_t *model_p, size_t step);

/* End-of-plan invariant: every created identity finalized exactly once. */
int scope_model_check_finish (scope_model_t *model_p);

/* Compare two step-by-step traces and fill the first difference. */
int scope_model_compare (const scope_model_t *first_p, const scope_model_t *second_p, scope_diff_t *out_p);

/* Snapshot the current model state at the end of a step. */
void scope_model_snapshot (scope_model_t *model_p, const scope_record_t *record_p, scope_escape_result_t escape);

/* Helpers shared with the replay engine. */
int scope_model_ticket_alive (const scope_model_t *model_p, uint8_t ticket);
const char *scope_label_name (scope_label_t label);

#endif /* !TESTS_FUZZ_SCOPE_MODEL_H */
