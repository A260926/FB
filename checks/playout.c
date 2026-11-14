/* Copyright (C) 2026 Xiph.Org Foundation
   File: playout.c

   Playout, reordering and ownership regression checks for the
   SpeexDSP jitter buffer (public API only).

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
   These checks drive jitter_buffer_* through scripted playout sessions and
   compare every observable event against an independent expectation model
   that this file maintains next to the library:

   - Media time lives on a 64-bit *unwrapped* timeline inside the model.
     Values are truncated to spx_uint32_t only at the moment they are handed
     to jitter_buffer_put().  Every script keeps all live timestamps inside
     a window far smaller than 2^31 around the play cursor, so the unwrapped
     model and the library's modular arithmetic describe the same ordering.
   - A packet identity ledger tracks which packets are queued, which media
     interval [start, start+span) each one covers, and the arrival order of
     the puts (kept separate from media time).  Sequence numbers are used
     purely as identity tags, never as an ordering key.
   - Expected events are derived from declarative interval rules (which set
     of queued packets covers the cursor, which packet starts earliest
     inside the consumption chunk, whether the chunk is a gap), not from
     the branch order of the library implementation.  The library's own
     time-comparison macros are never used, its internal state is never
     read, and no second library instance is consulted for answers.

   See checks/playout.md for the mapping between mutations, failing events
   and the API behaviour being pinned down.
*/

#include "task_support/check.h"
#include "speex/speex_jitter.h"

#include <stdint.h>
#include <string.h>

#define STEP_SIZE 10              /* jitter_buffer_init(10): delay step == concealment == 10 */
#define PKT_LEN 4                 /* payload bytes per packet in these scripts */
#define MDL_MAX 32
#define LOG_CAP 64
#define CURSOR_NA 0xFFFFFFFFu     /* log marker: no meaningful cursor on this entry */
#define TS_NA 0xFFFFFFFFu         /* log marker: no timestamp returned on this event */

enum { OP_PUT = 1, OP_GET = 2, OP_ANOTHER = 3, OP_MARK = 4 };
enum { MARK_TICK = 1, MARK_REMAIN = 2, MARK_UPDELAY = 3 };

/* One scripted packet.  Media time is relative to the script origin and
   never wraps the 64-bit axis; arrival order is decided by the script,
   not by this table. */
struct pkt_def {
   int64_t start;      /* unwrapped media time, relative to the script origin */
   uint32_t span;      /* media units covered */
   uint16_t seq;       /* identity tag only - never used for ordering */
   uint32_t ud;        /* user_data identity tag */
   char tag[PKT_LEN];  /* expected payload bytes */
};

/* The independent model: unwrapped timeline + identity ledger. */
struct model {
   const struct pkt_def *defs;
   int ndefs;
   int inq[MDL_MAX];       /* identity ledger: 1 while queued in the buffer */
   int64_t pointer;        /* expected play cursor (unwrapped) */
   int64_t last_ret;       /* timestamp anchor for get_another (unwrapped) */
   int reset_state;        /* mirrors the public reset/sync contract */
   int may_have_timings;   /* set once a put could record arrival statistics */
   int32_t step;           /* delay step (jitter_buffer_init argument) */
   int32_t conceal;        /* concealment unit */
};

/* What the model says the next call must produce. */
struct expect {
   int rc;                 /* JITTER_BUFFER_OK / JITTER_BUFFER_MISSING */
   int cand[MDL_MAX];      /* acceptable packet identities when rc == OK */
   int ncand;
   int64_t ts;             /* expected media time of the event (MISS: gap start) */
   uint32_t span;          /* expected span (MISS only; OK comes from pkt_def) */
   int64_t off;            /* expected start_offset (get only) */
   int64_t cursor;         /* expected public play cursor after the call */
};

/* One recorded event.  Media-time fields are de-shifted (origin removed);
   arrival order is the position of the entry in the log. */
struct logent {
   int op;                 /* OP_PUT / OP_GET / OP_ANOTHER / OP_MARK */
   int id;                 /* packet identity, -1 when none */
   int rc;                 /* return code for gets, 0 otherwise */
   uint32_t ts;            /* media timestamp relative to the origin */
   uint32_t span;
   uint32_t cursor;        /* public play cursor relative to the origin */
   uint32_t len;
   uint32_t ud;            /* user_data */
   int32_t off;            /* start_offset (get only) */
   uint16_t seq;           /* sequence (identity tag) */
   char payload[PKT_LEN];
};

/* ------------------------------------------------------------------ */
/* Independent expectation model                                       */
/* ------------------------------------------------------------------ */

static void mdl_init(struct model *m, const struct pkt_def *defs, int ndefs)
{
   int i;
   m->defs = defs;
   m->ndefs = ndefs;
   for (i = 0; i < MDL_MAX; i++)
      m->inq[i] = 0;
   m->pointer = 0;
   m->last_ret = 0;
   m->reset_state = 1;
   m->may_have_timings = 0;
   m->step = STEP_SIZE;
   m->conceal = STEP_SIZE;
}

static void mdl_reset(struct model *m)
{
   int i;
   for (i = 0; i < MDL_MAX; i++)
      m->inq[i] = 0;
   m->pointer = 0;
   m->reset_state = 1;
   m->may_have_timings = 0;
}

/* A put first sweeps queued packets whose coverage ends at or before the
   play cursor (they expired unplayed), then keeps the new packet unless it
   is hopelessly late.  All arithmetic here is unwrapped 64-bit. */
static int mdl_put(struct model *m, int id)
{
   int i;
   if (!m->reset_state) {
      for (i = 0; i < m->ndefs; i++) {
         if (m->inq[i] && m->defs[i].start + (int64_t)m->defs[i].span <= m->pointer)
            m->inq[i] = 0;
      }
      m->may_have_timings = 1;
   }
   /* The scripts never offer a packet the buffer would refuse. */
   TASK_CHECK(m->reset_state ||
              m->defs[id].start + (int64_t)m->defs[id].span + m->step >= m->pointer);
   TASK_CHECK(id >= 0 && id < m->ndefs && !m->inq[id]);
   m->inq[id] = 1;
   return 0;
}

/* Declarative get() expectation:
   - if any queued packet covers the cursor (start <= p < start+span), one
     of them comes out; the scripts are built so that all such candidates
     share the same interval (same-timestamp fragments), making the outcome
     determined up to identity, and the identity set is what we check;
   - otherwise the packet with the smallest start inside the consumption
     chunk [p, p+desired) comes out (unique in every script);
   - otherwise the chunk is a gap: MISSING and the cursor advances by the
     concealment-rounded desired span.  A gap is only scripted when no
     arrival statistics can exist, because with a timing history the buffer
     would be allowed to answer with a delay-inflating INSERTION instead. */
static int mdl_get_expect(struct model *m, int32_t desired, struct expect *e)
{
   int i;
   e->ncand = 0;
   e->span = 0;
   e->off = 0;
   if (m->reset_state) {
      /* First get after a reset syncs the cursor onto the oldest packet. */
      int found = 0;
      int64_t oldest = 0;
      for (i = 0; i < m->ndefs; i++) {
         if (m->inq[i] && (!found || m->defs[i].start < oldest)) {
            oldest = m->defs[i].start;
            found = 1;
         }
      }
      TASK_CHECK(found); /* every script preloads before the first get */
      m->pointer = oldest;
      m->reset_state = 0;
   }
   m->last_ret = m->pointer;
   /* Covering set. */
   for (i = 0; i < m->ndefs; i++) {
      if (m->inq[i] && m->defs[i].start <= m->pointer &&
          m->pointer < m->defs[i].start + (int64_t)m->defs[i].span)
         e->cand[e->ncand++] = i;
   }
   if (e->ncand > 0) {
      for (i = 1; i < e->ncand; i++) {
         TASK_CHECK(m->defs[e->cand[i]].start == m->defs[e->cand[0]].start);
         TASK_CHECK(m->defs[e->cand[i]].span == m->defs[e->cand[0]].span);
      }
      e->rc = JITTER_BUFFER_OK;
      e->off = m->defs[e->cand[0]].start - m->pointer;
      e->cursor = m->defs[e->cand[0]].start + (int64_t)m->defs[e->cand[0]].span;
      return 0;
   }
   /* Earliest starter inside the chunk. */
   {
      int have = 0;
      int64_t best = 0;
      for (i = 0; i < m->ndefs; i++) {
         if (m->inq[i] && m->defs[i].start >= m->pointer &&
             m->defs[i].start < m->pointer + desired) {
            if (!have || m->defs[i].start < best) {
               best = m->defs[i].start;
               have = 1;
            }
         }
      }
      if (have) {
         for (i = 0; i < m->ndefs; i++)
            if (m->inq[i] && m->defs[i].start == best)
               e->cand[e->ncand++] = i;
         TASK_CHECK(e->ncand == 1); /* unique earliest starter in all scripts */
         e->rc = JITTER_BUFFER_OK;
         e->off = best - m->pointer;
         e->cursor = best + (int64_t)m->defs[e->cand[0]].span;
         return 0;
      }
   }
   /* Gap. */
   TASK_CHECK(!m->may_have_timings); /* otherwise INSERTION would be legal */
   e->rc = JITTER_BUFFER_MISSING;
   e->ts = m->pointer;
   e->span = (uint32_t)(desired / m->conceal) * (uint32_t)m->conceal;
   e->cursor = m->pointer + (int64_t)e->span;
   return 0;
}

static void mdl_get_commit(struct model *m, const struct expect *e, int id)
{
   if (e->rc == JITTER_BUFFER_OK) {
      m->inq[id] = 0;
      m->last_ret = m->defs[id].start;
   }
   m->pointer = e->cursor;
}

/* get_another anchors on the timestamp of the last returned packet, never
   on the play cursor; it must not move the cursor. */
static int mdl_another_expect(struct model *m, struct expect *e)
{
   int i;
   e->ncand = 0;
   for (i = 0; i < m->ndefs; i++)
      if (m->inq[i] && m->defs[i].start == m->last_ret)
         e->cand[e->ncand++] = i;
   e->rc = e->ncand ? JITTER_BUFFER_OK : JITTER_BUFFER_MISSING;
   e->cursor = m->pointer;
   e->off = 0;
   e->span = 0;
   e->ts = m->last_ret;
   return 0;
}

static void mdl_another_commit(struct model *m, const struct expect *e, int id)
{
   if (e->rc == JITTER_BUFFER_OK)
      m->inq[id] = 0;
}

/* ------------------------------------------------------------------ */
/* Scripted drivers: run one library call, check it against the model,  */
/* and append the de-shifted event to the log.                          */
/* ------------------------------------------------------------------ */

static int find_by_seq(const struct pkt_def *defs, int ndefs, uint16_t seq)
{
   int i;
   for (i = 0; i < ndefs; i++)
      if (defs[i].seq == seq)
         return i;
   return -1;
}

static int expect_has(const struct expect *e, int id)
{
   int i;
   for (i = 0; i < e->ncand; i++)
      if (e->cand[i] == id)
         return 1;
   return 0;
}

#define STEP(x) do { if (x) { jitter_buffer_destroy(jb); return 1; } } while (0)
/* For helpers that borrow a jitter buffer owned by their caller: fail
   without destroying, so the buffer is destroyed exactly once. */
#define QSTEP(x) do { if (x) return 1; } while (0)

static void log_mark(struct logent *log, int *nl, int kind, uint32_t arg, int rc)
{
   struct logent *le;
   if (!log)
      return; /* logging is optional for scripts that do not compare logs */
   le = &log[*nl];
   le->op = OP_MARK;
   le->id = kind;
   le->rc = rc;
   le->ts = arg;
   le->span = 0;
   le->cursor = CURSOR_NA;
   le->len = 0;
   le->ud = 0;
   le->off = 0;
   le->seq = 0;
   memset(le->payload, 0, PKT_LEN);
   ++*nl;
}

static int do_put(JitterBuffer *jb, struct model *m, int id, spx_uint32_t origin,
                  const char *src, struct logent *log, int *nl)
{
   JitterBufferPacket in;
   struct logent *le = &log[*nl];
   if (mdl_put(m, id))
      return 1;
   in.data = (char *)src;
   in.len = PKT_LEN;
   /* The unwrapped media time is truncated to 32 bits only here. */
   in.timestamp = origin + (spx_uint32_t)m->defs[id].start;
   in.span = m->defs[id].span;
   in.sequence = m->defs[id].seq;
   in.user_data = m->defs[id].ud;
   jitter_buffer_put(jb, &in);
   le->op = OP_PUT;
   le->id = id;
   le->rc = 0;
   le->ts = (spx_uint32_t)m->defs[id].start;
   le->span = m->defs[id].span;
   le->cursor = CURSOR_NA; /* pre-sync cursor is absolute, not origin-relative */
   le->len = PKT_LEN;
   le->ud = m->defs[id].ud;
   le->off = 0;
   le->seq = m->defs[id].seq;
   memcpy(le->payload, m->defs[id].tag, PKT_LEN);
   ++*nl;
   return 0;
}

static int do_get(JitterBuffer *jb, struct model *m, spx_uint32_t origin,
                  spx_int32_t desired, struct logent *log, int *nl,
                  struct logent *out)
{
   struct expect e;
   JitterBufferPacket pkt;
   spx_int32_t off = -9999;
   char buf[8];
   int rc, id;
   struct logent *le = &log[*nl];
   if (mdl_get_expect(m, desired, &e))
      return 1;
   memset(buf, '?', sizeof buf);
   pkt.data = buf;
   pkt.len = PKT_LEN;
   pkt.timestamp = 0x5A5A5A5Au;
   pkt.span = 0xA5A5A5A5u;
   pkt.sequence = 0xBEEF;
   pkt.user_data = 0xDEADBEEFu;
   rc = jitter_buffer_get(jb, &pkt, desired, &off);
   le->op = OP_GET;
   le->id = -1;
   le->rc = rc;
   le->ts = pkt.timestamp - origin;
   le->span = pkt.span;
   le->cursor = (spx_uint32_t)jitter_buffer_get_pointer_timestamp(jb) - origin;
   le->len = pkt.len;
   le->ud = pkt.user_data;
   le->off = off;
   le->seq = pkt.sequence;
   memcpy(le->payload, buf, PKT_LEN);
   TASK_CHECK(rc == e.rc);
   if (rc == JITTER_BUFFER_OK) {
      id = find_by_seq(m->defs, m->ndefs, pkt.sequence);
      TASK_CHECK(id >= 0);
      TASK_CHECK(expect_has(&e, id));
      le->id = id;
      TASK_CHECK(le->ts == (spx_uint32_t)m->defs[id].start);
      TASK_CHECK(le->span == m->defs[id].span);
      TASK_CHECK(le->ud == m->defs[id].ud);
      TASK_CHECK(off == (spx_int32_t)e.off);
      TASK_CHECK(pkt.len == PKT_LEN);
      TASK_CHECK(memcmp(buf, m->defs[id].tag, PKT_LEN) == 0);
      mdl_get_commit(m, &e, id);
   } else {
      /* Gap report: gap start, concealment-rounded span, nothing copied,
         sequence/user_data untouched, zero start_offset. */
      TASK_CHECK(le->ts == (spx_uint32_t)e.ts);
      TASK_CHECK(le->span == e.span);
      TASK_CHECK(pkt.len == 0);
      TASK_CHECK(off == 0);
      TASK_CHECK(pkt.sequence == 0xBEEF);
      TASK_CHECK(pkt.user_data == 0xDEADBEEFu);
      mdl_get_commit(m, &e, -1);
   }
   TASK_CHECK(le->cursor == (spx_uint32_t)e.cursor);
   ++*nl;
   if (out)
      *out = *le;
   return 0;
}

static int do_get_another(JitterBuffer *jb, struct model *m, spx_uint32_t origin,
                          struct logent *log, int *nl, struct logent *out)
{
   struct expect e;
   JitterBufferPacket pkt;
   char buf[8];
   int rc, id;
   struct logent *le = &log[*nl];
   if (mdl_another_expect(m, &e))
      return 1;
   memset(buf, '?', sizeof buf);
   pkt.data = buf;
   pkt.len = PKT_LEN;
   pkt.timestamp = 0x5A5A5A5Au;
   pkt.span = 0xA5A5A5A5u;
   pkt.sequence = 0xBEEF;
   pkt.user_data = 0xDEADBEEFu;
   rc = jitter_buffer_get_another(jb, &pkt);
   le->op = OP_ANOTHER;
   le->id = -1;
   le->rc = rc;
   le->ts = pkt.timestamp - origin;
   le->span = pkt.span;
   le->cursor = (spx_uint32_t)jitter_buffer_get_pointer_timestamp(jb) - origin;
   le->len = pkt.len;
   le->ud = pkt.user_data;
   le->off = 0; /* get_another has no start_offset */
   le->seq = pkt.sequence;
   memcpy(le->payload, buf, PKT_LEN);
   TASK_CHECK(rc == e.rc);
   if (rc == JITTER_BUFFER_OK) {
      id = find_by_seq(m->defs, m->ndefs, pkt.sequence);
      TASK_CHECK(id >= 0);
      TASK_CHECK(expect_has(&e, id));
      le->id = id;
      TASK_CHECK(le->ts == (spx_uint32_t)m->defs[id].start);
      TASK_CHECK(le->span == m->defs[id].span);
      TASK_CHECK(le->ud == m->defs[id].ud);
      TASK_CHECK(pkt.len == PKT_LEN);
      TASK_CHECK(memcmp(buf, m->defs[id].tag, PKT_LEN) == 0);
      mdl_another_commit(m, &e, id);
   } else {
      TASK_CHECK(pkt.data == NULL);
      TASK_CHECK(pkt.len == 0);
      TASK_CHECK(pkt.span == 0);
      TASK_CHECK(pkt.sequence == 0xBEEF);
      TASK_CHECK(pkt.user_data == 0xDEADBEEFu);
      le->ts = TS_NA; /* no timestamp returned: keep the log origin-independent */
      memset(le->payload, 0, PKT_LEN);
   }
   /* A trailing fetch must never move the play cursor. */
   TASK_CHECK(le->cursor == (spx_uint32_t)e.cursor);
   ++*nl;
   if (out)
      *out = *le;
   return 0;
}

static int do_updelay(JitterBuffer *jb, struct logent *log, int *nl)
{
   /* Switch to manual delay mode while no arrival statistics exist: the
      call must be a no-op and later tick/remaining_span calls must not
      move the cursor on their own. */
   int rc = jitter_buffer_update_delay(jb, NULL, NULL);
   log_mark(log, nl, MARK_UPDELAY, 0, rc);
   TASK_CHECK(rc == 0);
   return 0;
}

static void do_tick(JitterBuffer *jb, struct logent *log, int *nl)
{
   jitter_buffer_tick(jb);
   log_mark(log, nl, MARK_TICK, 0, 0);
}

static void do_remain(JitterBuffer *jb, spx_uint32_t rem, struct logent *log, int *nl)
{
   jitter_buffer_remaining_span(jb, rem);
   log_mark(log, nl, MARK_REMAIN, rem, 0);
}

/* ------------------------------------------------------------------ */
/* wrap_timeline: one relative script, several absolute origins         */
/* ------------------------------------------------------------------ */

/* Media layout (relative units, consumption chunks of 10):
   A [0,30) long; B [30,40); C [40,50); D [45,60) starts before the cursor
   but still covers the chunk; E [65,75) starts slightly after the cursor;
   F [75,85); G [85,115) long; H [110,118) only partly covers the chunk;
   I [120,130).  Packets never overlap while co-resident, so every get has
   exactly one legal candidate. */
static const struct pkt_def wrap_pkts[] = {
   {   0, 30, 101, 1001, {'P','A','0','1'} }, /* 0: long frame */
   {  30, 10, 102, 1002, {'P','B','0','2'} }, /* 1 */
   {  40, 10, 103, 1003, {'P','C','0','3'} }, /* 2 */
   {  45, 15, 104, 1004, {'P','D','0','4'} }, /* 3: earlier start, covers chunk */
   {  65, 10, 105, 1005, {'P','E','0','5'} }, /* 4: starts after the cursor */
   {  75, 10, 106, 1006, {'P','F','0','6'} }, /* 5 */
   {  85, 30, 107, 1007, {'P','G','0','7'} }, /* 6: long frame */
   { 110,  8, 108, 1008, {'P','H','0','8'} }, /* 7: partial cover */
   { 120, 10, 109, 1009, {'P','I','0','9'} }, /* 8 */
};
#define WRAP_NPKTS 9
#define WRAP_NGET 9

static int wrap_script(JitterBuffer *jb, spx_uint32_t origin,
                       struct logent *log, int *nl)
{
   struct model m;
   *nl = 0;
   mdl_init(&m, wrap_pkts, WRAP_NPKTS);
   /* Preload with arrival order scrambled against media order. */
   QSTEP(do_put(jb, &m, 2, origin, wrap_pkts[2].tag, log, nl));
   QSTEP(do_put(jb, &m, 0, origin, wrap_pkts[0].tag, log, nl));
   QSTEP(do_put(jb, &m, 1, origin, wrap_pkts[1].tag, log, nl));
   QSTEP(do_updelay(jb, log, nl));
   /* Long frame first; the application reports the still-buffered span. */
   QSTEP(do_get(jb, &m, origin, STEP_SIZE, log, nl, NULL));
   do_remain(jb, 20, log, nl);
   do_tick(jb, log, nl);
   /* No packet shares the returned timestamp: trailing fetch finds nothing. */
   QSTEP(do_get_another(jb, &m, origin, log, nl, NULL));
   QSTEP(do_get(jb, &m, origin, STEP_SIZE, log, nl, NULL));
   do_tick(jb, log, nl);
   QSTEP(do_get(jb, &m, origin, STEP_SIZE, log, nl, NULL));
   do_tick(jb, log, nl);
   /* Mid-stream arrivals, again out of order. */
   QSTEP(do_put(jb, &m, 4, origin, wrap_pkts[4].tag, log, nl));
   QSTEP(do_put(jb, &m, 3, origin, wrap_pkts[3].tag, log, nl));
   QSTEP(do_put(jb, &m, 6, origin, wrap_pkts[6].tag, log, nl));
   QSTEP(do_put(jb, &m, 5, origin, wrap_pkts[5].tag, log, nl));
   QSTEP(do_get(jb, &m, origin, STEP_SIZE, log, nl, NULL)); /* D: offset < 0 */
   do_tick(jb, log, nl);
   QSTEP(do_get(jb, &m, origin, STEP_SIZE, log, nl, NULL)); /* E: offset > 0 */
   do_tick(jb, log, nl);
   QSTEP(do_get(jb, &m, origin, STEP_SIZE, log, nl, NULL)); /* F */
   do_tick(jb, log, nl);
   QSTEP(do_get(jb, &m, origin, STEP_SIZE, log, nl, NULL)); /* G: long frame */
   do_remain(jb, 20, log, nl);
   do_tick(jb, log, nl);
   QSTEP(do_put(jb, &m, 8, origin, wrap_pkts[8].tag, log, nl));
   QSTEP(do_put(jb, &m, 7, origin, wrap_pkts[7].tag, log, nl));
   QSTEP(do_get(jb, &m, origin, STEP_SIZE, log, nl, NULL)); /* H: partial cover */
   do_tick(jb, log, nl);
   QSTEP(do_get(jb, &m, origin, STEP_SIZE, log, nl, NULL)); /* I */
   do_tick(jb, log, nl);
   QSTEP(do_get_another(jb, &m, origin, log, nl, NULL));
   return 0;
}

static int logent_equal(const struct logent *a, const struct logent *b)
{
   return a->op == b->op && a->id == b->id && a->rc == b->rc &&
          a->ts == b->ts && a->span == b->span && a->cursor == b->cursor &&
          a->len == b->len && a->ud == b->ud && a->off == b->off &&
          a->seq == b->seq && memcmp(a->payload, b->payload, PKT_LEN) == 0;
}

static int case_wrap_plain(void)
{
   /* Hand-derived from the interval layout: the cursor must land on the
      end of each returned packet, and start_offset must be the signed
      distance between the packet start and the cursor before the get. */
   static const uint32_t want_cur[WRAP_NGET] = {30, 40, 50, 60, 75, 85, 115, 118, 130};
   static const int32_t want_off[WRAP_NGET] = {0, 0, 0, -5, 5, 0, 0, -5, 2};
   struct logent log[LOG_CAP];
   JitterBuffer *jb;
   int nl = 0, g = 0, a = 0, i, rc;
   jb = jitter_buffer_init(STEP_SIZE);
   TASK_CHECK(jb != NULL);
   rc = wrap_script(jb, 1000, log, &nl);
   jitter_buffer_destroy(jb);
   if (rc)
      return 1;
   for (i = 0; i < nl; i++) {
      if (log[i].op == OP_GET) {
         TASK_CHECK(g < WRAP_NGET);
         TASK_CHECK(log[i].rc == JITTER_BUFFER_OK);
         TASK_CHECK(log[i].id == g); /* consumption in media order */
         TASK_CHECK(log[i].cursor == want_cur[g]);
         TASK_CHECK(log[i].off == want_off[g]);
         g++;
      } else if (log[i].op == OP_ANOTHER) {
         TASK_CHECK(log[i].rc == JITTER_BUFFER_MISSING);
         a++;
      }
   }
   TASK_CHECK(g == WRAP_NGET);
   TASK_CHECK(a == 2);
   return 0;
}

static int case_wrap_shifted(void)
{
   /* The same relative script at a plain origin, an origin crossing
      INT32_MAX and an origin crossing UINT32_MAX.  After removing the
      shift, the complete event records must be identical. */
   static const spx_uint32_t origins[3] = {1000u, 0x7FFFFFF0u, 0xFFFFFFF0u};
   static struct logent logs[3][LOG_CAP];
   int nls[3];
   int i, k;
   for (k = 0; k < 3; k++) {
      JitterBuffer *jb = jitter_buffer_init(STEP_SIZE);
      TASK_CHECK(jb != NULL);
      if (wrap_script(jb, origins[k], logs[k], &nls[k])) {
         jitter_buffer_destroy(jb);
         return 1;
      }
      jitter_buffer_destroy(jb);
   }
   TASK_CHECK(nls[0] == nls[1] && nls[1] == nls[2]);
   TASK_CHECK(nls[0] > 0 && nls[0] <= LOG_CAP);
   for (i = 0; i < nls[0]; i++) {
      TASK_CHECK(logent_equal(&logs[0][i], &logs[1][i]));
      TASK_CHECK(logent_equal(&logs[0][i], &logs[2][i]));
   }
   return 0;
}

/* ------------------------------------------------------------------ */
/* fragment_cursor: mixed frames, same-timestamp fragments, gap         */
/* ------------------------------------------------------------------ */

/* L [0,30) long; F1..F3 [30,40) three fragments of one frame; Q [35,55)
   starts before the cursor but still covers the chunk; R [58,68) starts
   slightly after the cursor.  Q and the fragments overlap in media time
   but are never candidates for the same cursor position. */
static const struct pkt_def frag_pkts[] = {
   {  0, 30, 201, 2001, {'L','0','0','1'} }, /* 0: long frame, fetched first */
   { 30, 10, 211, 2111, {'F','1','1','1'} }, /* 1: fragment of frame @30 */
   { 30, 10, 212, 2122, {'F','1','2','2'} }, /* 2: fragment of frame @30 */
   { 30, 10, 213, 2133, {'F','1','3','3'} }, /* 3: fragment of frame @30 */
   { 35, 20, 214, 2144, {'Q','0','1','4'} }, /* 4: earlier start, covers chunk */
   { 58, 10, 215, 2155, {'R','0','1','5'} }, /* 5: starts after the cursor */
};
#define FRAG_NPKTS 6
#define FRAG_ORIGIN 3000u

static int case_fragment_mixed(void)
{
   struct logent log[LOG_CAP];
   struct logent ev;
   JitterBuffer *jb;
   struct model m;
   int nl = 0, got1, got2;
   jb = jitter_buffer_init(STEP_SIZE);
   TASK_CHECK(jb != NULL);
   mdl_init(&m, frag_pkts, FRAG_NPKTS);
   /* Out-of-order submission of mixed-length frames. */
   STEP(do_put(jb, &m, 2, FRAG_ORIGIN, frag_pkts[2].tag, log, &nl));
   STEP(do_put(jb, &m, 0, FRAG_ORIGIN, frag_pkts[0].tag, log, &nl));
   STEP(do_put(jb, &m, 4, FRAG_ORIGIN, frag_pkts[4].tag, log, &nl));
   STEP(do_put(jb, &m, 3, FRAG_ORIGIN, frag_pkts[3].tag, log, &nl));
   STEP(do_put(jb, &m, 1, FRAG_ORIGIN, frag_pkts[1].tag, log, &nl));
   STEP(do_updelay(jb, log, &nl));
   /* The long packet comes out first; report the remaining buffered span. */
   STEP(do_get(jb, &m, FRAG_ORIGIN, STEP_SIZE, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_OK && ev.id == 0);
   TASK_CHECK(ev.cursor == 30);
   do_remain(jb, 20, log, &nl);
   do_tick(jb, log, &nl);
   /* First fragment of the shared-timestamp frame (identity set {1,2,3}). */
   STEP(do_get(jb, &m, FRAG_ORIGIN, STEP_SIZE, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_OK);
   got1 = ev.id;
   TASK_CHECK(got1 >= 1 && got1 <= 3);
   TASK_CHECK(ev.cursor == 40);
   do_tick(jb, log, &nl);
   /* Drain another fragment; the play cursor must not move. */
   STEP(do_get_another(jb, &m, FRAG_ORIGIN, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_OK);
   got2 = ev.id;
   TASK_CHECK(got2 >= 1 && got2 <= 3 && got2 != got1);
   TASK_CHECK(ev.cursor == 40);
   /* The last fragment stays queued and expires exactly at the cursor
      (end == 40 == cursor).  The next put must sweep it, so the trailing
      fetch afterwards finds nothing. */
   STEP(do_put(jb, &m, 5, FRAG_ORIGIN, frag_pkts[5].tag, log, &nl));
   STEP(do_get_another(jb, &m, FRAG_ORIGIN, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_MISSING);
   TASK_CHECK(ev.cursor == 40);
   /* Packet that started before the cursor but still covers the chunk. */
   STEP(do_get(jb, &m, FRAG_ORIGIN, STEP_SIZE, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_OK && ev.id == 4);
   TASK_CHECK(ev.off == -5); /* negative start_offset */
   TASK_CHECK(ev.cursor == 55);
   do_tick(jb, log, &nl);
   /* Packet that starts slightly after the cursor. */
   STEP(do_get(jb, &m, FRAG_ORIGIN, STEP_SIZE, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_OK && ev.id == 5);
   TASK_CHECK(ev.off == 3); /* positive start_offset */
   TASK_CHECK(ev.cursor == 68);
   do_tick(jb, log, &nl);
   STEP(do_get_another(jb, &m, FRAG_ORIGIN, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_MISSING);
   TASK_CHECK(ev.cursor == 68);
   jitter_buffer_destroy(jb);
   return 0;
}

/* G1 [0,10); gap [10,20); G2 [20,30). */
static const struct pkt_def gap_pkts[] = {
   {  0, 10, 301, 3011, {'G','1','0','1'} }, /* 0 */
   { 20, 10, 302, 3022, {'G','2','0','2'} }, /* 1 */
};
#define GAP_NPKTS 2
#define GAP_ORIGIN 4000u

static int case_fragment_gap(void)
{
   /* The buffer is reset and then preloaded before playout starts, so no
      arrival statistics can exist: every put lands in reset state and is
      stamped "no arrival", and no fetched packet can record a timing.
      With an empty timing history the buffer has no basis for growing the
      playout delay, so the gap must surface as JITTER_BUFFER_MISSING
      spanning one concealment unit - never as a delay-inflating
      JITTER_BUFFER_INSERTION.  Each return code is checked individually;
      nothing is polled past. */
   struct logent log[LOG_CAP];
   struct logent ev;
   JitterBuffer *jb;
   struct model m;
   int nl = 0;
   jb = jitter_buffer_init(STEP_SIZE);
   TASK_CHECK(jb != NULL);
   mdl_init(&m, gap_pkts, GAP_NPKTS);
   STEP(do_updelay(jb, log, &nl));
   jitter_buffer_reset(jb);
   mdl_reset(&m);
   /* Preloaded input, out of order, no arrival statistics. */
   STEP(do_put(jb, &m, 1, GAP_ORIGIN, gap_pkts[1].tag, log, &nl));
   STEP(do_put(jb, &m, 0, GAP_ORIGIN, gap_pkts[0].tag, log, &nl));
   STEP(do_get(jb, &m, GAP_ORIGIN, STEP_SIZE, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_OK && ev.id == 0);
   TASK_CHECK(ev.cursor == 10);
   /* The gap: MISSING with the gap start as timestamp and a span of one
      concealment unit; the cursor advances across the hole. */
   STEP(do_get(jb, &m, GAP_ORIGIN, STEP_SIZE, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_MISSING);
   TASK_CHECK(ev.ts == 10);
   TASK_CHECK(ev.span == STEP_SIZE);
   TASK_CHECK(ev.len == 0);
   TASK_CHECK(ev.off == 0);
   TASK_CHECK(ev.cursor == 20);
   /* The next packet recovers playout right after the gap. */
   STEP(do_get(jb, &m, GAP_ORIGIN, STEP_SIZE, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_OK && ev.id == 1);
   TASK_CHECK(ev.ts == 20);
   TASK_CHECK(ev.cursor == 30);
   STEP(do_get_another(jb, &m, GAP_ORIGIN, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_MISSING);
   TASK_CHECK(ev.cursor == 30);
   jitter_buffer_destroy(jb);
   return 0;
}

/* ------------------------------------------------------------------ */
/* ownership: copy mode and destroy-callback mode                       */
/* ------------------------------------------------------------------ */

static const struct pkt_def copy_pkts[] = {
   {  0, 10, 401, 4101, {'C','1','a','1'} }, /* 0 */
   { 10, 10, 402, 4102, {'C','2','a','2'} }, /* 1 */
   { 20, 20, 403, 4103, {'C','3','a','3'} }, /* 2: long frame */
   { 40, 10, 404, 4104, {'C','4','a','4'} }, /* 3 */
};
#define COPY_NPKTS 4
#define COPY_ORIGIN 2000u

static void scribble(char *b)
{
   /* Overwrite the caller's input buffer after the put. */
   b[0] ^= 0x5A;
   b[1] ^= 0x3C;
   b[2] ^= 0x66;
   b[3] ^= 0x11;
}

static int case_own_copy(void)
{
   /* Default copy mode: the buffer copies packet data at put time, so
      rewriting the input afterwards must not change what comes back. */
   struct logent log[LOG_CAP];
   struct logent ev;
   char buf[COPY_NPKTS][PKT_LEN];
   JitterBuffer *jb;
   struct model m;
   int nl = 0, i;
   for (i = 0; i < COPY_NPKTS; i++)
      memcpy(buf[i], copy_pkts[i].tag, PKT_LEN);
   jb = jitter_buffer_init(STEP_SIZE);
   TASK_CHECK(jb != NULL);
   mdl_init(&m, copy_pkts, COPY_NPKTS);
   STEP(do_put(jb, &m, 0, COPY_ORIGIN, buf[0], log, &nl));
   scribble(buf[0]);
   STEP(do_put(jb, &m, 1, COPY_ORIGIN, buf[1], log, &nl));
   scribble(buf[1]);
   STEP(do_put(jb, &m, 2, COPY_ORIGIN, buf[2], log, &nl));
   scribble(buf[2]);
   STEP(do_updelay(jb, log, &nl));
   STEP(do_get(jb, &m, COPY_ORIGIN, STEP_SIZE, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_OK && ev.id == 0);
   do_tick(jb, log, &nl);
   STEP(do_get(jb, &m, COPY_ORIGIN, STEP_SIZE, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_OK && ev.id == 1);
   do_tick(jb, log, &nl);
   /* Interleaved late arrival, also scribbled after the put. */
   STEP(do_put(jb, &m, 3, COPY_ORIGIN, buf[3], log, &nl));
   scribble(buf[3]);
   STEP(do_get(jb, &m, COPY_ORIGIN, STEP_SIZE, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_OK && ev.id == 2);
   do_tick(jb, log, &nl);
   STEP(do_get(jb, &m, COPY_ORIGIN, STEP_SIZE, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_OK && ev.id == 3);
   do_tick(jb, log, &nl);
   STEP(do_get_another(jb, &m, COPY_ORIGIN, log, &nl, &ev));
   TASK_CHECK(ev.rc == JITTER_BUFFER_MISSING);
   /* The scribbles really happened, so the payloads checked above came
      from the buffer's own copies, not from our input buffers. */
   for (i = 0; i < COPY_NPKTS; i++)
      TASK_CHECK(memcmp(buf[i], copy_pkts[i].tag, PKT_LEN) != 0);
   jitter_buffer_destroy(jb);
   return 0;
}

/* Destroy-callback mode: identity objects handed to the buffer by pointer.
   The ledger records exactly one terminal owner per object: the caller
   (fetched), the sweep inside a later put (expired in the queue), or the
   reset/destroy path (still queued).  The callback never frees anything;
   double reclamation is reported through the ledger instead. */
#define OWN_N 11
enum { OWN_NEW = 0, OWN_QUEUED = 1, OWN_CALLER = 2, OWN_CB = 3 };

struct own_obj {
   int64_t start;
   uint32_t span;
   uint16_t seq;
   uint32_t ud;
   char tag[PKT_LEN];
   int owner;
   int cbn;
};

static struct own_obj own[OWN_N];
static int own_cb_total;
static int own_violations;

static void own_reset_ledger(void)
{
   static const struct {
      int64_t s; uint32_t sp; uint16_t q; uint32_t u; char t[PKT_LEN];
   } init[OWN_N] = {
      {   0, 10, 501, 5101, {'N','0','0','1'} }, /* 0: anchor */
      {  10, 10, 511, 5111, {'F','1','1','1'} }, /* 1: fragment @10 */
      {  10, 10, 512, 5122, {'F','1','2','2'} }, /* 2: fragment @10 */
      {  10, 10, 513, 5133, {'F','1','3','3'} }, /* 3: fragment @10 */
      {  20, 10, 521, 5211, {'M','1','2','1'} }, /* 4 */
      {  30, 10, 522, 5222, {'M','2','2','2'} }, /* 5 */
      {  40, 10, 531, 5311, {'K','1','3','1'} }, /* 6: left queued for reset */
      {  50, 10, 532, 5322, {'K','2','3','2'} }, /* 7: left queued for reset */
      {5000, 10, 541, 5411, {'S','1','4','1'} }, /* 8: new session origin */
      {5010, 10, 542, 5422, {'S','2','4','2'} }, /* 9: new session */
      {5020, 10, 551, 5511, {'T','1','5','1'} }, /* 10: left queued for destroy */
   };
   int i;
   for (i = 0; i < OWN_N; i++) {
      own[i].start = init[i].s;
      own[i].span = init[i].sp;
      own[i].seq = init[i].q;
      own[i].ud = init[i].u;
      memcpy(own[i].tag, init[i].t, PKT_LEN);
      own[i].owner = OWN_NEW;
      own[i].cbn = 0;
   }
   own_cb_total = 0;
   own_violations = 0;
}

static void own_destroy_cb(void *p)
{
   int i;
   for (i = 0; i < OWN_N; i++) {
      if ((void *)&own[i] == p) {
         /* Only a queued object may be reclaimed: a caller-owned or an
            already-reclaimed object here means double reclamation. */
         if (own[i].owner != OWN_QUEUED)
            own_violations++;
         own[i].owner = OWN_CB;
         own[i].cbn++;
         own_cb_total++;
         return;
      }
   }
   own_violations++; /* foreign pointer */
}

static int own_id_by_seq(uint16_t seq)
{
   int i;
   for (i = 0; i < OWN_N; i++)
      if (own[i].seq == seq)
         return i;
   return -1;
}

static int int_in_set(const int *set, int nset, int id)
{
   int i;
   for (i = 0; i < nset; i++)
      if (set[i] == id)
         return 1;
   return 0;
}

static int own_put(JitterBuffer *jb, int id)
{
   JitterBufferPacket in;
   TASK_CHECK(own[id].owner == OWN_NEW);
   in.data = (char *)&own[id]; /* zero-copy: the buffer keeps our pointer */
   in.len = PKT_LEN;
   in.timestamp = (spx_uint32_t)own[id].start;
   in.span = own[id].span;
   in.sequence = own[id].seq;
   in.user_data = own[id].ud;
   jitter_buffer_put(jb, &in);
   own[id].owner = OWN_QUEUED;
   return 0;
}

/* Every get in this script is an exact-start fetch (offset 0). */
static int own_get_one(JitterBuffer *jb, const int *set, int nset,
                       uint32_t want_cursor, int *got_id)
{
   JitterBufferPacket pkt;
   char buf[8];
   spx_int32_t off = -777;
   int rc, id;
   pkt.data = buf;
   pkt.len = PKT_LEN;
   pkt.sequence = 0xBEEF;
   pkt.user_data = 0xDEADBEEFu;
   rc = jitter_buffer_get(jb, &pkt, STEP_SIZE, &off);
   TASK_CHECK(rc == JITTER_BUFFER_OK);
   id = own_id_by_seq(pkt.sequence);
   TASK_CHECK(id >= 0);
   TASK_CHECK(int_in_set(set, nset, id));
   TASK_CHECK(pkt.data == (char *)&own[id]); /* zero-copy handoff */
   TASK_CHECK(pkt.timestamp == (spx_uint32_t)own[id].start);
   TASK_CHECK(pkt.span == own[id].span);
   TASK_CHECK(pkt.user_data == own[id].ud);
   TASK_CHECK(off == 0);
   TASK_CHECK((spx_uint32_t)jitter_buffer_get_pointer_timestamp(jb) == want_cursor);
   TASK_CHECK(own[id].owner == OWN_QUEUED);
   own[id].owner = OWN_CALLER; /* the caller reclaims fetched objects */
   *got_id = id;
   return 0;
}

static int own_another_ok(JitterBuffer *jb, const int *set, int nset, int *got_id)
{
   JitterBufferPacket pkt;
   char buf[8];
   int rc, id;
   pkt.data = buf;
   pkt.len = PKT_LEN;
   pkt.sequence = 0xBEEF;
   pkt.user_data = 0xDEADBEEFu;
   rc = jitter_buffer_get_another(jb, &pkt);
   TASK_CHECK(rc == JITTER_BUFFER_OK);
   id = own_id_by_seq(pkt.sequence);
   TASK_CHECK(id >= 0);
   TASK_CHECK(int_in_set(set, nset, id));
   TASK_CHECK(pkt.data == (char *)&own[id]);
   TASK_CHECK(own[id].owner == OWN_QUEUED);
   own[id].owner = OWN_CALLER;
   *got_id = id;
   return 0;
}

static int own_another_miss(JitterBuffer *jb)
{
   JitterBufferPacket pkt;
   char buf[8];
   int rc;
   pkt.data = buf;
   pkt.len = PKT_LEN;
   pkt.sequence = 0xBEEF;
   pkt.user_data = 0xDEADBEEFu;
   rc = jitter_buffer_get_another(jb, &pkt);
   TASK_CHECK(rc == JITTER_BUFFER_MISSING);
   TASK_CHECK(pkt.data == NULL);
   TASK_CHECK(pkt.len == 0);
   TASK_CHECK(pkt.span == 0);
   return 0;
}

static int case_own_destroy(void)
{
   JitterBuffer *jb;
   int got0, got1, got2, leftover, i;
   int frags[3] = {1, 2, 3};
   int frags_left[2];
   int one[1];
   own_reset_ledger();
   jb = jitter_buffer_init(STEP_SIZE);
   TASK_CHECK(jb != NULL);
   jitter_buffer_ctl(jb, JITTER_BUFFER_SET_DESTROY_CALLBACK, (void *)own_destroy_cb);
   STEP(do_updelay(jb, NULL, NULL));
   /* Preload, arrival order scrambled: two fragments, the anchor, the next
      frame, then the last fragment. */
   STEP(own_put(jb, 2));
   STEP(own_put(jb, 0));
   STEP(own_put(jb, 4));
   STEP(own_put(jb, 3));
   STEP(own_put(jb, 1));
   /* The anchor packet comes out first and becomes caller-owned. */
   one[0] = 0;
   STEP(own_get_one(jb, one, 1, 10, &got0));
   TASK_CHECK(got0 == 0);
   jitter_buffer_tick(jb);
   /* Fetch two of the three fragments; the caller owns each fetched one. */
   STEP(own_get_one(jb, frags, 3, 20, &got1));
   jitter_buffer_tick(jb);
   {
      int j, n = 0;
      for (j = 0; j < 3; j++)
         if (frags[j] != got1)
            frags_left[n++] = frags[j];
      TASK_CHECK(n == 2);
   }
   STEP(own_another_ok(jb, frags_left, 2, &got2));
   TASK_CHECK(got2 != got1);
   TASK_CHECK((spx_uint32_t)jitter_buffer_get_pointer_timestamp(jb) == 20);
   /* The leftover fragment expires exactly at the cursor (end == 20 ==
      cursor).  This put must sweep it: exactly one callback, for exactly
      that object, before the put returns. */
   leftover = 6 - got1 - got2; /* fragment ids are 1, 2 and 3 */
   TASK_CHECK(leftover >= 1 && leftover <= 3);
   STEP(own_put(jb, 5));
   TASK_CHECK(own[leftover].owner == OWN_CB);
   TASK_CHECK(own[leftover].cbn == 1);
   TASK_CHECK(own_cb_total == 1);
   /* Play out the two queued frames; each becomes caller-owned. */
   one[0] = 4;
   STEP(own_get_one(jb, one, 1, 30, &got0));
   TASK_CHECK(got0 == 4);
   jitter_buffer_tick(jb);
   one[0] = 5;
   STEP(own_get_one(jb, one, 1, 40, &got0));
   TASK_CHECK(got0 == 5);
   jitter_buffer_tick(jb);
   STEP(own_another_miss(jb));
   TASK_CHECK((spx_uint32_t)jitter_buffer_get_pointer_timestamp(jb) == 40);
   /* Two packets stay queued and unexpired; no sweep may touch them. */
   STEP(own_put(jb, 6));
   STEP(own_put(jb, 7));
   TASK_CHECK(own_cb_total == 1);
   TASK_CHECK(own[6].owner == OWN_QUEUED);
   TASK_CHECK(own[7].owner == OWN_QUEUED);
   /* reset() reclaims whatever is still queued, exactly once each. */
   jitter_buffer_reset(jb);
   TASK_CHECK(own[6].owner == OWN_CB && own[6].cbn == 1);
   TASK_CHECK(own[7].owner == OWN_CB && own[7].cbn == 1);
   TASK_CHECK(own_cb_total == 3);
   /* Everything already handed out is untouched by the reset. */
   TASK_CHECK(own[0].owner == OWN_CALLER);
   TASK_CHECK(own[4].owner == OWN_CALLER);
   TASK_CHECK(own[5].owner == OWN_CALLER);
   /* Continue on a new time origin: the old session's fragments must not
      leak into the new one. */
   STEP(own_put(jb, 8));
   STEP(own_put(jb, 9));
   one[0] = 8;
   STEP(own_get_one(jb, one, 1, 5010, &got0));
   TASK_CHECK(got0 == 8);
   jitter_buffer_tick(jb);
   one[0] = 9;
   STEP(own_get_one(jb, one, 1, 5020, &got0));
   TASK_CHECK(got0 == 9);
   jitter_buffer_tick(jb);
   /* One more packet stays queued; destroy() reclaims it. */
   STEP(own_put(jb, 10));
   TASK_CHECK(own_cb_total == 3);
   jitter_buffer_destroy(jb);
   TASK_CHECK(own[10].owner == OWN_CB && own[10].cbn == 1);
   TASK_CHECK(own_cb_total == 4);
   /* Final audit: every object ended with exactly one owner. */
   {
      int ncaller = 0, ncb = 0, cbsum = 0;
      for (i = 0; i < OWN_N; i++) {
         TASK_CHECK(own[i].owner == OWN_CALLER || own[i].owner == OWN_CB);
         TASK_CHECK(own[i].cbn == (own[i].owner == OWN_CB ? 1 : 0));
         if (own[i].owner == OWN_CALLER)
            ncaller++;
         else
            ncb++;
         cbsum += own[i].cbn;
      }
      TASK_CHECK(ncaller == 7); /* anchor, 2 fragments, M1, M2, S1, S2 */
      TASK_CHECK(ncb == 4);     /* swept fragment, K1, K2, T1 */
      TASK_CHECK(cbsum == 4 && own_cb_total == 4);
      TASK_CHECK(own_violations == 0);
   }
   return 0;
}

const struct task_case task_cases[] = {
   {"wrap_timeline", "plain_origin_model", case_wrap_plain},
   {"wrap_timeline", "shifted_origins_invariance", case_wrap_shifted},
   {"fragment_cursor", "mixed_playout", case_fragment_mixed},
   {"fragment_cursor", "playout_gap", case_fragment_gap},
   {"ownership", "copy_mode_payload", case_own_copy},
   {"ownership", "destroy_mode_ledger", case_own_destroy},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
