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
 * Pure logical model implementation. No JerryScript header is included and
 * no jerry_value_t is ever stored here: tickets, identities, labels and
 * scope ownership are the whole state.
 */
#include "tests/fuzz/scope_model.h"

#include <string.h>

#define SCOPE_IDENTITY_WORDS ((SCOPE_PLAN_MAX_IDENTITIES + 31) / 32)

void
scope_model_init (scope_model_t *model_p)
{
  size_t ticket;

  memset (model_p, 0, sizeof (*model_p));
  model_p->depth = 0;
  model_p->max_depth = 0;
  for (ticket = 0; ticket < SCOPE_PLAN_TICKET_COUNT; ticket++)
  {
    model_p->ticket_scope[ticket] = -1;
  }
} /* scope_model_init */

const char *
scope_label_name (scope_label_t label)
{
  switch (label)
  {
    case SCOPE_LABEL_NORMAL:
      return "normal";
    case SCOPE_LABEL_EXCEPTION:
      return "exception";
    case SCOPE_LABEL_ABORT:
      return "abort";
  }
  return "unknown";
} /* scope_label_name */

int
scope_model_ticket_alive (const scope_model_t *model_p, uint8_t ticket)
{
  return model_p->ticket_scope[ticket] >= 0;
} /* scope_model_ticket_alive */

static void
scope_model_set_mask (uint32_t *mask_p, uint8_t index)
{
  mask_p[index >> 5] |= (uint32_t) 1u << (index & 31);
} /* scope_model_set_mask */

static void
scope_model_fail (scope_model_t *model_p, const scope_record_t *record_p,
                  uint8_t ticket, uint8_t identity, scope_label_t label, const char *reason)
{
  scope_diff_t *diff_p = &model_p->diff;
  diff_p->valid = 1;
  diff_p->step = model_p->trace_count;
  diff_p->op = record_p->op;
  diff_p->depth = model_p->depth;
  diff_p->ticket = ticket;
  diff_p->ticket_scope = model_p->ticket_scope[ticket];
  diff_p->identity = identity;
  diff_p->label = label;
  diff_p->reason = reason;
} /* scope_model_fail */

/* Take a freshly created or derived ownership at the current scope. */
static int
scope_model_assign (scope_model_t *model_p, uint8_t ticket, uint8_t identity, scope_label_t label)
{
  if (model_p->ticket_scope[ticket] != -1)
  {
    return -1;
  }

  model_p->ticket_identity[ticket] = identity;
  model_p->ticket_label[ticket] = (uint8_t) label;
  model_p->ticket_scope[ticket] = (int8_t) model_p->depth;
  model_p->identity_refs[identity]++;
  if (model_p->identity_refs[identity] == 1)
  {
    scope_model_set_mask (model_p->alive_mask, identity);
  }
  return 0;
} /* scope_model_assign */

/* Decode and validate one raw four-byte record. */
static int
scope_model_decode (scope_model_t *model_p, const uint8_t *raw_p, scope_record_t *record_p)
{
  record_p->op = raw_p[0];
  record_p->a = raw_p[1];
  record_p->b = raw_p[2];
  record_p->flags = raw_p[3];

  if (record_p->op >= SCOPE_OP__COUNT)
  {
    scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL, "unknown op");
    return -1;
  }

  switch (record_p->op)
  {
    case SCOPE_OP_OPEN:
    case SCOPE_OP_CLOSE:
    case SCOPE_OP_GC:
      if (record_p->a != 0 || record_p->b != 0 || record_p->flags != 0)
      {
        scope_model_fail (model_p, record_p, 0, 0, SCOPE_LABEL_NORMAL, "unused fields must be zero");
        return -1;
      }
      break;
    case SCOPE_OP_NEW:
    case SCOPE_OP_ESCAPE:
      if (record_p->b != 0 || record_p->flags != 0)
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL,
                          "unused fields must be zero");
        return -1;
      }
      break;
    case SCOPE_OP_COPY:
      if (record_p->flags != 0)
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL,
                          "unused flags must be zero");
        return -1;
      }
      break;
    case SCOPE_OP_WRAP:
      if (record_p->flags > SCOPE_BRANCH_ABORT_TAKE)
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL, "bad wrap flags");
        return -1;
      }
      break;
    case SCOPE_OP_EXTRACT:
      if (record_p->flags > SCOPE_BRANCH_EXCEPTION_TAKE)
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL, "bad extract flags");
        return -1;
      }
      break;
  }
  return 0;
} /* scope_model_decode */

int
scope_model_apply (scope_model_t *model_p, const scope_record_t *record_p)
{
  model_p->last_escape = SCOPE_ESCAPE_NONE;
  switch (record_p->op)
  {
    case SCOPE_OP_OPEN:
      if (model_p->depth >= SCOPE_PLAN_MAX_DEPTH)
      {
        scope_model_fail (model_p, record_p, 0, 0, SCOPE_LABEL_NORMAL, "scope depth exceeds 64");
        return -1;
      }
      model_p->depth++;
      if (model_p->depth > model_p->max_depth)
      {
        model_p->max_depth = model_p->depth;
      }
      model_p->scope_escaped[model_p->depth] = 0;
      return 0;

    case SCOPE_OP_NEW:
      if (model_p->depth == 0)
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL,
                          "NEW requires a non-root scope");
        return -1;
      }
      if (model_p->identity_created >= SCOPE_PLAN_MAX_IDENTITIES)
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL,
                          "identity limit of 128 exceeded");
        return -1;
      }
      if (model_p->ticket_scope[record_p->a] != -1)
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL,
                          "destination ticket still owned");
        return -1;
      }
      uint8_t identity;
      identity = model_p->identity_created++;
      if (scope_model_assign (model_p, record_p->a, identity, SCOPE_LABEL_NORMAL))
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL,
                          "destination ticket still owned");
        return -1;
      }
      return 0;

    case SCOPE_OP_COPY:
    {
      if (!scope_model_ticket_alive (model_p, record_p->a))
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL,
                          "source ticket has no live scope ownership");
        return -1;
      }
      identity = model_p->ticket_identity[record_p->a];
      scope_label_t label = (scope_label_t) model_p->ticket_label[record_p->a];
      if (scope_model_assign (model_p, record_p->b, identity, label))
      {
        scope_model_fail (model_p, record_p, record_p->b, identity, label,
                          "destination ticket still owned");
        return -1;
      }
      return 0;
    }

    case SCOPE_OP_WRAP:
    {
      if (!scope_model_ticket_alive (model_p, record_p->a))
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL,
                          "source ticket has no live scope ownership");
        return -1;
      }
      identity = model_p->ticket_identity[record_p->a];
      scope_label_t label = (record_p->flags & 2) ? SCOPE_LABEL_ABORT : SCOPE_LABEL_EXCEPTION;
      if (scope_model_assign (model_p, record_p->b, identity, label))
      {
        scope_model_fail (model_p, record_p, record_p->b, identity, label,
                          "destination ticket still owned");
        return -1;
      }
      return 0;
    }

    case SCOPE_OP_EXTRACT:
    {
      if (!scope_model_ticket_alive (model_p, record_p->a))
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL,
                          "source ticket has no live scope ownership");
        return -1;
      }
      scope_label_t source_label = (scope_label_t) model_p->ticket_label[record_p->a];
      if (source_label == SCOPE_LABEL_NORMAL)
      {
        scope_model_fail (model_p, record_p, record_p->a,
                          model_p->ticket_identity[record_p->a], source_label,
                          "EXTRACT requires an exception/abort wrapper");
        return -1;
      }
      identity = model_p->ticket_identity[record_p->a];
      if (scope_model_assign (model_p, record_p->b, identity, SCOPE_LABEL_NORMAL))
      {
        scope_model_fail (model_p, record_p, record_p->b, identity, SCOPE_LABEL_NORMAL,
                          "destination ticket still owned");
        return -1;
      }
      return 0;
    }

    case SCOPE_OP_ESCAPE:
    {
      if (model_p->depth < 2)
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL,
                          "ESCAPE requires a non-outermost scope");
        return -1;
      }
      if (!scope_model_ticket_alive (model_p, record_p->a)
          || model_p->ticket_scope[record_p->a] != model_p->depth)
      {
        scope_model_fail (model_p, record_p, record_p->a, 0, SCOPE_LABEL_NORMAL,
                          "escapee must belong to the current scope");
        return -1;
      }
      if (model_p->scope_escaped[model_p->depth])
      {
        /* Public contract: one successful escape per scope, later calls fail
         * and leave every ownership unchanged. */
        model_p->last_escape = SCOPE_ESCAPE_TWICE;
        return 0;
      }
      model_p->ticket_scope[record_p->a] = (int8_t) (model_p->depth - 1);
      model_p->scope_escaped[model_p->depth] = 1;
      model_p->last_escape = SCOPE_ESCAPE_OK;
      return 0;
    }

    case SCOPE_OP_CLOSE:
    {
      if (model_p->depth == 0)
      {
        scope_model_fail (model_p, record_p, 0, 0, SCOPE_LABEL_NORMAL, "CLOSE at root scope");
        return -1;
      }
      size_t ticket;
      for (ticket = 0; ticket < SCOPE_PLAN_TICKET_COUNT; ticket++)
      {
        if (model_p->ticket_scope[ticket] != model_p->depth)
        {
          continue;
        }
        identity = model_p->ticket_identity[ticket];
        model_p->ticket_scope[ticket] = -1;
        model_p->identity_refs[identity]--;
        if (model_p->identity_refs[identity] == 0)
        {
          model_p->alive_mask[identity >> 5] &= ~((uint32_t) 1u << (identity & 31));
        }
      }
      model_p->depth--;
      return 0;
    }

    case SCOPE_OP_GC:
      model_p->gc_events[model_p->gc_event_count++] = (uint16_t) model_p->trace_count;
      return 0;
  }

  scope_model_fail (model_p, record_p, 0, 0, SCOPE_LABEL_NORMAL, "unhandled op");
  return -1;
} /* scope_model_apply */

int
scope_model_precheck (scope_model_t *model_p, const uint8_t *data, size_t size)
{
  size_t raw_count;
  size_t index;

  scope_model_init (model_p);

  if (size % 4u != 0u)
  {
    scope_diff_t *diff_p = &model_p->diff;
    diff_p->valid = 1;
    diff_p->step = size / 4u;
    diff_p->op = 0;
    diff_p->depth = 0;
    diff_p->ticket = 0;
    diff_p->ticket_scope = -1;
    diff_p->identity = 0;
    diff_p->label = SCOPE_LABEL_NORMAL;
    diff_p->reason = "truncated plan: size is not a multiple of four";
    return 1;
  }

  raw_count = size / 4u;
  if (raw_count > SCOPE_PLAN_MAX_STEPS)
  {
    model_p->diff.valid = 1;
    model_p->diff.step = raw_count;
    model_p->diff.reason = "plan exceeds 4096 records";
    return 1;
  }
  model_p->step_count = raw_count;

  for (index = 0; index < raw_count; index++)
  {
    scope_record_t record;
    if (scope_model_decode (model_p, data + index * 4u, &record))
    {
      model_p->diff.step = index;
      return 1;
    }
    model_p->steps[index] = record;
    if (scope_model_apply (model_p, &record))
    {
      model_p->diff.step = index;
      return 1;
    }
  }

  if (model_p->depth != 0)
  {
    model_p->diff.valid = 1;
    model_p->diff.step = raw_count;
    model_p->diff.depth = model_p->depth;
    model_p->diff.reason = "unbalanced plan: explicit scopes remain open";
    return 1;
  }

  /* Re-initialize so callers start a real replay from a clean model, then
   * store the decoded records for the replay to consume. The validated
   * maximum depth is plan metadata and must survive the re-initialization. */
  {
    int validated_max_depth = model_p->max_depth;

    scope_model_init (model_p);
    model_p->max_depth = validated_max_depth;
    model_p->step_count = raw_count;
  }
  for (index = 0; index < raw_count; index++)
  {
    model_p->steps[index].op = data[index * 4u];
    model_p->steps[index].a = data[index * 4u + 1u];
    model_p->steps[index].b = data[index * 4u + 2u];
    model_p->steps[index].flags = data[index * 4u + 3u];
  }
  return 0;
} /* scope_model_precheck */

void
scope_model_note_finalized (scope_model_t *model_p, uint8_t identity)
{
  if (identity >= SCOPE_PLAN_MAX_IDENTITIES)
  {
    return;
  }
  model_p->identity_final_count[identity]++;
  scope_model_set_mask (model_p->finalized_mask, identity);
  model_p->alive_mask[identity >> 5] &= ~((uint32_t) 1u << (identity & 31));
} /* scope_model_note_finalized */

int
scope_model_check_gc (scope_model_t *model_p, size_t step)
{
  uint8_t identity;

  for (identity = 0; identity < model_p->identity_created; identity++)
  {
    uint8_t refs = model_p->identity_refs[identity];
    uint8_t finals = model_p->identity_final_count[identity];
    if (refs == 0)
    {
      if (finals != 1)
      {
        model_p->diff.valid = 1;
        model_p->diff.step = step;
        model_p->diff.depth = model_p->depth;
        model_p->diff.identity = identity;
        model_p->diff.reason = "zero-responsibility object not finalized exactly once at GC";
        return -1;
      }
    }
    else if (finals != 0)
    {
      model_p->diff.valid = 1;
      model_p->diff.step = step;
      model_p->diff.depth = model_p->depth;
      model_p->diff.identity = identity;
      model_p->diff.reason = "live object finalized while still owned";
      return -1;
    }
  }
  return 0;
} /* scope_model_check_gc */

int
scope_model_check_finish (scope_model_t *model_p)
{
  uint8_t identity;

  if (model_p->depth != 0)
  {
    model_p->diff.valid = 1;
    model_p->diff.reason = "finish check with open scopes";
    return -1;
  }
  for (identity = 0; identity < model_p->identity_created; identity++)
  {
    if (model_p->identity_final_count[identity] != 1)
    {
      model_p->diff.valid = 1;
      model_p->diff.identity = identity;
      model_p->diff.reason = "object finalized other than exactly once by plan end";
      return -1;
    }
  }
  return 0;
} /* scope_model_check_finish */

void
scope_model_snapshot (scope_model_t *model_p, const scope_record_t *record_p,
                      scope_escape_result_t escape)
{
  scope_step_state_t *snapshot_p;
  size_t ticket;
  size_t word;

  if (model_p->trace_count >= SCOPE_PLAN_MAX_STEPS)
  {
    return;
  }
  snapshot_p = &model_p->trace[model_p->trace_count++];
  memset (snapshot_p, 0, sizeof (*snapshot_p));
  snapshot_p->step = model_p->trace_count - 1;
  snapshot_p->op = record_p->op;
  snapshot_p->depth = model_p->depth;
  snapshot_p->escape = escape;

  for (ticket = 0; ticket < SCOPE_PLAN_TICKET_COUNT; ticket++)
  {
    if (model_p->ticket_scope[ticket] >= 0)
    {
      snapshot_p->live_owners++;
    }
  }

  for (word = 0; word < SCOPE_IDENTITY_WORDS; word++)
  {
    snapshot_p->alive_mask[word] = model_p->alive_mask[word];
    snapshot_p->finalized_mask[word] = model_p->finalized_mask[word];
    snapshot_p->final_count_mask[word] = 0;
  }
  for (ticket = 0; ticket < model_p->identity_created; ticket++)
  {
    if (model_p->identity_final_count[ticket] == 1)
    {
      scope_model_set_mask (snapshot_p->final_count_mask, (uint8_t) ticket);
    }
  }
} /* scope_model_snapshot */

int
scope_model_compare (const scope_model_t *first_p, const scope_model_t *second_p, scope_diff_t *out_p)
{
  size_t step;
  size_t word;

  memset (out_p, 0, sizeof (*out_p));

  if (first_p->trace_count != second_p->trace_count
      || first_p->identity_created != second_p->identity_created
      || first_p->gc_event_count != second_p->gc_event_count)
  {
    const scope_model_t *longer_p =
      first_p->trace_count >= second_p->trace_count ? first_p : second_p;
    size_t at = first_p->trace_count;

    out_p->valid = 1;
    out_p->reason = "replay trace shape differs";
    out_p->step = at;
    if (at < longer_p->trace_count)
    {
      out_p->op = longer_p->trace[at].op;
      out_p->depth = longer_p->trace[at].depth;
    }
    return -1;
  }

  for (step = 0; step < first_p->trace_count; step++)
  {
    const scope_step_state_t *left_p = &first_p->trace[step];
    const scope_step_state_t *right_p = &second_p->trace[step];

    if (left_p->op != right_p->op || left_p->depth != right_p->depth
        || left_p->escape != right_p->escape || left_p->live_owners != right_p->live_owners)
    {
      out_p->valid = 1;
      out_p->step = step;
      out_p->op = left_p->op;
      out_p->depth = left_p->depth;
      out_p->reason = "ownership responsibility differs between replays";
      return -1;
    }
    for (word = 0; word < SCOPE_IDENTITY_WORDS; word++)
    {
      if (left_p->alive_mask[word] != right_p->alive_mask[word]
          || left_p->finalized_mask[word] != right_p->finalized_mask[word]
          || left_p->final_count_mask[word] != right_p->final_count_mask[word])
      {
        out_p->valid = 1;
        out_p->step = step;
        out_p->op = left_p->op;
        out_p->depth = left_p->depth;
        out_p->reason = "alive/finalized identity set differs between replays";
        return -1;
      }
    }
  }

  for (step = 0; step < first_p->gc_event_count; step++)
  {
    if (first_p->gc_events[step] != second_p->gc_events[step])
    {
      out_p->valid = 1;
      out_p->step = step;
      out_p->reason = "GC event position differs between replays";
      return -1;
    }
  }
  return 0;
} /* scope_model_compare */
