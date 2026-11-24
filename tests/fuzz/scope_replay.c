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
 * Replay engine. Each validated plan is executed twice, every run inside its
 * own jerry_init/cleanup cycle, while the pure scope_model_t audits ticket
 * responsibility, wrapper labels, object identities and finalization.
 *
 * Ownership rules followed here:
 *  - scope-registered values are released only by closing their scope;
 *  - jerryx_remove_handle and manual frees of registered values are unused;
 *  - take branches first copy the source into an independent temporary;
 *  - the model never sees a jerry_value_t.
 */
#include "jerryscript.h"
#include "jerryscript-ext/handle-scope.h"

#include "tests/fuzz/scope_model.h"
#include "tests/fuzz/scope_replay.h"

#include <stdio.h>
#include <string.h>

/* Stable per-identity native pointer storage, reset every engine cycle. */
static int s_identity_storage[SCOPE_PLAN_MAX_IDENTITIES];

/* Engine-side finalization bookkeeping; the model keeps its own copy. */
static uint8_t s_engine_final_count[SCOPE_PLAN_MAX_IDENTITIES];
static scope_model_t *s_active_model_p;

static void
scope_finalize_cb (void *native_p, jerry_object_native_info_t *info_p)
{
  (void) info_p;
  int identity = *(int *) native_p;

  /* The callback only records the identity: it never calls back into the
   * engine and never holds a jerry value. */
  if (identity >= 0 && identity < SCOPE_PLAN_MAX_IDENTITIES)
  {
    s_engine_final_count[identity]++;
    if (s_active_model_p != NULL)
    {
      scope_model_note_finalized (s_active_model_p, (uint8_t) identity);
    }
  }
} /* scope_finalize_cb */

static const jerry_object_native_info_t scope_native_info = {
  .free_cb = scope_finalize_cb,
  .number_of_references = 0,
  .offset_of_references = 0,
};

typedef struct
{
  jerry_value_t handles[SCOPE_PLAN_TICKET_COUNT];
  int8_t owner[SCOPE_PLAN_TICKET_COUNT]; /* scope depth owning the ticket */
  jerryx_handle_scope scopes[SCOPE_PLAN_MAX_DEPTH + 1];
  int depth;
  size_t opened;
  size_t escapes;
  size_t takes;
  size_t copies;
} scope_engine_t;

static void
scope_engine_init (scope_engine_t *engine_p)
{
  memset (engine_p, 0, sizeof (*engine_p));
  memset (engine_p->owner, -1, sizeof (engine_p->owner));
} /* scope_engine_init */

/*
 * Release every still-open scope in reverse creation order so the engine
 * heap is empty when jerry_cleanup runs. Used both by normal CLOSE steps and
 * by the failure path: jerry_cleanup itself does not know jerryx scopes.
 */
static void
scope_engine_close_all (scope_engine_t *engine_p)
{
  while (engine_p->depth > 0)
  {
    jerryx_close_escapable_handle_scope (engine_p->scopes[engine_p->depth]);
    engine_p->depth--;
  }
} /* scope_engine_close_all */

static void
scope_diff_print (const char *phase_p, const scope_diff_t *diff_p)
{
  if (!diff_p->valid)
  {
    fprintf (stderr, "SCOPE_DIVERGENCE phase=%s reason=%s\n", phase_p,
             diff_p->reason != NULL ? diff_p->reason : "unspecified");
    return;
  }
  fprintf (stderr,
           "SCOPE_DIVERGENCE phase=%s step=%zu op=%u depth=%d ticket=%u "
           "ticket_scope=%d identity=%u label=%s reason=%s\n",
           phase_p, diff_p->step, (unsigned) diff_p->op, diff_p->depth,
           (unsigned) diff_p->ticket, diff_p->ticket_scope,
           (unsigned) diff_p->identity, scope_label_name (diff_p->label),
           diff_p->reason != NULL ? diff_p->reason : "unspecified");
} /* scope_diff_print */

static int
scope_engine_label_matches (jerry_value_t value, scope_label_t label)
{
  if (label == SCOPE_LABEL_NORMAL)
  {
    return !jerry_value_is_exception (value);
  }
  if (!jerry_value_is_exception (value))
  {
    return 0;
  }
  if (label == SCOPE_LABEL_ABORT)
  {
    return jerry_value_is_abort (value);
  }
  return !jerry_value_is_abort (value);
} /* scope_engine_label_matches */

/*
 * Verify that the engine value carries the same logical identity as the
 * model. Wrapper values are unwrapped with a borrowed copy that is released
 * locally; scope-registered values are never freed here.
 */
static int
scope_engine_identity_matches (scope_model_t *model_p, jerry_value_t value, uint8_t expected_identity)
{
  jerry_value_t object = value;
  int borrowed = 0;

  model_p->comparisons++;
  if (jerry_value_is_exception (value))
  {
    object = jerry_exception_value (value, false);
    borrowed = 1;
  }

  if (!jerry_value_is_object (object))
  {
    if (borrowed)
    {
      jerry_value_free (object);
    }
    return 0;
  }

  int *identity_p = (int *) jerry_object_get_native_ptr (object, &scope_native_info);
  int ok = (identity_p != NULL && *identity_p == expected_identity);

  if (borrowed)
  {
    jerry_value_free (object);
  }
  return ok;
} /* scope_engine_identity_matches */

typedef struct
{
  uint8_t seen[SCOPE_PLAN_MAX_IDENTITIES];
  size_t count;
} scope_live_scan_t;

static bool
scope_live_cb (const jerry_value_t object, void *object_data_p, void *user_data_p)
{
  (void) object;
  scope_live_scan_t *scan_p = (scope_live_scan_t *) user_data_p;
  int identity = *(int *) object_data_p;

  if (identity >= 0 && identity < SCOPE_PLAN_MAX_IDENTITIES && !scan_p->seen[identity])
  {
    scan_p->seen[identity] = 1;
    scan_p->count++;
  }
  /* foreach treats a false callback result as "stop"; true keeps walking. */
  return true;
} /* scope_live_cb */

/*
 * Cross-check the engine's live object set against the model after a GC:
 * every model-live identity must be found on the engine and every
 * zero-responsibility identity must already have been finalized exactly once.
 */
static int
scope_engine_check_gc (scope_model_t *model_p, size_t step)
{
  scope_live_scan_t scan;
  uint8_t identity;

  memset (&scan, 0, sizeof (scan));
  jerry_foreach_live_object_with_info (&scope_native_info, scope_live_cb, &scan);
  model_p->comparisons++;

  for (identity = 0; identity < model_p->identity_created; identity++)
  {
    int model_live = (model_p->identity_refs[identity] != 0);
    int engine_live = (scan.seen[identity] != 0);

    model_p->comparisons++;
    if (model_live != engine_live)
    {
      model_p->diff.valid = 1;
      model_p->diff.step = step;
      model_p->diff.depth = model_p->depth;
      model_p->diff.identity = identity;
      model_p->diff.reason = model_live ? "model-live object missing from engine"
                                        : "engine kept an object the model released";
      return -1;
    }
    if (s_engine_final_count[identity] != model_p->identity_final_count[identity])
    {
      model_p->comparisons++;
      model_p->diff.valid = 1;
      model_p->diff.step = step;
      model_p->diff.identity = identity;
      model_p->diff.reason = "engine and model finalization counts differ";
      return -1;
    }
  }
  return scope_model_check_gc (model_p, step);
} /* scope_engine_check_gc */

static int
scope_engine_fail (scope_model_t *model_p, const scope_record_t *record_p, const char *reason_p)
{
  if (!model_p->diff.valid)
  {
    model_p->diff.valid = 1;
    model_p->diff.step = model_p->trace_count;
    model_p->diff.op = record_p->op;
    model_p->diff.depth = model_p->depth;
    model_p->diff.reason = reason_p;
  }
  return -1;
} /* scope_engine_fail */

static int
scope_engine_run_cycle (const uint8_t *data, size_t size,
                        scope_model_t *model_p, struct jerry_scope_report *report_p)
{
  scope_engine_t engine;
  size_t index;
  int result = 2;

  memset (report_p, 0, sizeof (*report_p));
  if (scope_model_precheck (model_p, data, size) != 0)
  {
    return 1;
  }

  scope_engine_init (&engine);
  memset (s_engine_final_count, 0, sizeof (s_engine_final_count));
  memset (s_identity_storage, 0, sizeof (s_identity_storage));
  s_active_model_p = model_p;

  jerry_init (JERRY_INIT_EMPTY);
  if (jerryx_handle_scope_get_current () != jerryx_handle_scope_get_root ())
  {
    scope_engine_fail (model_p, &model_p->steps[0], "fresh engine did not start at root scope");
    goto cleanup;
  }

  for (index = 0; index < model_p->step_count; index++)
  {
    const scope_record_t *record_p = &model_p->steps[index];
    scope_escape_result_t expected_escape = SCOPE_ESCAPE_NONE;

    if (scope_model_apply (model_p, record_p) != 0)
    {
      scope_diff_print ("model", &model_p->diff);
      goto cleanup;
    }
    expected_escape = model_p->last_escape;

    switch (record_p->op)
    {
      case SCOPE_OP_OPEN:
      {
        jerryx_handle_scope_status status;
        status = jerryx_open_escapable_handle_scope (&engine.scopes[engine.depth + 1]);
        model_p->comparisons++;
        if (status != jerryx_handle_scope_ok)
        {
          scope_engine_fail (model_p, record_p, "open escapable scope failed");
          goto cleanup;
        }
        engine.depth++;
        engine.opened++;
        break;
      }

      case SCOPE_OP_NEW:
      {
        uint8_t ticket = record_p->a;
        uint8_t identity = model_p->ticket_identity[ticket];
        jerry_value_t object;

        s_identity_storage[identity] = identity;
        object = jerry_object ();
        jerry_object_set_native_ptr (object, &scope_native_info, &s_identity_storage[identity]);
        engine.handles[ticket] = jerryx_create_handle (object);
        engine.owner[ticket] = (int8_t) engine.depth;

        model_p->comparisons++;
        if (!jerry_value_is_object (engine.handles[ticket]))
        {
          scope_engine_fail (model_p, record_p, "NEW did not produce an object");
          goto cleanup;
        }
        if (scope_engine_identity_matches (model_p, engine.handles[ticket], identity) != 1)
        {
          scope_engine_fail (model_p, record_p, "NEW identity mismatch");
          goto cleanup;
        }
        break;
      }

      case SCOPE_OP_COPY:
      {
        uint8_t src = record_p->a;
        uint8_t dst = record_p->b;
        uint8_t identity = model_p->ticket_identity[dst];
        scope_label_t label = (scope_label_t) model_p->ticket_label[dst];

        engine.handles[dst] = jerryx_create_handle (jerry_value_copy (engine.handles[src]));
        engine.owner[dst] = (int8_t) engine.depth;

        model_p->comparisons++;
        if (!scope_engine_label_matches (engine.handles[dst], label))
        {
          scope_engine_fail (model_p, record_p, "COPY label changed");
          goto cleanup;
        }
        if (scope_engine_identity_matches (model_p, engine.handles[dst], identity) != 1)
        {
          scope_engine_fail (model_p, record_p, "COPY identity mismatch");
          goto cleanup;
        }
        break;
      }

      case SCOPE_OP_WRAP:
      {
        uint8_t src = record_p->a;
        uint8_t dst = record_p->b;
        uint8_t identity = model_p->ticket_identity[dst];
        scope_label_t label = (scope_label_t) model_p->ticket_label[dst];
        int take = (record_p->flags & 1);
        int abort_wrap = (record_p->flags & 2);
        jerry_value_t input = engine.handles[src];
        jerry_value_t wrapped;

        if (take)
        {
          /* Independent temporary responsibility handed over to the engine. */
          jerry_value_t temporary = jerry_value_copy (input);
          wrapped = abort_wrap ? jerry_throw_abort (temporary, true)
                               : jerry_throw_value (temporary, true);
          engine.takes++;
        }
        else
        {
          wrapped = abort_wrap ? jerry_throw_abort (input, false)
                               : jerry_throw_value (input, false);
          engine.copies++;
        }

        engine.handles[dst] = jerryx_create_handle (wrapped);
        engine.owner[dst] = (int8_t) engine.depth;

        model_p->comparisons++;
        if (!scope_engine_label_matches (engine.handles[dst], label))
        {
          scope_engine_fail (model_p, record_p, "WRAP label mismatch");
          goto cleanup;
        }
        if (scope_engine_identity_matches (model_p, engine.handles[dst], identity) != 1)
        {
          scope_engine_fail (model_p, record_p, "WRAP identity mismatch");
          goto cleanup;
        }
        /* The source ticket keeps its responsibility and its label. It may
         * live in the current scope or in any still-open outer scope. */
        model_p->comparisons++;
        if (engine.owner[src] < 0 || engine.owner[src] > engine.depth
            || engine.handles[src] == 0)
        {
          scope_engine_fail (model_p, record_p, "WRAP disturbed source ownership");
          goto cleanup;
        }
        break;
      }

      case SCOPE_OP_EXTRACT:
      {
        uint8_t src = record_p->a;
        uint8_t dst = record_p->b;
        uint8_t identity = model_p->ticket_identity[dst];
        jerry_value_t payload;

        if (record_p->flags == SCOPE_BRANCH_EXCEPTION_TAKE)
        {
          jerry_value_t temporary = jerry_value_copy (engine.handles[src]);
          payload = jerry_exception_value (temporary, true);
          engine.takes++;
        }
        else
        {
          payload = jerry_exception_value (engine.handles[src], false);
          engine.copies++;
        }

        engine.handles[dst] = jerryx_create_handle (payload);
        engine.owner[dst] = (int8_t) engine.depth;

        model_p->comparisons++;
        if (!scope_engine_label_matches (engine.handles[dst], SCOPE_LABEL_NORMAL))
        {
          scope_engine_fail (model_p, record_p, "EXTRACT result is not a normal value");
          goto cleanup;
        }
        if (scope_engine_identity_matches (model_p, engine.handles[dst], identity) != 1)
        {
          scope_engine_fail (model_p, record_p, "EXTRACT identity mismatch");
          goto cleanup;
        }
        model_p->comparisons++;
        if (engine.owner[src] < 0 || engine.owner[src] > engine.depth
            || engine.handles[src] == 0)
        {
          scope_engine_fail (model_p, record_p, "EXTRACT released source responsibility");
          goto cleanup;
        }
        break;
      }

      case SCOPE_OP_ESCAPE:
      {
        uint8_t ticket = record_p->a;
        jerry_value_t escaped = 0;
        jerryx_handle_scope_status status;

        status = jerryx_escape_handle (engine.scopes[engine.depth],
                                       engine.handles[ticket], &escaped);
        model_p->comparisons++;
        if (expected_escape == SCOPE_ESCAPE_TWICE)
        {
          if (status != jerryx_escape_called_twice)
          {
            scope_engine_fail (model_p, record_p, "second escape not rejected");
            goto cleanup;
          }
          /* Model and engine ownership stay untouched. */
          model_p->comparisons++;
          if (engine.owner[ticket] != (int8_t) engine.depth)
          {
            scope_engine_fail (model_p, record_p, "rejected escape moved ownership");
            goto cleanup;
          }
        }
        else
        {
          if (status != jerryx_handle_scope_ok || expected_escape != SCOPE_ESCAPE_OK)
          {
            scope_engine_fail (model_p, record_p, "escape prediction mismatch");
            goto cleanup;
          }
          engine.handles[ticket] = escaped;
          engine.owner[ticket] = (int8_t) engine.depth - 1;
          engine.escapes++;
        }
        break;
      }

      case SCOPE_OP_CLOSE:
      {
        uint16_t ticket;

        model_p->comparisons++;
        if (jerryx_close_escapable_handle_scope (engine.scopes[engine.depth])
            != jerryx_handle_scope_ok)
        {
          scope_engine_fail (model_p, record_p, "closing scope failed");
          goto cleanup;
        }
        for (ticket = 0; ticket < SCOPE_PLAN_TICKET_COUNT; ticket++)
        {
          if (engine.owner[ticket] == engine.depth)
          {
            engine.owner[ticket] = -1;
            engine.handles[ticket] = 0;
          }
        }
        engine.depth--;
        break;
      }

      case SCOPE_OP_GC:
      {
        jerry_heap_gc (JERRY_GC_PRESSURE_HIGH);
        if (scope_engine_check_gc (model_p, index) != 0)
        {
          scope_diff_print ("gc", &model_p->diff);
          goto cleanup;
        }
        break;
      }

      default:
        scope_engine_fail (model_p, record_p, "unhandled op in engine");
        goto cleanup;
    }

    model_p->comparisons++;
    if (engine.depth != model_p->depth)
    {
      scope_engine_fail (model_p, record_p, "engine/model scope depth diverged");
      goto cleanup;
    }
    scope_model_snapshot (model_p, record_p, expected_escape);
  }

  /* Balanced plan: every explicit scope is closed and root holds nothing. */
  model_p->comparisons++;
  if (engine.depth != 0 || model_p->depth != 0
      || jerryx_handle_scope_get_current () != jerryx_handle_scope_get_root ())
  {
    model_p->diff.valid = 1;
    model_p->diff.reason = "plan ended away from root scope";
    scope_diff_print ("finish", &model_p->diff);
    goto cleanup;
  }

  jerry_heap_gc (JERRY_GC_PRESSURE_HIGH);
  if (scope_engine_check_gc (model_p, model_p->step_count) != 0)
  {
    scope_diff_print ("finish-gc", &model_p->diff);
    goto cleanup;
  }

  jerry_cleanup ();
  s_active_model_p = NULL;

  /* After cleanup every logical identity was finalized exactly once. */
  {
    uint8_t identity;
    for (identity = 0; identity < model_p->identity_created; identity++)
    {
      model_p->comparisons++;
      if (s_engine_final_count[identity] != 1)
      {
        model_p->diff.valid = 1;
        model_p->diff.identity = identity;
        model_p->diff.reason = "engine finalized an object other than exactly once";
        scope_diff_print ("cleanup", &model_p->diff);
        goto cleanup_after;
      }
    }
  }

  if (scope_model_check_finish (model_p) != 0)
  {
    scope_diff_print ("model-finish", &model_p->diff);
    goto cleanup_after;
  }
  if (jerryx_handle_scope_get_current () != jerryx_handle_scope_get_root ())
  {
    model_p->diff.valid = 1;
    model_p->diff.reason = "root scope not restored after cleanup";
    scope_diff_print ("cleanup", &model_p->diff);
    goto cleanup_after;
  }

  report_p->steps = model_p->step_count;
  report_p->created = model_p->identity_created;
  {
    size_t identity;
    for (identity = 0; identity < model_p->identity_created; identity++)
    {
      report_p->finalized += model_p->identity_final_count[identity];
    }
  }
  report_p->comparisons = model_p->comparisons;
  report_p->opened = engine.opened;
  report_p->escapes = engine.escapes;
  report_p->takes = engine.takes;
  report_p->copies = engine.copies;
  return 0;

cleanup:
  scope_engine_close_all (&engine);
  jerry_cleanup ();
  s_active_model_p = NULL;
cleanup_after:
  return result;
} /* scope_engine_run_cycle */

int
jerry_scope_run (const uint8_t *data, size_t size, struct jerry_scope_report *out)
{
  scope_model_t first_model;
  scope_model_t second_model;
  struct jerry_scope_report first_report;
  struct jerry_scope_report second_report;
  scope_diff_t diff;
  int first_rc;
  int second_rc;

  if (out == NULL)
  {
    return 1;
  }
  memset (out, 0, sizeof (*out));

  /* Whole-plan refusal happens before any engine initialization. */
  {
    scope_model_t probe;
    if (scope_model_precheck (&probe, data, size) != 0)
    {
      scope_diff_print ("precheck", &probe.diff);
      return 1;
    }
  }

  first_rc = scope_engine_run_cycle (data, size, &first_model, &first_report);
  if (first_rc != 0)
  {
    return first_rc == 1 ? 1 : 2;
  }

  /* First-round state is fully retained in first_model; the second cycle is a
   * clean startup with its own model and its own init/cleanup pair. */
  second_rc = scope_engine_run_cycle (data, size, &second_model, &second_report);
  if (second_rc != 0)
  {
    return second_rc == 1 ? 1 : 2;
  }

  if (scope_model_compare (&first_model, &second_model, &diff) != 0)
  {
    scope_diff_print ("replay-compare", &diff);
    return 2;
  }
  if (memcmp (&first_report, &second_report, sizeof (first_report)) != 0)
  {
    fprintf (stderr, "SCOPE_DIVERGENCE phase=report reason=replay counters differ\n");
    return 2;
  }

  *out = first_report;
  return 0;
} /* jerry_scope_run */

/* ------------------------------------------------------------------ */
/* Deterministic fixed-seed plan generator. Plans are constructed to be */
/* valid by construction and independently re-validated before replay.  */
/* ------------------------------------------------------------------ */

#define SCOPE_SEED_COUNT 256

typedef struct
{
  uint8_t bytes[SCOPE_PLAN_MAX_STEPS * 4u];
  size_t length;
  uint32_t rng;
  int depth;
  int16_t ticket_owner[SCOPE_PLAN_TICKET_COUNT];
  uint8_t ticket_label[SCOPE_PLAN_TICKET_COUNT];
  uint8_t ticket_identity[SCOPE_PLAN_TICKET_COUNT];
  uint8_t scope_escaped[SCOPE_PLAN_MAX_DEPTH + 1];
  uint16_t scope_handles[SCOPE_PLAN_MAX_DEPTH + 1];
  uint16_t max_handles_in_scope;
  uint16_t next_ticket;
  uint8_t identities;
} scope_generator_t;

static uint32_t
scope_gen_rand (scope_generator_t *gen_p)
{
  uint32_t x = gen_p->rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  gen_p->rng = x;
  return x;
} /* scope_gen_rand */

static void
scope_gen_emit (scope_generator_t *gen_p, uint8_t op, uint8_t a, uint8_t b, uint8_t flags)
{
  size_t offset = gen_p->length;
  if (gen_p->length >= sizeof (gen_p->bytes))
  {
    return;
  }
  gen_p->bytes[offset] = op;
  gen_p->bytes[offset + 1u] = a;
  gen_p->bytes[offset + 2u] = b;
  gen_p->bytes[offset + 3u] = flags;
  gen_p->length += 4u;
} /* scope_gen_emit */

static void
scope_gen_init (scope_generator_t *gen_p, uint32_t seed)
{
  memset (gen_p, 0, sizeof (*gen_p));
  memset (gen_p->ticket_owner, -1, sizeof (gen_p->ticket_owner));
  gen_p->rng = seed ^ 0x9e3779b9u;
  if (gen_p->rng == 0)
  {
    gen_p->rng = 1u;
  }
} /* scope_gen_init */

static int
scope_gen_records_left (const scope_generator_t *gen_p)
{
  return (int) (SCOPE_PLAN_MAX_STEPS - gen_p->length / 4u);
} /* scope_gen_records_left */

static void
scope_gen_open (scope_generator_t *gen_p)
{
  if (gen_p->depth >= SCOPE_PLAN_MAX_DEPTH || scope_gen_records_left (gen_p) < 2)
  {
    return;
  }
  scope_gen_emit (gen_p, SCOPE_OP_OPEN, 0, 0, 0);
  gen_p->depth++;
  gen_p->scope_escaped[gen_p->depth] = 0;
  gen_p->scope_handles[gen_p->depth] = 0;
} /* scope_gen_open */

static void
scope_gen_close (scope_generator_t *gen_p)
{
  uint16_t ticket;

  if (gen_p->depth == 0)
  {
    return;
  }
  scope_gen_emit (gen_p, SCOPE_OP_CLOSE, 0, 0, 0);
  for (ticket = 0; ticket < SCOPE_PLAN_TICKET_COUNT; ticket++)
  {
    if (gen_p->ticket_owner[ticket] == gen_p->depth)
    {
      gen_p->ticket_owner[ticket] = -1;
    }
  }
  gen_p->depth--;
} /* scope_gen_close */

static int
scope_gen_alloc_ticket (scope_generator_t *gen_p)
{
  while (gen_p->next_ticket < SCOPE_PLAN_TICKET_COUNT
         && gen_p->ticket_owner[gen_p->next_ticket] >= 0)
  {
    gen_p->next_ticket++;
  }
  if (gen_p->next_ticket >= SCOPE_PLAN_TICKET_COUNT)
  {
    return -1;
  }
  return (int) gen_p->next_ticket;
} /* scope_gen_alloc_ticket */

static int
scope_gen_pick_live (scope_generator_t *gen_p, int require_wrapper)
{
  uint16_t candidates[SCOPE_PLAN_TICKET_COUNT];
  size_t count = 0;
  uint16_t ticket;

  for (ticket = 0; ticket < SCOPE_PLAN_TICKET_COUNT; ticket++)
  {
    if (gen_p->ticket_owner[ticket] < 0)
    {
      continue;
    }
    if (require_wrapper && gen_p->ticket_label[ticket] == SCOPE_LABEL_NORMAL)
    {
      continue;
    }
    candidates[count++] = ticket;
  }
  if (count == 0)
  {
    return -1;
  }
  return (int) candidates[scope_gen_rand (gen_p) % (uint32_t) count];
} /* scope_gen_pick_live */

static int
scope_gen_new (scope_generator_t *gen_p)
{
  int ticket;

  if (gen_p->depth == 0 || gen_p->identities >= SCOPE_PLAN_MAX_IDENTITIES)
  {
    return -1;
  }
  ticket = scope_gen_alloc_ticket (gen_p);
  if (ticket < 0 || scope_gen_records_left (gen_p) < 1)
  {
    return -1;
  }
  scope_gen_emit (gen_p, SCOPE_OP_NEW, (uint8_t) ticket, 0, 0);
  gen_p->ticket_owner[ticket] = (int16_t) gen_p->depth;
  gen_p->ticket_label[ticket] = SCOPE_LABEL_NORMAL;
  gen_p->ticket_identity[ticket] = gen_p->identities++;
  gen_p->scope_handles[gen_p->depth]++;
  if (gen_p->scope_handles[gen_p->depth] > gen_p->max_handles_in_scope)
  {
    gen_p->max_handles_in_scope = gen_p->scope_handles[gen_p->depth];
  }
  return ticket;
} /* scope_gen_new */

static void
scope_gen_copy (scope_generator_t *gen_p)
{
  int src;
  int dst;

  src = scope_gen_pick_live (gen_p, 0);
  dst = scope_gen_alloc_ticket (gen_p);
  if (src < 0 || dst < 0)
  {
    return;
  }
  scope_gen_emit (gen_p, SCOPE_OP_COPY, (uint8_t) src, (uint8_t) dst, 0);
  gen_p->ticket_owner[dst] = (int16_t) gen_p->depth;
  gen_p->ticket_label[dst] = gen_p->ticket_label[src];
  gen_p->ticket_identity[dst] = gen_p->ticket_identity[src];
  gen_p->scope_handles[gen_p->depth]++;
  if (gen_p->scope_handles[gen_p->depth] > gen_p->max_handles_in_scope)
  {
    gen_p->max_handles_in_scope = gen_p->scope_handles[gen_p->depth];
  }
} /* scope_gen_copy */

static void
scope_gen_wrap (scope_generator_t *gen_p)
{
  int src;
  int dst;
  uint8_t flags;

  src = scope_gen_pick_live (gen_p, 0);
  dst = scope_gen_alloc_ticket (gen_p);
  if (src < 0 || dst < 0)
  {
    return;
  }
  flags = (uint8_t) (scope_gen_rand (gen_p) % 4u);
  scope_gen_emit (gen_p, SCOPE_OP_WRAP, (uint8_t) src, (uint8_t) dst, flags);
  gen_p->ticket_owner[dst] = (int16_t) gen_p->depth;
  gen_p->ticket_label[dst] = (flags & 2) ? SCOPE_LABEL_ABORT : SCOPE_LABEL_EXCEPTION;
  gen_p->ticket_identity[dst] = gen_p->ticket_identity[src];
  gen_p->scope_handles[gen_p->depth]++;
  if (gen_p->scope_handles[gen_p->depth] > gen_p->max_handles_in_scope)
  {
    gen_p->max_handles_in_scope = gen_p->scope_handles[gen_p->depth];
  }
} /* scope_gen_wrap */

static void
scope_gen_extract (scope_generator_t *gen_p)
{
  int src;
  int dst;
  uint8_t flags;

  src = scope_gen_pick_live (gen_p, 1);
  dst = scope_gen_alloc_ticket (gen_p);
  if (src < 0 || dst < 0)
  {
    return;
  }
  flags = (uint8_t) (scope_gen_rand (gen_p) % 2u);
  scope_gen_emit (gen_p, SCOPE_OP_EXTRACT, (uint8_t) src, (uint8_t) dst, flags);
  gen_p->ticket_owner[dst] = (int16_t) gen_p->depth;
  gen_p->ticket_label[dst] = SCOPE_LABEL_NORMAL;
  gen_p->ticket_identity[dst] = gen_p->ticket_identity[src];
  gen_p->scope_handles[gen_p->depth]++;
  if (gen_p->scope_handles[gen_p->depth] > gen_p->max_handles_in_scope)
  {
    gen_p->max_handles_in_scope = gen_p->scope_handles[gen_p->depth];
  }
} /* scope_gen_extract */

static void
scope_gen_escape (scope_generator_t *gen_p)
{
  uint16_t ticket;

  if (gen_p->depth < 2 || gen_p->scope_escaped[gen_p->depth])
  {
    return;
  }
  for (ticket = 0; ticket < SCOPE_PLAN_TICKET_COUNT; ticket++)
  {
    if (gen_p->ticket_owner[ticket] == gen_p->depth)
    {
      scope_gen_emit (gen_p, SCOPE_OP_ESCAPE, ticket, 0, 0);
      gen_p->ticket_owner[ticket] = (int16_t) (gen_p->depth - 1);
      gen_p->scope_handles[gen_p->depth]--;
      gen_p->scope_escaped[gen_p->depth] = 1;
      return;
    }
  }
} /* scope_gen_escape */

static void
scope_gen_gc (scope_generator_t *gen_p)
{
  scope_gen_emit (gen_p, SCOPE_OP_GC, 0, 0, 0);
} /* scope_gen_gc */

static void
scope_gen_close_all (scope_generator_t *gen_p)
{
  while (gen_p->depth > 0)
  {
    scope_gen_close (gen_p);
  }
} /* scope_gen_close_all */

/*
 * Threshold plan: more than the 20-entry scope pool and more than the
 * 20-handle prelist of one scope, so the upstream dynamic storage paths are
 * exercised. An escape removes a handle from the dynamic chain, scopes are
 * reopened after closing, and GC steps bracket the transitions.
 */
static void
scope_gen_threshold_plan (scope_generator_t *gen_p)
{
  int level;
  int ticket;
  int dynamic_ticket = -1;

  for (level = 0; level < 23; level++)
  {
    scope_gen_open (gen_p);
  }
  /* 25 registrations in one scope: first 20 use the prelist, the rest spill
   * into the dynamically allocated handle chain. */
  for (ticket = 0; ticket < 25; ticket++)
  {
    int created = scope_gen_new (gen_p);
    if (ticket == 24)
    {
      dynamic_ticket = created;
    }
  }
  scope_gen_wrap (gen_p);
  scope_gen_copy (gen_p);
  if (dynamic_ticket >= 0)
  {
    scope_gen_emit (gen_p, SCOPE_OP_ESCAPE, (uint8_t) dynamic_ticket, 0, 0);
    gen_p->ticket_owner[dynamic_ticket] = (int16_t) (gen_p->depth - 1);
    gen_p->scope_handles[gen_p->depth]--;
    gen_p->scope_escaped[gen_p->depth] = 1;
  }
  scope_gen_gc (gen_p);
  for (level = 0; level < 6; level++)
  {
    scope_gen_close (gen_p);
  }
  scope_gen_gc (gen_p);
  scope_gen_close_all (gen_p);
  scope_gen_gc (gen_p);

  /* Reopen after everything was closed: the pool and prelists are reused. */
  scope_gen_open (gen_p);
  scope_gen_open (gen_p);
  scope_gen_new (gen_p);
  scope_gen_wrap (gen_p);
  scope_gen_escape (gen_p);
  scope_gen_close_all (gen_p);
  scope_gen_gc (gen_p);
} /* scope_gen_threshold_plan */

/* Wrapper-alias plan: every take/copy branch plus exception<->abort flips. */
static void
scope_gen_alias_plan (scope_generator_t *gen_p)
{
  int base;
  int wrapper;

  scope_gen_open (gen_p);
  base = scope_gen_new (gen_p);
  if (base < 0)
  {
    scope_gen_close_all (gen_p);
    return;
  }

  {
    uint8_t flags;
    for (flags = 0; flags < 4; flags++)
    {
      int dst = scope_gen_alloc_ticket (gen_p);
      if (dst < 0)
      {
        scope_gen_close_all (gen_p);
        return;
      }
      scope_gen_emit (gen_p, SCOPE_OP_WRAP, (uint8_t) base, (uint8_t) dst, flags);
      gen_p->ticket_owner[dst] = (int16_t) gen_p->depth;
      gen_p->ticket_label[dst] = (flags & 2) ? SCOPE_LABEL_ABORT : SCOPE_LABEL_EXCEPTION;
      gen_p->ticket_identity[dst] = gen_p->ticket_identity[base];
      gen_p->scope_handles[gen_p->depth]++;
      if (gen_p->scope_handles[gen_p->depth] > gen_p->max_handles_in_scope)
      {
        gen_p->max_handles_in_scope = gen_p->scope_handles[gen_p->depth];
      }
    }
  }

  wrapper = scope_gen_pick_live (gen_p, 1);
  if (wrapper < 0)
  {
    scope_gen_close_all (gen_p);
    return;
  }
  {
    /* Alias of a wrapper keeps identity and label. */
    int dst = scope_gen_alloc_ticket (gen_p);
    if (dst < 0)
    {
      scope_gen_close_all (gen_p);
      return;
    }
    scope_gen_emit (gen_p, SCOPE_OP_COPY, (uint8_t) wrapper, (uint8_t) dst, 0);
    gen_p->ticket_owner[dst] = (int16_t) gen_p->depth;
    gen_p->ticket_label[dst] = gen_p->ticket_label[wrapper];
    gen_p->ticket_identity[dst] = gen_p->ticket_identity[wrapper];
    gen_p->scope_handles[gen_p->depth]++;
    if (gen_p->scope_handles[gen_p->depth] > gen_p->max_handles_in_scope)
    {
      gen_p->max_handles_in_scope = gen_p->scope_handles[gen_p->depth];
    }
  }
  {
    /* Both extract branches on the wrapper. */
    uint8_t flags;
    for (flags = 0; flags < 2; flags++)
    {
      int dst = scope_gen_alloc_ticket (gen_p);
      if (dst < 0)
      {
        scope_gen_close_all (gen_p);
        return;
      }
      scope_gen_emit (gen_p, SCOPE_OP_EXTRACT, (uint8_t) wrapper, (uint8_t) dst, flags);
      gen_p->ticket_owner[dst] = (int16_t) gen_p->depth;
      gen_p->ticket_label[dst] = SCOPE_LABEL_NORMAL;
      gen_p->ticket_identity[dst] = gen_p->ticket_identity[wrapper];
      gen_p->scope_handles[gen_p->depth]++;
      if (gen_p->scope_handles[gen_p->depth] > gen_p->max_handles_in_scope)
      {
        gen_p->max_handles_in_scope = gen_p->scope_handles[gen_p->depth];
      }
    }
  }
  scope_gen_gc (gen_p);
  scope_gen_close_all (gen_p);
  scope_gen_gc (gen_p);
} /* scope_gen_alias_plan */

static void
scope_gen_build (scope_generator_t *gen_p, uint32_t seed)
{
  size_t iterations;
  size_t index;

  scope_gen_init (gen_p, seed);

  if (seed == 0)
  {
    /* Explicitly valid empty plan. */
    return;
  }
  if (seed == 1)
  {
    scope_gen_threshold_plan (gen_p);
    return;
  }
  if (seed == 2)
  {
    scope_gen_alias_plan (gen_p);
    return;
  }

  iterations = 24u + (scope_gen_rand (gen_p) % 96u);
  for (index = 0; index < iterations && scope_gen_records_left (gen_p) > 8; index++)
  {
    uint32_t choice = scope_gen_rand (gen_p) % 10u;
    switch (choice)
    {
      case 0:
      case 1:
        scope_gen_open (gen_p);
        break;
      case 2:
      case 3:
        scope_gen_new (gen_p);
        break;
      case 4:
        scope_gen_copy (gen_p);
        break;
      case 5:
        scope_gen_wrap (gen_p);
        break;
      case 6:
        scope_gen_extract (gen_p);
        break;
      case 7:
        scope_gen_escape (gen_p);
        break;
      case 8:
        scope_gen_close (gen_p);
        break;
      default:
        scope_gen_gc (gen_p);
        break;
    }
  }
  scope_gen_close_all (gen_p);
  scope_gen_gc (gen_p);
} /* scope_gen_build */

int
jerry_scope_fuzz_seed_drive (size_t *seed_count_p)
{
  size_t seed;
  size_t aggregate_takes = 0;
  size_t aggregate_copies = 0;
  size_t aggregate_escapes = 0;
  size_t max_opened = 0;
  size_t max_handles_in_scope = 0;

  for (seed = 0; seed < SCOPE_SEED_COUNT; seed++)
  {
    scope_generator_t gen;
    scope_model_t probe;
    struct jerry_scope_report report;
    int rc;

    scope_gen_build (&gen, (uint32_t) (seed + 1u));

    /* Every fixed seed must describe a whole valid plan. */
    if (scope_model_precheck (&probe, gen.bytes, gen.length) != 0)
    {
      scope_diff_print ("seed-precheck", &probe.diff);
      fprintf (stderr, "SCOPE_DIVERGENCE phase=seed reason=generated invalid plan seed=%zu\n", seed);
      return 2;
    }
    if (probe.max_depth > (int) max_opened)
    {
      max_opened = (size_t) probe.max_depth;
    }
    if (gen.max_handles_in_scope > max_handles_in_scope)
    {
      max_handles_in_scope = gen.max_handles_in_scope;
    }

    rc = jerry_scope_run (gen.bytes, gen.length, &report);
    if (rc != 0)
    {
      fprintf (stderr, "SCOPE_DIVERGENCE phase=seed reason=replay failed seed=%zu rc=%d\n",
               seed, rc);
      return rc;
    }
    aggregate_takes += report.takes;
    aggregate_copies += report.copies;
    aggregate_escapes += report.escapes;
  }

  /* The fixed set must really exercise both ownership branches, cross-level
   * escapes and the upstream dynamic storage thresholds (20-entry scope pool
   * and 20-handle per-scope prelist). */
  if (aggregate_takes == 0 || aggregate_copies == 0 || aggregate_escapes == 0
      || max_opened <= JERRYX_SCOPE_PRELIST_SIZE
      || max_handles_in_scope <= JERRYX_HANDLE_PRELIST_SIZE)
  {
    fprintf (stderr,
             "SCOPE_DIVERGENCE phase=seed reason=coverage gap takes=%zu copies=%zu "
             "escapes=%zu max_opened=%zu max_handles=%zu\n",
             aggregate_takes, aggregate_copies, aggregate_escapes, max_opened,
             max_handles_in_scope);
    return 2;
  }

  if (seed_count_p != NULL)
  {
    *seed_count_p = SCOPE_SEED_COUNT;
  }
  return 0;
} /* jerry_scope_fuzz_seed_drive */
