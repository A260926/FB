/*
 * Reassembly fuzzing support.
 *
 * Drives the real ngtcp2 stream/ROB reassembly path with locally
 * generated legal bodies and offsets, injects single recoverable
 * allocation failures, and checks the production objects against an
 * independent byte-presence model after every operation.  A second
 * control world runs the same schedule without any refusal so that
 * stressed and control states can be compared at every boundary.
 */
#include "reassembly.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ngtcp2_strm.h"
#include "ngtcp2_rob.h"
#include "ngtcp2_ksl.h"
#include "ngtcp2_objalloc.h"
#include "ngtcp2_err.h"

#define RXA_HEADER "RXA1\n"
#define RXA_HEADER_LEN 5
#define RXA_MAX_RECORDS 128
#define RXA_SLOTS 4

/*
 * rxa_op is the decoded record op field.
 */
typedef enum rxa_op {
  RXA_OP_OPEN = 0,
  RXA_OP_FEED = 1,
  RXA_OP_DRAIN = 2,
  RXA_OP_CANCEL = 3,
  RXA_OP_FAILNEXT = 4
} rxa_op;

typedef struct rxa_rec {
  uint32_t a, b, c;
  uint8_t op;
} rxa_rec;

/*
 * tracked_item is one outstanding allocation handed to the library.
 */
typedef struct tracked_item {
  void *ptr;
  struct tracked_item *next;
} tracked_item;

/*
 * tracked_mem is a ledger allocator.  When fail_index is positive,
 * the fail_index-th malloc/calloc/realloc of an armed window is
 * refused; the window is disarmed as soon as the refusal happens.
 */
typedef struct tracked_mem {
  ngtcp2_mem mem;
  tracked_item *head;
  size_t live;
  size_t total;
  /* armed is nonzero while a FEED window may refuse an allocation. */
  int armed;
  /* fail_index is the 1-based allocation ordinal to refuse. */
  size_t fail_index;
  /* window_count counts allocations inside the armed window. */
  size_t window_count;
} tracked_mem;

/*
 * model_slot is the independent expectation for one stream, built as
 * a byte presence set, an expected body and a consumption front.  It
 * never copies the production gap tree.
 */
typedef struct model_slot {
  int alive;
  uint64_t bodylen;
  uint64_t consumed;
  /* present is a bodylen-sized byte presence set. */
  uint8_t *present;
  /* body is the expected stream content. */
  uint8_t *body;
} model_slot;

/*
 * prod_slot is the production stream state for one slot.
 */
typedef struct prod_slot {
  int alive;
  ngtcp2_strm strm;
} prod_slot;

typedef struct rxa_world {
  tracked_mem tmem;
  ngtcp2_objalloc frames;
  model_slot model[RXA_SLOTS];
  prod_slot prod[RXA_SLOTS];
  /* pending fail parameters for the next valid FEED; -1 if unset. */
  int pending_fail;
  /* injections_enabled is zero in the control world. */
  int injections_enabled;
  rxcheck_result result;
  FILE *trace;
  /* diff records the first model difference, if any. */
  char diff[160];
  int mismatch;
} rxa_world;

/* ------------------------------------------------------------------ */
/* Tracking allocator                                                 */
/* ------------------------------------------------------------------ */

static void tracked_free(void *ptr, void *user_data) {
  tracked_mem *tmem = user_data;
  tracked_item **q;

  if (ptr == NULL) {
    return;
  }

  for (q = &tmem->head; *q != NULL; q = &(*q)->next) {
    if ((*q)->ptr == ptr) {
      tracked_item *item = *q;

      *q = item->next;
      free(item);
      --tmem->live;
      free(ptr);
      return;
    }
  }

  /* Every freed pointer must have been handed out by this
     allocator. */
  abort();
}

static void *tracked_malloc(size_t size, void *user_data) {
  tracked_mem *tmem = user_data;
  tracked_item *item;
  void *ptr;

  if (tmem->armed) {
    ++tmem->window_count;
    if (tmem->window_count == tmem->fail_index) {
      /* The refusal is a single event for this FEED window. */
      tmem->armed = 0;
      return NULL;
    }
  }

  ptr = malloc(size);
  if (ptr == NULL) {
    return NULL;
  }

  item = malloc(sizeof(*item));
  if (item == NULL) {
    free(ptr);
    return NULL;
  }

  item->ptr = ptr;
  item->next = tmem->head;
  tmem->head = item;
  ++tmem->live;
  ++tmem->total;

  return ptr;
}

static void *tracked_calloc(size_t nmemb, size_t size, void *user_data) {
  void *ptr = tracked_malloc(nmemb * size, user_data);

  if (ptr != NULL) {
    memset(ptr, 0, nmemb * size);
  }

  return ptr;
}

static void *tracked_realloc(void *ptr, size_t size, void *user_data) {
  tracked_mem *tmem = user_data;
  tracked_item *item;
  void *nptr;

  if (ptr == NULL) {
    return tracked_malloc(size, user_data);
  }

  if (size == 0) {
    tracked_free(ptr, user_data);
    return NULL;
  }

  if (tmem->armed) {
    ++tmem->window_count;
    if (tmem->window_count == tmem->fail_index) {
      tmem->armed = 0;
      return NULL;
    }
  }

  nptr = realloc(ptr, size);
  if (nptr == NULL) {
    return NULL;
  }

  for (item = tmem->head; item != NULL; item = item->next) {
    if (item->ptr == ptr) {
      item->ptr = nptr;
      return nptr;
    }
  }

  abort();
}

static void tracked_mem_init(tracked_mem *tmem) {
  tmem->mem.user_data = tmem;
  tmem->mem.malloc = tracked_malloc;
  tmem->mem.free = tracked_free;
  tmem->mem.calloc = tracked_calloc;
  tmem->mem.realloc = tracked_realloc;
  tmem->head = NULL;
  tmem->live = 0;
  tmem->total = 0;
  tmem->armed = 0;
  tmem->fail_index = 0;
  tmem->window_count = 0;
}

/* ------------------------------------------------------------------ */
/* Independent model                                                  */
/* ------------------------------------------------------------------ */

static uint8_t model_byte(uint64_t i, uint32_t c) {
  return (uint8_t)('a' + (i + c) % 26);
}

static uint64_t model_front(const model_slot *slot) {
  uint64_t i;

  for (i = slot->consumed; i < slot->bodylen; ++i) {
    if (!slot->present[i]) {
      return i;
    }
  }

  return slot->bodylen;
}

/*
 * model_feed records [offset, offset+len) in the presence set and
 * returns the number of newly present bytes.
 */
static uint64_t model_feed(model_slot *slot, uint64_t offset, uint64_t len) {
  uint64_t i, nnew = 0;

  for (i = 0; i < len; ++i) {
    if (!slot->present[offset + i]) {
      slot->present[offset + i] = 1;
      ++nnew;
    }
  }

  return nnew;
}

/*
 * model_drain marks the next up to want contiguous bytes consumed and
 * returns the number of bytes the production side must deliver.
 */
static uint64_t model_drain(model_slot *slot, uint64_t want) {
  uint64_t front = model_front(slot);
  uint64_t avail = front - slot->consumed;
  uint64_t take = want < avail ? want : avail;

  slot->consumed += take;

  return take;
}

/* ------------------------------------------------------------------ */
/* World lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void world_init(rxa_world *world, int injections_enabled,
                       FILE *trace) {
  memset(world, 0, sizeof(*world));
  tracked_mem_init(&world->tmem);
  ngtcp2_objalloc_init(&world->frames, 1024, &world->tmem.mem);
  world->pending_fail = -1;
  world->injections_enabled = injections_enabled;
  world->trace = trace;
}

static void prod_destroy(rxa_world *world, size_t slot) {
  prod_slot *ps = &world->prod[slot];

  if (!ps->alive) {
    return;
  }

  ngtcp2_strm_free(&ps->strm);
  ps->alive = 0;
}

static void model_destroy(rxa_world *world, size_t slot) {
  model_slot *ms = &world->model[slot];

  free(ms->present);
  free(ms->body);
  memset(ms, 0, sizeof(*ms));
}

static void world_destroy(rxa_world *world) {
  size_t i;

  for (i = 0; i < RXA_SLOTS; ++i) {
    prod_destroy(world, i);
    model_destroy(world, i);
  }

  ngtcp2_objalloc_free(&world->frames);

  /* Every production allocation must have been returned. */
  if (world->tmem.live != 0 || world->tmem.head != NULL) {
    abort();
  }
}

static int prod_open(rxa_world *world, size_t slot, uint64_t bodylen) {
  prod_slot *ps = &world->prod[slot];

  ngtcp2_strm_init(&ps->strm, (int64_t)slot, NGTCP2_STRM_FLAG_NONE, 1u << 30,
                   1u << 30, NULL, &world->frames, &world->tmem.mem);
  ps->strm.rx.last_offset = bodylen;
  ps->alive = 1;

  return 0;
}

static int model_open(rxa_world *world, size_t slot, uint64_t bodylen,
                      uint32_t c) {
  model_slot *ms = &world->model[slot];
  uint64_t i;

  ms->present = calloc((size_t)bodylen, 1);
  ms->body = malloc((size_t)bodylen);
  if (ms->present == NULL || ms->body == NULL) {
    free(ms->present);
    free(ms->body);
    ms->present = NULL;
    ms->body = NULL;
    return NGTCP2_ERR_NOMEM;
  }

  for (i = 0; i < bodylen; ++i) {
    ms->body[i] = model_byte(i, c);
  }

  ms->alive = 1;
  ms->bodylen = bodylen;
  ms->consumed = 0;

  return 0;
}

/* ------------------------------------------------------------------ */
/* Verification                                                       */
/* ------------------------------------------------------------------ */

/*
 * verify_slot compares the production stream against the independent
 * model: the contiguous front, the first gap, the delivered prefix
 * and the readable body.
 */
static int verify_slot(rxa_world *world, size_t slot, const char *stage) {
  model_slot *ms = &world->model[slot];
  prod_slot *ps = &world->prod[slot];
  uint64_t front, prod_front, prod_gap, off;
  const uint8_t *p;
  size_t n;

  if (!ms->alive) {
    if (ps->alive) {
      snprintf(world->diff, sizeof(world->diff),
               "slot=%zu stage=%s model released but stream alive", slot,
               stage);
      world->mismatch = 1;
      return 1;
    }

    return 0;
  }

  front = model_front(ms);

  prod_front = ngtcp2_strm_rx_offset(&ps->strm);
  if (prod_front != front) {
    snprintf(world->diff, sizeof(world->diff),
             "slot=%zu stage=%s front model=%llu prod=%llu", slot, stage,
             (unsigned long long)front, (unsigned long long)prod_front);
    world->mismatch = 1;
    return 1;
  }

  prod_gap = ps->strm.rx.rob == NULL
               ? ps->strm.rx.cont_offset
               : ngtcp2_rob_first_gap_offset(ps->strm.rx.rob);
  if (prod_gap != front) {
    snprintf(world->diff, sizeof(world->diff),
             "slot=%zu stage=%s gap model=%llu prod=%llu", slot, stage,
             (unsigned long long)front, (unsigned long long)prod_gap);
    world->mismatch = 1;
    return 1;
  }

  /* Walk the contiguous readable region through the production view
     and compare every byte. */
  for (off = ms->consumed; off < front;) {
    n = ngtcp2_rob_data_at(ps->strm.rx.rob, &p, off);
    if (n == 0) {
      snprintf(world->diff, sizeof(world->diff),
               "slot=%zu stage=%s no readable data at %llu", slot, stage,
               (unsigned long long)off);
      world->mismatch = 1;
      return 1;
    }

    if (off + n > front) {
      snprintf(world->diff, sizeof(world->diff),
               "slot=%zu stage=%s readable run crosses front", slot, stage);
      world->mismatch = 1;
      return 1;
    }

    if (memcmp(p, ms->body + off, n) != 0) {
      snprintf(world->diff, sizeof(world->diff),
               "slot=%zu stage=%s body mismatch at %llu", slot, stage,
               (unsigned long long)off);
      world->mismatch = 1;
      return 1;
    }

    off += n;
  }

  return 0;
}

static int verify_all(rxa_world *world, const char *stage) {
  size_t i;

  for (i = 0; i < RXA_SLOTS; ++i) {
    if (verify_slot(world, i, stage) != 0) {
      return 1;
    }
  }

  return 0;
}

/*
 * compare_worlds checks that the stressed world and the refusal-free
 * control world hold the same logical state at an operation boundary.
 * oom_retries is intentionally excluded because it records the
 * injection itself rather than observable reassembly state.
 */
static int compare_worlds(rxa_world *stress, rxa_world *control,
                          uint64_t step) {
  size_t i;

  for (i = 0; i < RXA_SLOTS; ++i) {
    model_slot *a = &stress->model[i];
    model_slot *b = &control->model[i];

    if (a->alive != b->alive) {
      snprintf(stress->diff, sizeof(stress->diff),
               "step=%llu slot=%zu liveness stress=%d control=%d",
               (unsigned long long)step, i, a->alive, b->alive);
      stress->mismatch = 1;
      return 1;
    }

    if (!a->alive) {
      continue;
    }

    if (a->consumed != b->consumed || model_front(a) != model_front(b) ||
        ngtcp2_strm_rx_offset(&stress->prod[i].strm) !=
          ngtcp2_strm_rx_offset(&control->prod[i].strm)) {
      snprintf(stress->diff, sizeof(stress->diff),
               "step=%llu slot=%zu front stress=(%llu,%llu) "
               "control=(%llu,%llu)",
               (unsigned long long)step, i,
               (unsigned long long)a->consumed,
               (unsigned long long)model_front(a),
               (unsigned long long)b->consumed,
               (unsigned long long)model_front(b));
      stress->mismatch = 1;
      return 1;
    }
  }

  if (stress->result.accepted_bytes != control->result.accepted_bytes ||
      stress->result.delivered_bytes != control->result.delivered_bytes ||
      stress->result.canceled != control->result.canceled ||
      stress->result.operations != control->result.operations) {
    snprintf(stress->diff, sizeof(stress->diff),
             "step=%llu totals diverged from control",
             (unsigned long long)step);
    stress->mismatch = 1;
    return 1;
  }

  return 0;
}

/* ------------------------------------------------------------------ */
/* Trace                                                              */
/* ------------------------------------------------------------------ */

static void trace_line(rxa_world *world, uint64_t step, uint8_t op,
                       size_t slot, const char *verdict, const char *note) {
  static const char opname[][16] = {"OPEN", "FEED", "DRAIN", "CANCEL",
                                    "FAILNEXT"};

  if (world->trace == NULL) {
    return;
  }

  if (note != NULL) {
    fprintf(world->trace, "step=%llu op=%s slot=%zu %s %s\n",
            (unsigned long long)step, opname[op], slot, verdict, note);
  } else {
    fprintf(world->trace, "step=%llu op=%s slot=%zu %s\n",
            (unsigned long long)step, opname[op], slot, verdict);
  }
}

/* ------------------------------------------------------------------ */
/* Recorded operations                                                */
/* ------------------------------------------------------------------ */

static int do_open(rxa_world *world, const rxa_rec *rec, uint64_t step) {
  size_t slot = rec->a % RXA_SLOTS;
  uint64_t bodylen = 1 + 257 * rec->b;

  if (world->model[slot].alive) {
    trace_line(world, step, rec->op, slot, "SKIP", "slot-open");
    return 0;
  }

  if (model_open(world, slot, bodylen, rec->c) != 0) {
    return NGTCP2_ERR_NOMEM;
  }

  if (prod_open(world, slot, bodylen) != 0) {
    model_destroy(world, slot);
    return NGTCP2_ERR_NOMEM;
  }

  trace_line(world, step, rec->op, slot, "EXEC", NULL);

  return verify_all(world, "open");
}

/*
 * feed_once performs one production FEED together with the matching
 * model update.  The model presence snapshot lets a failed call be
 * reverted.  The allocator refusal window is armed by the caller.
 */
static int feed_once(rxa_world *world, size_t slot, uint64_t offset,
                     uint64_t len, const uint8_t *snapshot,
                     uint64_t *accepted) {
  model_slot *ms = &world->model[slot];
  prod_slot *ps = &world->prod[slot];
  ngtcp2_ssize rv;
  uint64_t nnew;

  nnew = model_feed(ms, offset, len);

  rv = ngtcp2_strm_recv_reordering_atomic(
    &ps->strm, ms->body + offset, (size_t)len, offset);

  if (rv < 0) {
    /* Revert the model update so a retry starts from the same
       logical state. */
    memcpy(ms->present + offset, snapshot, (size_t)len);
    return (int)rv;
  }

  if ((uint64_t)rv != nnew) {
    snprintf(world->diff, sizeof(world->diff),
             "slot=%zu accepted model=%llu prod=%zd", slot,
             (unsigned long long)nnew, rv);
    world->mismatch = 1;
    return 1;
  }

  *accepted = nnew;

  return 0;
}

static int do_feed(rxa_world *world, const rxa_rec *rec, uint64_t step) {
  size_t slot = rec->a % RXA_SLOTS;
  model_slot *ms;
  uint64_t bodylen, offset, len;
  uint64_t accepted = 0;
  uint8_t *snapshot;
  int rv;
  int injected = 0;
  char note[96];

  if (!world->model[slot].alive) {
    trace_line(world, step, rec->op, slot, "SKIP", "slot-dead");
    return 0;
  }

  ms = &world->model[slot];
  bodylen = ms->bodylen;
  offset = (rec->b + 256 * (rec->a / 4)) % bodylen;
  len = (uint64_t)(1 + rec->c % 32);
  if (len > bodylen - offset) {
    len = bodylen - offset;
  }

  snapshot = malloc((size_t)len);
  if (snapshot == NULL) {
    return NGTCP2_ERR_NOMEM;
  }
  memcpy(snapshot, ms->present + offset, (size_t)len);

  if (world->injections_enabled && world->pending_fail >= 0) {
    world->tmem.armed = 1;
    world->tmem.fail_index = (size_t)(1 + world->pending_fail);
    world->tmem.window_count = 0;
  }

  rv = feed_once(world, slot, offset, len, snapshot, &accepted);

  if (rv == NGTCP2_ERR_NOMEM) {
    if (world->pending_fail < 0 || !world->injections_enabled) {
      /* An unarmed allocator only fails on real exhaustion. */
      free(snapshot);
      snprintf(world->diff, sizeof(world->diff),
               "slot=%zu unexpected NOMEM without injection", slot);
      world->mismatch = 1;
      return 1;
    }

    /* The refusal happened (arming was cleared inside the allocator).
       Retry the same live object with injections disabled; no
       destruction or reconstruction is involved. */
    injected = 1;
    world->tmem.armed = 0;

    rv = feed_once(world, slot, offset, len, snapshot, &accepted);
    if (rv != 0) {
      free(snapshot);
      return rv < 0 ? rv : 1;
    }

    ++world->result.oom_retries;
  } else if (rv != 0) {
    free(snapshot);
    return rv < 0 ? rv : 1;
  }

  if (world->tmem.armed) {
    /* The configured ordinal never occurred: the call succeeded
       normally, so no retry is counted. */
    world->tmem.armed = 0;
  }

  world->pending_fail = -1;
  free(snapshot);

  world->result.accepted_bytes += accepted;

  if (injected) {
    snprintf(note, sizeof(note), "oom-retry accepted=%llu",
             (unsigned long long)accepted);
  } else {
    snprintf(note, sizeof(note), "accepted=%llu",
             (unsigned long long)accepted);
  }
  trace_line(world, step, rec->op, slot, "EXEC", note);

  return verify_all(world, "feed");
}

static int do_drain(rxa_world *world, const rxa_rec *rec, uint64_t step) {
  size_t slot = rec->a % RXA_SLOTS;
  model_slot *ms;
  prod_slot *ps;
  uint64_t want, expect, delivered = 0;
  uint64_t start_consumed;
  char note[96];

  if (!world->model[slot].alive) {
    trace_line(world, step, rec->op, slot, "SKIP", "slot-dead");
    return 0;
  }

  ms = &world->model[slot];
  ps = &world->prod[slot];
  want = 1 + rec->b;

  expect = model_drain(ms, want);
  start_consumed = ms->consumed - expect;

  /* Drive the production consumption path and compare the content of
     every readable run. */
  while (delivered < expect) {
    const uint8_t *p = NULL;
    uint64_t off = start_consumed + delivered;
    size_t remain = (size_t)(expect - delivered);
    size_t n = ngtcp2_rob_data_at(ps->strm.rx.rob, &p, off);

    if (n == 0) {
      snprintf(world->diff, sizeof(world->diff),
               "slot=%zu drain stalled at %llu", slot,
               (unsigned long long)off);
      world->mismatch = 1;
      return 1;
    }

    if (n > remain) {
      n = remain;
    }

    if (memcmp(p, ms->body + off, n) != 0) {
      snprintf(world->diff, sizeof(world->diff),
               "slot=%zu drain body mismatch at %llu", slot,
               (unsigned long long)off);
      world->mismatch = 1;
      return 1;
    }

    ngtcp2_strm_update_rx_offset(&ps->strm, off + n);
    delivered += n;
  }

  world->result.delivered_bytes += delivered;

  snprintf(note, sizeof(note), "delivered=%llu",
           (unsigned long long)delivered);
  trace_line(world, step, rec->op, slot, "EXEC", note);

  return verify_all(world, "drain");
}

static int do_cancel(rxa_world *world, const rxa_rec *rec, uint64_t step) {
  size_t slot = rec->a % RXA_SLOTS;
  size_t live_before;

  if (!world->model[slot].alive) {
    trace_line(world, step, rec->op, slot, "SKIP", "slot-dead");
    return 0;
  }

  live_before = world->tmem.live;
  prod_destroy(world, slot);
  model_destroy(world, slot);
  ++world->result.canceled;

  /* Releasing the stream must return the allocations it owned; the
     shared frame objalloc and the other streams stay live. */
  if (world->tmem.live > live_before) {
    snprintf(world->diff, sizeof(world->diff),
             "slot=%zu cancel grew live allocations", slot);
    world->mismatch = 1;
    return 1;
  }

  trace_line(world, step, rec->op, slot, "EXEC", "released");

  return verify_all(world, "cancel");
}

static int do_failnext(rxa_world *world, const rxa_rec *rec, uint64_t step) {
  if (world->injections_enabled) {
    world->pending_fail = (int)(rec->a % 16);
  }

  trace_line(world, step, rec->op, rec->a % RXA_SLOTS, "EXEC",
             world->injections_enabled ? "armed" : "control");

  return 0;
}

/* ------------------------------------------------------------------ */
/* Finalization: complete, drain and release surviving streams        */
/* ------------------------------------------------------------------ */

static int finalize_world(rxa_world *world) {
  size_t slot;

  /* Disarm any leftover injection policy. */
  world->tmem.armed = 0;
  world->pending_fail = -1;

  for (slot = 0; slot < RXA_SLOTS; ++slot) {
    model_slot *ms;
    prod_slot *ps;
    uint64_t off, missing;

    if (!world->model[slot].alive) {
      continue;
    }

    ms = &world->model[slot];
    ps = &world->prod[slot];

    /* Feed every still-missing byte in legal, bounded pieces so that
       the surviving body becomes complete. */
    off = 0;
    while (off < ms->bodylen) {
      uint64_t run = 0;
      uint64_t start, gaplen, accepted = 0;
      uint8_t *snapshot;
      int rv;

      while (off + run < ms->bodylen && ms->present[off + run]) {
        ++run;
      }

      if (off + run == ms->bodylen) {
        break;
      }

      start = off + run;
      off = start;
      while (off < ms->bodylen && !ms->present[off]) {
        ++off;
      }

      gaplen = off - start;
      snapshot = malloc((size_t)gaplen);
      if (snapshot == NULL) {
        return NGTCP2_ERR_NOMEM;
      }
      memcpy(snapshot, ms->present + start, (size_t)gaplen);

      rv = feed_once(world, slot, start, gaplen, snapshot, &accepted);
      free(snapshot);
      if (rv != 0) {
        return rv < 0 ? rv : 1;
      }

      world->result.accepted_bytes += accepted;
    }

    /* Drain the complete body through the production path. */
    for (;;) {
      const uint8_t *p = NULL;
      size_t n = ngtcp2_rob_data_at(ps->strm.rx.rob, &p, ms->consumed);

      if (n == 0) {
        break;
      }

      if (memcmp(p, ms->body + ms->consumed, n) != 0) {
        snprintf(world->diff, sizeof(world->diff),
                 "slot=%zu finalize body mismatch", slot);
        world->mismatch = 1;
        return 1;
      }

      ngtcp2_strm_update_rx_offset(&ps->strm, ms->consumed + n);
      ms->consumed += n;
      world->result.delivered_bytes += n;
    }

    missing = model_front(ms);
    if (missing != ms->bodylen ||
        ngtcp2_strm_rx_offset(&ps->strm) != ms->bodylen) {
      snprintf(world->diff, sizeof(world->diff),
               "slot=%zu finalize incomplete front=%llu bodylen=%llu", slot,
               (unsigned long long)missing,
               (unsigned long long)ms->bodylen);
      world->mismatch = 1;
      return 1;
    }

    prod_destroy(world, slot);
    model_destroy(world, slot);
  }

  return 0;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                            */
/* ------------------------------------------------------------------ */

static int hexval(int ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }

  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }

  return -1;
}

static int parse_schedule(const uint8_t *data, size_t size, rxa_rec *recs,
                          size_t *pnrec) {
  size_t pos = 0;
  size_t nrec = 0;

  if (size < RXA_HEADER_LEN ||
      memcmp(data, RXA_HEADER, RXA_HEADER_LEN) != 0) {
    return 2;
  }

  pos = RXA_HEADER_LEN;

  while (pos < size) {
    uint32_t word = 0;
    size_t k;
    uint8_t op;

    if (nrec >= RXA_MAX_RECORDS) {
      return 2;
    }

    for (k = 0; k < 8; ++k) {
      int v;

      if (pos >= size) {
        return 2;
      }

      v = hexval(data[pos++]);
      if (v < 0) {
        return 2;
      }

      word = (word << 4) | (uint32_t)v;
    }

    if (pos >= size || data[pos++] != '\n') {
      return 2;
    }

    op = (uint8_t)((word >> 24) & 0xF);
    if (op > 4) {
      return 2;
    }

    recs[nrec].op = op;
    recs[nrec].a = (word >> 16) & 0xFF;
    recs[nrec].b = (word >> 8) & 0xFF;
    recs[nrec].c = word & 0xF;
    ++nrec;
  }

  *pnrec = nrec;

  return 0;
}

/* ------------------------------------------------------------------ */
/* Execution                                                          */
/* ------------------------------------------------------------------ */

static int execute_record(rxa_world *world, const rxa_rec *rec,
                          uint64_t step) {
  switch (rec->op) {
  case RXA_OP_OPEN:
    return do_open(world, rec, step);
  case RXA_OP_FEED:
    return do_feed(world, rec, step);
  case RXA_OP_DRAIN:
    return do_drain(world, rec, step);
  case RXA_OP_CANCEL:
    return do_cancel(world, rec, step);
  case RXA_OP_FAILNEXT:
    return do_failnext(world, rec, step);
  default:
    return 2;
  }
}

/*
 * run_pair executes the schedule twice in lockstep: once with the
 * single-allocation refusal windows and once as a refusal-free
 * control.  The two worlds are compared at every operation boundary.
 */
static int run_pair(const rxa_rec *recs, size_t nrec, rxcheck_result *result,
                    FILE *trace) {
  rxa_world stress;
  rxa_world control;
  size_t i;
  int rv = 0;

  world_init(&stress, 1, trace);
  world_init(&control, 0, NULL);

  for (i = 0; i < nrec; ++i) {
    ++stress.result.operations;
    ++control.result.operations;

    rv = execute_record(&stress, &recs[i], i);
    if (rv != 0) {
      goto out;
    }

    rv = execute_record(&control, &recs[i], i);
    if (rv != 0) {
      goto out;
    }

    if (compare_worlds(&stress, &control, i) != 0) {
      rv = 1;
      goto out;
    }
  }

  rv = finalize_world(&stress);
  if (rv != 0) {
    goto out;
  }

  rv = finalize_world(&control);
  if (rv != 0) {
    goto out;
  }

  if (stress.result.accepted_bytes != control.result.accepted_bytes ||
      stress.result.delivered_bytes != control.result.delivered_bytes ||
      stress.result.canceled != control.result.canceled ||
      stress.result.operations != control.result.operations) {
    if (trace != NULL) {
      fprintf(trace, "DIFF final totals diverged from control\n");
    }

    stress.mismatch = 1;
    rv = 1;
    goto out;
  }

  *result = stress.result;

  if (trace != NULL) {
    fprintf(trace,
            "RXCHECK operations=%llu accepted=%llu delivered=%llu "
            "canceled=%llu oom_retries=%llu\n",
            (unsigned long long)result->operations,
            (unsigned long long)result->accepted_bytes,
            (unsigned long long)result->delivered_bytes,
            (unsigned long long)result->canceled,
            (unsigned long long)result->oom_retries);
  }

out:
  if (stress.mismatch && trace != NULL) {
    fprintf(trace, "DIFF %s\n", stress.diff);
  }

  world_destroy(&control);
  world_destroy(&stress);

  return rv < 0 ? 1 : rv;
}

int rxcheck_run(const uint8_t *data, size_t size, rxcheck_result *result,
                FILE *trace) {
  rxa_rec recs[RXA_MAX_RECORDS];
  size_t nrec = 0;
  int rv;

  if (result == NULL) {
    return 2;
  }

  memset(result, 0, sizeof(*result));

  rv = parse_schedule(data, size, recs, &nrec);
  if (rv != 0) {
    return rv;
  }

  return run_pair(recs, nrec, result, trace);
}

/* ------------------------------------------------------------------ */
/* Deterministic mutator                                              */
/* ------------------------------------------------------------------ */

/*
 * rng is a small deterministic PRNG fully determined by the seed.
 */
typedef struct rng {
  uint64_t state;
} rng;

static void rng_init(rng *r, unsigned int seed) {
  r->state = (uint64_t)seed + 0x9e3779b97f4a7c15ULL;
}

static uint64_t rng_next(rng *r) {
  uint64_t x = r->state;

  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  r->state = x;

  return x * 0x2545f4914f6cdd1dULL;
}

static uint64_t rng_below(rng *r, uint64_t bound) {
  if (bound == 0) {
    return 0;
  }

  return rng_next(r) % bound;
}

static void emit_record(uint8_t *out, size_t *ppos, size_t cap, uint8_t op,
                        uint32_t a, uint32_t b, uint32_t c) {
  static const char hexd[] = "0123456789abcdef";
  /* Layout per record nibble: 0 op a_hi a_lo b_hi b_lo 0 c. */
  uint32_t word = ((uint32_t)(op & 0xF) << 24) | ((a & 0xFFU) << 16) |
                  ((b & 0xFFU) << 8) | (c & 0xFU);
  size_t pos = *ppos;
  int k;

  if (cap - pos < 9) {
    return;
  }

  for (k = 7; k >= 0; --k) {
    out[pos++] = (uint8_t)hexd[(word >> (4 * k)) & 0xF];
  }

  out[pos++] = '\n';
  *ppos = pos;
}

size_t rxcheck_mutate(uint8_t *data, size_t size, size_t max_size,
                      unsigned int seed) {
  rxa_rec recs[RXA_MAX_RECORDS];
  size_t nrec = 0;
  rng r;
  size_t pos;
  size_t action;
  size_t cap_records;
  size_t i;

  if (max_size < RXA_HEADER_LEN) {
    return 0;
  }

  if (parse_schedule(data, size, recs, &nrec) != 0) {
    /* Invalid inputs restart from an empty schedule. */
    nrec = 0;
  }

  rng_init(&r, seed);

  cap_records = (max_size - RXA_HEADER_LEN) / 9;
  if (cap_records > RXA_MAX_RECORDS) {
    cap_records = RXA_MAX_RECORDS;
  }

  action = (size_t)rng_below(&r, 4);

  if (action == 0 && nrec > 0) {
    /* Delete one record. */
    size_t idx = (size_t)rng_below(&r, nrec);
    memmove(&recs[idx], &recs[idx + 1],
            sizeof(recs[0]) * (nrec - idx - 1));
    --nrec;
  } else if (action == 1 && nrec > 0) {
    /* Change one record while keeping it syntactically legal. */
    size_t idx = (size_t)rng_below(&r, nrec);

    recs[idx].op = (uint8_t)rng_below(&r, 5);
    recs[idx].a = (uint32_t)rng_below(&r, 256);
    recs[idx].b = (uint32_t)rng_below(&r, 256);
    recs[idx].c = (uint32_t)rng_below(&r, 16);
  } else if (nrec < cap_records) {
    /* Insert a record, either at a random position or the end. */
    size_t idx = (size_t)rng_below(&r, nrec + 1);
    rxa_rec rec;

    rec.op = (uint8_t)rng_below(&r, 5);
    rec.a = (uint32_t)rng_below(&r, 256);
    rec.b = (uint32_t)rng_below(&r, 256);
    rec.c = (uint32_t)rng_below(&r, 16);

    memmove(&recs[idx + 1], &recs[idx], sizeof(recs[0]) * (nrec - idx));
    recs[idx] = rec;
    ++nrec;
  } else if (nrec > 0) {
    /* Full buffer: overwrite in place. */
    size_t idx = (size_t)rng_below(&r, nrec);

    recs[idx].op = (uint8_t)rng_below(&r, 5);
    recs[idx].a = (uint32_t)rng_below(&r, 256);
    recs[idx].b = (uint32_t)rng_below(&r, 256);
    recs[idx].c = (uint32_t)rng_below(&r, 16);
  }

  memcpy(data, RXA_HEADER, RXA_HEADER_LEN);
  pos = RXA_HEADER_LEN;

  for (i = 0; i < nrec; ++i) {
    emit_record(data, &pos, max_size, recs[i].op, recs[i].a, recs[i].b,
                recs[i].c);
  }

  return pos;
}
