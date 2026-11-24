/*
 * session_model.c -- Text driven benign CoAP session scheduling model.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This file is part of the CoAP library libcoap. Please see README for terms
 * of use.
 *
 * The executor drives the real libcoap retransmission queue through the
 * normal internal API (coap_wait_ack_at / coap_pop_next / coap_retransmit_at /
 * coap_cancel_all_messages / coap_cancel_session_messages / coap_dispatch)
 * with an explicit logical tick clock, while an independent absolute-time
 * model predicts every queue node, retry stage and response/nack callback.
 * Each CASE runs in its own forked worker sharing no state with its siblings;
 * the parent only merges deterministically ordered state and edge coverage.
 */

#include "tests/oss-fuzz/session_model.h"
#include "coap3/coap_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---- Plan limits (mirror session_contract.md) ---- */

#define MODEL_MAX_INPUT      65536u
#define MODEL_MAX_CASES      16
#define MODEL_MAX_SESSIONS   4
#define MODEL_MAX_SENDS      4   /* per session */
#define MODEL_MAX_EVENTS     256 /* total event lines in one plan */
#define MODEL_MAX_ID_LEN     16
#define MODEL_NUM_LIMIT      1000000L
#define MODEL_IPC_LIMIT      (16u * 1024u * 1024u)

#define MODEL_OP_SEND        0
#define MODEL_OP_ACK         1
#define MODEL_OP_CANCEL      2
#define MODEL_OP_CLOSE       3
#define MODEL_OP_DEADLINE    4
#define MODEL_OP_RETRY       5
#define MODEL_OP_TIMEOUT     6

/* ---- Parsed plan ------------------------------------------------------- */

enum model_evkind {
  EV_SEND = 1,
  EV_ACK,
  EV_CANCEL,
  EV_CLOSE
};

typedef struct model_event {
  int kind;
  long tick;
  int sess;
  long mid;
  long token;
  long timeout;
} model_event_t;

typedef struct model_send {
  long tick;
  long mid;
  long token;
  long timeout;
  int acked;       /* ACK declaration seen */
  int cancelled;   /* CANCEL declaration seen */
} model_send_t;

typedef struct model_case {
  char id[MODEL_MAX_ID_LEN + 1];
  int sessions;
  long deadline[MODEL_MAX_SESSIONS];
  int used[MODEL_MAX_SESSIONS];          /* 1 once SESSION was declared */
  int send_count[MODEL_MAX_SESSIONS];
  model_send_t sends[MODEL_MAX_SESSIONS][MODEL_MAX_SENDS];
  model_event_t events[MODEL_MAX_EVENTS];
  int nevents;
} model_case_t;

typedef struct model_plan {
  const uint8_t *raw;
  size_t raw_size;
  model_case_t cases[MODEL_MAX_CASES];
  int ncases;
  int total_events;
} model_plan_t;

/* ---- Runtime model ----------------------------------------------------- */

enum model_req_status {
  REQ_PENDING = 1,
  REQ_ACKED,
  REQ_CANCELED,
  REQ_TIMEOUT
};

typedef struct model_req {
  int sess;
  long mid;
  long token;
  long timeout;
  long send_tick;
  long due;                 /* independent absolute due tick */
  int retry;                /* 0..max_retransmit */
  int status;
  int seq;                  /* stable insertion order */
  int cancel_via_session;   /* canceled through coap_cancel_session_messages */
} model_req_t;

typedef struct model_state8 {
  unsigned u, p0, p1, p2, a, c, t, z;
} model_state8_t;

typedef struct model_edge {
  model_state8_t from;
  int op;
  model_state8_t to;
} model_edge_t;

#define MODEL_MAX_REQS (MODEL_MAX_SESSIONS * MODEL_MAX_SENDS)
#define MODEL_MAX_EDGES (MODEL_MAX_EVENTS + MODEL_MAX_EVENTS)
/*
 * A single CASE can hold up to MODEL_MAX_EVENTS event lines. The longest
 * line (SEND with all fields at their limits) is 34 bytes including LF, so a
 * 12KiB witness buffer covers the largest legal plan with ample headroom
 * while every IPC frame still fits in a uint16 length.
 */
#define MODEL_WITNESS_MAX 12288

/* Per worker runtime. */
typedef struct model_runtime {
  const model_plan_t *plan;
  const model_case_t *cp;
  coap_context_t *ctx;
  coap_session_t *sess[MODEL_MAX_SESSIONS];
  model_req_t req[MODEL_MAX_REQS];
  int nreq;
  int closed[MODEL_MAX_SESSIONS];
  /* observed state tuples and edges */
  model_state8_t states[64];
  int nstates;
  model_edge_t edges[MODEL_MAX_EDGES];
  int nedges;
  /* callback accounting */
  long resp_hits[MODEL_MAX_REQS];
  long nack_hits[MODEL_MAX_REQS];
  int nack_reason[MODEL_MAX_REQS];
  long unknown_resp;       /* callbacks the model did not predict */
  long unknown_nack;
  int mismatch;
  char mismatch_field[16];
  long mismatch_tick;
  int mismatch_sess;
  long mismatch_mid;
  long mismatch_expected;
  long mismatch_actual;
} model_runtime_t;

/* ---- IPC frames -------------------------------------------------------- */

#define MODEL_FRAME_RESULT 'R'
#define MODEL_FRAME_STATE  'S'
#define MODEL_FRAME_EDGE   'E'
#define MODEL_FRAME_CASE   'C'
#define MODEL_FRAME_WITNESS 'W'
#define MODEL_FRAME_END    'X'

typedef struct model_frame {
  unsigned char type;
  const unsigned char *payload;
  uint16_t len;
} model_frame_t;

/* Forward declarations. */
static int model_ipc_emit(model_runtime_t *rt, int case_idx, int out_fd);
static int model_ipc_emit_mismatch(model_runtime_t *rt, int out_fd);
static int model_worker_run(const model_plan_t *plan, int case_idx, int out_fd);


/* ---- Small helpers ----------------------------------------------------- */

static int
model_is_id_char(int c) {
  return isalnum(c) || c == '_';
}

/* Strict non-negative decimal parser: no leading zeroes (except "0"),
 * enforces @p max. Returns 1 on success. */
static int
model_parse_num(const char *s, size_t len, long max, long *out) {
  size_t i;
  long v = 0;

  if (len == 0 || len > 10)
    return 0;
  if (len > 1 && s[0] == '0')
    return 0;
  for (i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9')
      return 0;
    v = v * 10 + (s[i] - '0');
    if (v > max)
      return 0;
  }
  *out = v;
  return 1;
}

static int
model_state_equal(const model_state8_t *a, const model_state8_t *b) {
  return a->u == b->u && a->p0 == b->p0 && a->p1 == b->p1 &&
         a->p2 == b->p2 && a->a == b->a && a->c == b->c &&
         a->t == b->t && a->z == b->z;
}

static int
model_state_cmp(const model_state8_t *a, const model_state8_t *b) {
  const unsigned va[8] = { a->u, a->p0, a->p1, a->p2, a->a, a->c, a->t, a->z };
  const unsigned vb[8] = { b->u, b->p0, b->p1, b->p2, b->a, b->c, b->t, b->z };
  int i;

  for (i = 0; i < 8; i++) {
    if (va[i] != vb[i])
      return va[i] < vb[i] ? -1 : 1;
  }
  return 0;
}

static const char *
model_op_name(int op) {
  switch (op) {
  case MODEL_OP_SEND:     return "SEND";
  case MODEL_OP_ACK:      return "ACK";
  case MODEL_OP_CANCEL:   return "CANCEL";
  case MODEL_OP_CLOSE:    return "CLOSE";
  case MODEL_OP_DEADLINE: return "DEADLINE";
  case MODEL_OP_RETRY:    return "RETRY";
  case MODEL_OP_TIMEOUT:  return "TIMEOUT";
  default:                return "?";
  }
}

/* ---- Parsing and whole-input validation -------------------------------- */

typedef struct model_line {
  const char *p;
  size_t len;
} model_line_t;

static int
model_split_ws(const model_line_t *line, model_line_t *tok, int max_tok) {
  size_t i = 0;
  int n = 0;

  while (i < line->len) {
    while (i < line->len && line->p[i] == ' ')
      i++;
    if (i >= line->len)
      break;
    if (n >= max_tok)
      return -1;
    tok[n].p = line->p + i;
    tok[n].len = 0;
    while (i < line->len && line->p[i] != ' ') {
      tok[n].len++;
      i++;
    }
    n++;
  }
  return n;
}

static int
model_tok_eq(const model_line_t *t, const char *s) {
  size_t n = strlen(s);

  return t->len == n && memcmp(t->p, s, n) == 0;
}

static int
model_find_case_id(const model_plan_t *plan, const char *id, size_t len) {
  int i;

  for (i = 0; i < plan->ncases; i++) {
    if (strlen(plan->cases[i].id) == len &&
        memcmp(plan->cases[i].id, id, len) == 0)
      return i;
  }
  return -1;
}

static model_send_t *
model_find_send(model_case_t *cp, int sess, long mid) {
  int i;

  for (i = 0; i < cp->send_count[sess]; i++) {
    if (cp->sends[sess][i].mid == mid)
      return &cp->sends[sess][i];
  }
  return NULL;
}

static model_send_t *
model_find_send_token(model_case_t *cp, int sess, long token) {
  int i;

  for (i = 0; i < cp->send_count[sess]; i++) {
    if (cp->sends[sess][i].token == token)
      return &cp->sends[sess][i];
  }
  return NULL;
}

/* Validates the complete input before any context or worker is created.
 * The text order of event lines is unrestricted, so reference checks run in
 * a second pass after every SEND of a case has been recorded. */
static int
model_parse(const uint8_t *data, size_t size, model_plan_t *plan) {
  model_line_t lines[MODEL_MAX_EVENTS + MODEL_MAX_CASES * 8 + 8];
  typedef struct model_raw_event {
    int case_idx;
    model_event_t ev;
  } model_raw_event_t;
  model_raw_event_t rawev[MODEL_MAX_EVENTS];
  int nrawev = 0;
  int nlines = 0;
  size_t pos = 0;
  int li, ci;
  int in_case = 0;
  int declarations_open = 1;
  model_case_t *cp = NULL;
  long total_events = 0;
  long sends_per_case[MODEL_MAX_CASES];
  int used_tmp[MODEL_MAX_CASES][MODEL_MAX_SESSIONS];
  long deadline_tmp[MODEL_MAX_CASES][MODEL_MAX_SESSIONS];

  memset(plan, 0, sizeof(*plan));
  memset(sends_per_case, 0, sizeof(sends_per_case));
  memset(used_tmp, 0, sizeof(used_tmp));
  memset(deadline_tmp, 0, sizeof(deadline_tmp));
  plan->raw = data;
  plan->raw_size = size;

  if (size == 0 || size > MODEL_MAX_INPUT)
    return 0;
  /* Every byte must be printable ASCII text or LF; no CR or tabs. */
  for (pos = 0; pos < size; pos++) {
    unsigned char c = data[pos];
    if (c == '\n')
      continue;
    if (c < 0x20 || c > 0x7e)
      return 0;
  }

  pos = 0;
  while (pos < size) {
    size_t start = pos;
    const char *p2;
    size_t len;

    while (pos < size && data[pos] != '\n')
      pos++;
    p2 = (const char *)data + start;
    len = (size_t)(pos - start);
    if (pos < size)
      pos++;
    if (nlines >= (int)(sizeof(lines) / sizeof(lines[0])))
      return 0;
    lines[nlines].p = p2;
    lines[nlines].len = len;
    nlines++;
  }
  if (nlines < 2)
    return 0;

  /* Header. */
  {
    model_line_t tok[3];
    int n = model_split_ws(&lines[0], tok, 3);
    if (n != 2 || !model_tok_eq(&tok[0], "COAP_MODEL_PLAN") ||
        !model_tok_eq(&tok[1], "1"))
      return 0;
  }

  /* Pass 1: structure, declarations and per-line syntax. */
  for (li = 1; li < nlines; li++) {
    model_line_t tok[7];
    int n = model_split_ws(&lines[li], tok, 7);
    long a, b, c, d, e;
    model_event_t ev;

    if (n <= 0)
      return 0;
    memset(&ev, 0, sizeof(ev));

    if (model_tok_eq(&tok[0], "CASE")) {
      size_t idlen;
      int k;

      if (n != 2)
        return 0;
      idlen = tok[1].len;
      if (idlen < 1 || idlen > MODEL_MAX_ID_LEN)
        return 0;
      for (k = 0; k < (int)idlen; k++) {
        if (!model_is_id_char((unsigned char)tok[1].p[k]))
          return 0;
      }
      if (in_case)
        return 0;       /* previous CASE missing END */
      if (plan->ncases >= MODEL_MAX_CASES)
        return 0;
      if (model_find_case_id(plan, tok[1].p, idlen) >= 0)
        return 0;
      cp = &plan->cases[plan->ncases];
      memset(cp, 0, sizeof(*cp));
      memcpy(cp->id, tok[1].p, idlen);
      cp->id[idlen] = 0;
      plan->ncases++;
      in_case = 1;
      declarations_open = 1;
      continue;
    }

    if (model_tok_eq(&tok[0], "END")) {
      if (n != 1 || !in_case)
        return 0;
      if (cp->sessions < 1)
        return 0;
      in_case = 0;
      cp = NULL;
      continue;
    }

    if (!in_case)
      return 0;

    if (model_tok_eq(&tok[0], "SESSION")) {
      if (!declarations_open || n != 3)
        return 0;
      if (!model_parse_num(tok[1].p, tok[1].len, MODEL_MAX_SESSIONS - 1, &a) ||
          !model_parse_num(tok[2].p, tok[2].len, MODEL_NUM_LIMIT, &b) ||
          b < 1)
        return 0;
      if (used_tmp[plan->ncases - 1][(int)a])
        return 0;
      used_tmp[plan->ncases - 1][(int)a] = 1;
      deadline_tmp[plan->ncases - 1][(int)a] = b;
      cp->sessions++;
      continue;
    }

    /* Any non-SESSION line closes the declaration window. */
    declarations_open = 0;
    if (total_events >= MODEL_MAX_EVENTS || nrawev >= MODEL_MAX_EVENTS)
      return 0;

    if (model_tok_eq(&tok[0], "SEND")) {
      if (n != 6)
        return 0;
      if (!model_parse_num(tok[1].p, tok[1].len, MODEL_NUM_LIMIT, &a) ||
          !model_parse_num(tok[2].p, tok[2].len, MODEL_MAX_SESSIONS - 1, &b) ||
          !model_parse_num(tok[3].p, tok[3].len, 65535, &c) ||
          !model_parse_num(tok[4].p, tok[4].len, 255, &d) ||
          !model_parse_num(tok[5].p, tok[5].len, 1000, &e))
        return 0;
      if (c < 1 || e < 1 || !used_tmp[plan->ncases - 1][(int)b])
        return 0;
      if (a >= deadline_tmp[plan->ncases - 1][(int)b])
        return 0;
      ev.kind = EV_SEND;
      ev.tick = a; ev.sess = (int)b; ev.mid = c;
      ev.token = d; ev.timeout = e;
    } else if (model_tok_eq(&tok[0], "ACK")) {
      if (n != 4)
        return 0;
      if (!model_parse_num(tok[1].p, tok[1].len, MODEL_NUM_LIMIT, &a) ||
          !model_parse_num(tok[2].p, tok[2].len, MODEL_MAX_SESSIONS - 1, &b) ||
          !model_parse_num(tok[3].p, tok[3].len, 65535, &c))
        return 0;
      if (c < 1 || !used_tmp[plan->ncases - 1][(int)b])
        return 0;
      ev.kind = EV_ACK;
      ev.tick = a; ev.sess = (int)b; ev.mid = c;
    } else if (model_tok_eq(&tok[0], "CANCEL")) {
      if (n != 4)
        return 0;
      if (!model_parse_num(tok[1].p, tok[1].len, MODEL_NUM_LIMIT, &a) ||
          !model_parse_num(tok[2].p, tok[2].len, MODEL_MAX_SESSIONS - 1, &b) ||
          !model_parse_num(tok[3].p, tok[3].len, 255, &c))
        return 0;
      if (!used_tmp[plan->ncases - 1][(int)b])
        return 0;
      ev.kind = EV_CANCEL;
      ev.tick = a; ev.sess = (int)b; ev.token = c;
    } else if (model_tok_eq(&tok[0], "CLOSE")) {
      if (n != 3)
        return 0;
      if (!model_parse_num(tok[1].p, tok[1].len, MODEL_NUM_LIMIT, &a) ||
          !model_parse_num(tok[2].p, tok[2].len, MODEL_MAX_SESSIONS - 1, &b))
        return 0;
      if (!used_tmp[plan->ncases - 1][(int)b])
        return 0;
      ev.kind = EV_CLOSE;
      ev.tick = a; ev.sess = (int)b;
    } else {
      return 0; /* unknown line */
    }
    rawev[nrawev].case_idx = plan->ncases - 1;
    rawev[nrawev].ev = ev;
    nrawev++;
    total_events++;
  }

  if (in_case || plan->ncases == 0)
    return 0;

  /* Pass 2: populate cases and validate all cross references. */
  for (ci = 0; ci < plan->ncases; ci++) {
    model_case_t *c2 = &plan->cases[ci];
    int s;

    for (s = 0; s < MODEL_MAX_SESSIONS; s++) {
      c2->used[s] = used_tmp[ci][s];
      c2->deadline[s] = deadline_tmp[ci][s];
    }
    c2->sessions = 0;
    for (s = 0; s < MODEL_MAX_SESSIONS; s++) {
      if (c2->used[s])
        c2->sessions++;
    }
  }

  /* SEND declarations must exist before ACK/CANCEL references can be
   * validated regardless of text order, so register every SEND first. */
  for (int k = 0; k < nrawev; k++) {
    int case_idx = rawev[k].case_idx;
    model_case_t *c2 = &plan->cases[case_idx];
    const model_event_t *ev2 = &rawev[k].ev;

    if (ev2->kind != EV_SEND)
      continue;
    {
      model_send_t *sd;
      if (c2->send_count[ev2->sess] >= MODEL_MAX_SENDS)
        return 0;
      if (model_find_send(c2, ev2->sess, ev2->mid) ||
          model_find_send_token(c2, ev2->sess, ev2->token))
        return 0;
      sd = &c2->sends[ev2->sess][c2->send_count[ev2->sess]++];
      sd->tick = ev2->tick;
      sd->mid = ev2->mid;
      sd->token = ev2->token;
      sd->timeout = ev2->timeout;
      sends_per_case[case_idx]++;
    }
  }

  for (int k = 0; k < nrawev; k++) {
    int case_idx = rawev[k].case_idx;
    model_case_t *c2 = &plan->cases[case_idx];
    const model_event_t *ev2 = &rawev[k].ev;

    switch (ev2->kind) {
    case EV_SEND:
      break;
    case EV_ACK: {
      model_send_t *sd = model_find_send(c2, ev2->sess, ev2->mid);
      if (!sd || sd->acked || ev2->tick < sd->tick)
        return 0;
      sd->acked = 1;
      break;
    }
    case EV_CANCEL: {
      model_send_t *sd = model_find_send_token(c2, ev2->sess, ev2->token);
      if (!sd || sd->cancelled || ev2->tick < sd->tick)
        return 0;
      sd->cancelled = 1;
      break;
    }
    case EV_CLOSE: {
      int m;
      for (m = 0; m < c2->nevents; m++) {
        if (c2->events[m].kind == EV_CLOSE &&
            c2->events[m].sess == ev2->sess)
          return 0;
      }
      break;
    }
    default:
      return 0;
    }
    c2->events[c2->nevents++] = *ev2;
  }

  for (ci = 0; ci < plan->ncases; ci++) {
    int total = 0;
    for (int s2 = 0; s2 < MODEL_MAX_SESSIONS; s2++)
      total += plan->cases[ci].send_count[s2];
    if (total != (int)sends_per_case[ci])
      return 0;
  }
  plan->total_events = (int)total_events;
  return 1;
}

/* ---- Scheduled step list ----------------------------------------------- */

typedef struct model_step {
  long tick;
  int prio;          /* same-tick priority, smaller first */
  int sess;
  long key;          /* MID or TOKEN for stable ordering */
  int kind;          /* event kind or MODEL_OP_DEADLINE */
  const model_event_t *ev;
} model_step_t;

static int
model_step_cmp(const void *pa, const void *pb) {
  const model_step_t *a = (const model_step_t *)pa;
  const model_step_t *b = (const model_step_t *)pb;

  if (a->tick != b->tick)
    return a->tick < b->tick ? -1 : 1;
  if (a->prio != b->prio)
    return a->prio - b->prio;
  if (a->sess != b->sess)
    return a->sess - b->sess;
  if (a->key != b->key)
    return a->key < b->key ? -1 : 1;
  return 0;
}

/* ---- Real session construction ----------------------------------------- */

/*
 * Deterministic 100% send loss layer. The contract sets packet_loss to 100%
 * and no socket data is ever received; this write layer reports every
 * datagram as consumed and dropped without touching a socket fd, which keeps
 * the behavior identical in sandboxed builds where loopback connect(2) is
 * forbidden. All queue, retry and callback logic stays in the library.
 */
static ssize_t
model_drop_write(coap_session_t *session, const uint8_t *data, size_t datalen) {
  (void)session;
  (void)data;
  return (ssize_t)datalen;
}

/*
 * The primary path is the public coap_new_client_session() against loopback.
 * Sandboxed regression environments may forbid socket(2)/connect(2); in that
 * case we build an equivalent detached UDP client session through the same
 * internal initialization coap_make_session() performs, with no socket fd.
 * Every queue operation still goes through the normal library code.
 */
static coap_session_t *
model_new_session(coap_context_t *ctx, int idx, const coap_address_t *remote) {
  coap_session_t *s;

  s = coap_new_client_session(ctx, NULL, remote, COAP_PROTO_UDP);
  if (s) {
    s->max_retransmit = 2;
    /* Up to MODEL_MAX_SENDS concurrent CONs per session must stay on the
     * shared sendqueue; never let NSTART push them into a delayqueue. */
    s->nstart = MODEL_MAX_SENDS;
    s->state = COAP_SESSION_STATE_ESTABLISHED;
    s->sock.lfunc[COAP_LAYER_SESSION].l_write = model_drop_write;
    return s;
  }

  s = (coap_session_t *)coap_malloc_type(COAP_SESSION, sizeof(coap_session_t));
  if (!s)
    return NULL;
  memset(s, 0, sizeof(*s));
  s->proto = COAP_PROTO_UDP;
  s->type = COAP_SESSION_TYPE_CLIENT;
  s->state = COAP_SESSION_STATE_ESTABLISHED;
  s->ref = 1;
  s->context = ctx;
  s->client_initiated = 1;
  coap_address_init(&s->addr_info.local);
  coap_address_init(&s->addr_info.remote);
  coap_address_copy(&s->addr_info.remote, remote);
  coap_address_copy(&s->local_reconnect, &s->addr_info.local);
  s->addr_hash.remote = *remote;
  s->addr_hash.lport = (uint16_t)(ntohs(remote->addr.sin.sin_port) +
                                  (uint16_t)idx);
  s->addr_hash.proto = COAP_PROTO_UDP;
  coap_session_set_mtu(s, COAP_DEFAULT_MTU);
  s->block_mode = ctx->block_mode;
  s->rl_ticks_per_packet = ctx->rl_ticks_per_packet;
  s->ack_timeout = COAP_DEFAULT_ACK_TIMEOUT;
  s->ack_random_factor = COAP_DEFAULT_ACK_RANDOM_FACTOR;
  s->max_retransmit = 2;
  s->nstart = MODEL_MAX_SENDS;
  s->default_leisure = COAP_DEFAULT_DEFAULT_LEISURE;
  s->probing_rate = COAP_DEFAULT_PROBING_RATE;
  s->dtls_event = -1;
  s->last_ping_mid = COAP_INVALID_MID;
  s->last_resp_mid = COAP_INVALID_MID;
  s->last_con_handler_res = COAP_RESPONSE_OK;
  s->max_token_size = ctx->max_token_size;
  s->max_token_checked = COAP_EXT_T_CHECKED;
  coap_prng_lkd((unsigned char *)&s->tx_mid, sizeof(s->tx_mid));
  coap_prng_lkd((unsigned char *)&s->tx_rtag, sizeof(s->tx_rtag));
  s->sock.fd = COAP_INVALID_SOCKET;
  s->sock.flags = COAP_SOCKET_NOT_EMPTY | COAP_SOCKET_WANT_READ;
  s->sock.session = s;
  memcpy(&s->sock.lfunc, coap_layers_coap[COAP_PROTO_UDP],
         sizeof(s->sock.lfunc));
  /* No usable socket in this environment: drop deterministically. */
  s->sock.lfunc[COAP_LAYER_SESSION].l_write = model_drop_write;
  SESSIONS_ADD(ctx->sessions, s);
  return s;
}

/* ---- Callback recording ------------------------------------------------ */

static model_runtime_t *g_rt;

static model_req_t *
model_rt_find(model_runtime_t *rt, coap_session_t *session, coap_mid_t mid) {
  int i;

  for (i = 0; i < rt->nreq; i++) {
    if (rt->sess[rt->req[i].sess] == session && rt->req[i].mid == mid)
      return &rt->req[i];
  }
  return NULL;
}

static coap_response_t
model_response_handler(coap_session_t *session, const coap_pdu_t *sent,
                       const coap_pdu_t *received, const coap_mid_t mid) {
  model_runtime_t *rt = g_rt;
  model_req_t *r;

  (void)sent;
  if (!rt)
    return COAP_RESPONSE_OK;
  r = model_rt_find(rt, session, mid);
  if (r && received && received->type == COAP_MESSAGE_ACK &&
      received->code == COAP_RESPONSE_CODE_CONTENT) {
    rt->resp_hits[(int)(r - rt->req)]++;
  } else {
    rt->unknown_resp++;
  }
  /* OK stops the library from sending an automatic RST for this ACK. */
  return COAP_RESPONSE_OK;
}

static void
model_nack_handler(coap_session_t *session, const coap_pdu_t *sent,
                   const coap_nack_reason_t reason, const coap_mid_t mid) {
  model_runtime_t *rt = g_rt;
  model_req_t *r;

  (void)session;
  if (!rt || !sent) {
    if (rt)
      rt->unknown_nack++;
    return;
  }
  r = model_rt_find(rt, session, mid);
  if (r) {
    int idx = (int)(r - rt->req);
    rt->nack_hits[idx]++;
    rt->nack_reason[idx] = (int)reason;
  } else {
    rt->unknown_nack++;
  }
}

/* ---- Model state ------------------------------------------------------- */

static void
model_state_of(model_runtime_t *rt, int sess, model_state8_t *out) {
  int i;

  memset(out, 0, sizeof(*out));
  for (i = 0; i < rt->nreq; i++) {
    model_req_t *r = &rt->req[i];
    if (r->sess != sess)
      continue;
    if (r->status == REQ_PENDING) {
      if (r->retry == 0)
        out->p0++;
      else if (r->retry == 1)
        out->p1++;
      else
        out->p2++;
    } else if (r->status == REQ_ACKED) {
      out->a++;
    } else if (r->status == REQ_CANCELED) {
      out->c++;
    } else if (r->status == REQ_TIMEOUT) {
      out->t++;
    }
  }
  /* u = declared but not yet sent; a model request exists once sent. */
  out->u = (unsigned)rt->cp->send_count[sess];
  for (i = 0; i < rt->nreq; i++) {
    if (rt->req[i].sess == sess)
      out->u--;
  }
  out->z = rt->closed[sess] ? 1u : 0u;
}

static void
model_record_state(model_runtime_t *rt, const model_state8_t *st) {
  int i;

  for (i = 0; i < rt->nstates; i++) {
    if (model_state_equal(&rt->states[i], st))
      return;
  }
  if (rt->nstates < (int)(sizeof(rt->states) / sizeof(rt->states[0])))
    rt->states[rt->nstates++] = *st;
}

/* Legal transition predicate for the independent model's graph. Several
 * requests may sit in different retry stages at once, so a single RETRY/ACK/
 * CANCEL may consume a node from any non-empty stage: every one-node move is
 * enumerated rather than assuming a fixed stage preference. */
static int
model_state_one_move(const model_state8_t *from, model_state8_t *to,
                     int op) {
  model_state8_t e = *from;

  switch (op) {
  case MODEL_OP_SEND:
    if (e.u == 0)
      return 0;
    e.u--;
    e.p0++;
    break;
  case MODEL_OP_RETRY:
    if (e.p0) {
      e.p0--;
      e.p1++;
    } else if (e.p1) {
      e.p1--;
      e.p2++;
    } else {
      return 0;
    }
    break;
  case MODEL_OP_TIMEOUT:
    if (e.p2 == 0)
      return 0;
    e.p2--;
    e.t++;
    break;
  case MODEL_OP_ACK:
    if (e.p0) {
      e.p0--;
    } else if (e.p1) {
      e.p1--;
    } else if (e.p2) {
      e.p2--;
    } else {
      return 0;
    }
    e.a++;
    break;
  case MODEL_OP_CANCEL:
    if (e.p0) {
      e.p0--;
    } else if (e.p1) {
      e.p1--;
    } else if (e.p2) {
      e.p2--;
    } else {
      return 0;
    }
    e.c++;
    break;
  case MODEL_OP_CLOSE:
  case MODEL_OP_DEADLINE:
    e.c += e.p0 + e.p1 + e.p2;
    e.p0 = e.p1 = e.p2 = 0;
    e.z = 1;
    break;
  default:
    return 0;
  }
  *to = e;
  return 1;
}

static int
model_legal_edge(int op, const model_state8_t *from,
                 const model_state8_t *to, int sess, model_runtime_t *rt) {
  model_state8_t cand;

  (void)sess;
  (void)rt;

  if (op == MODEL_OP_CLOSE || op == MODEL_OP_DEADLINE) {
    if (!model_state_one_move(from, &cand, op))
      return 0;
    return model_state_equal(&cand, to);
  }

  /* Enumerate consuming one node from every non-empty pending stage. */
  if (op == MODEL_OP_RETRY) {
    if (from->p0) {
      cand = *from; cand.p0--; cand.p1++;
      if (model_state_equal(&cand, to))
        return 1;
    }
    if (from->p1) {
      cand = *from; cand.p1--; cand.p2++;
      if (model_state_equal(&cand, to))
        return 1;
    }
    return 0;
  }
  if (op == MODEL_OP_ACK || op == MODEL_OP_CANCEL) {
    if (from->p0) {
      cand = *from; cand.p0--;
      if (op == MODEL_OP_ACK) cand.a++; else cand.c++;
      if (model_state_equal(&cand, to))
        return 1;
    }
    if (from->p1) {
      cand = *from; cand.p1--;
      if (op == MODEL_OP_ACK) cand.a++; else cand.c++;
      if (model_state_equal(&cand, to))
        return 1;
    }
    if (from->p2) {
      cand = *from; cand.p2--;
      if (op == MODEL_OP_ACK) cand.a++; else cand.c++;
      if (model_state_equal(&cand, to))
        return 1;
    }
    return 0;
  }
  if (!model_state_one_move(from, &cand, op))
    return 0;
  return model_state_equal(&cand, to);
}

static void
model_record_edge(model_runtime_t *rt, int sess, int op,
                  const model_state8_t *from, const model_state8_t *to) {
  model_edge_t *e;
  int i;

  if (model_state_equal(from, to))
    return; /* token cancels without state change produce no edge */
  if (!model_legal_edge(op, from, to, sess, rt)) {
    rt->mismatch = 1;
    strcpy(rt->mismatch_field, "queue");
    rt->mismatch_sess = sess;
    rt->mismatch_mid = 0;
    rt->mismatch_expected = 0;
    rt->mismatch_actual = 1;
    return;
  }
  for (i = 0; i < rt->nedges; i++) {
    if (model_state_equal(&rt->edges[i].from, from) &&
        rt->edges[i].op == op &&
        model_state_equal(&rt->edges[i].to, to))
      return;
  }
  if (rt->nedges < (int)(sizeof(rt->edges) / sizeof(rt->edges[0]))) {
    e = &rt->edges[rt->nedges++];
    e->from = *from;
    e->op = op;
    e->to = *to;
  }
}

/* ---- Mismatch helper --------------------------------------------------- */

static void
model_fail(model_runtime_t *rt, const char *field, long tick, int sess,
           long mid, long expected, long actual) {
  if (rt->mismatch)
    return;
  rt->mismatch = 1;
  strncpy(rt->mismatch_field, field, sizeof(rt->mismatch_field) - 1);
  rt->mismatch_field[sizeof(rt->mismatch_field) - 1] = 0;
  rt->mismatch_tick = tick;
  rt->mismatch_sess = sess;
  rt->mismatch_mid = mid;
  rt->mismatch_expected = expected;
  rt->mismatch_actual = actual;
}

/* ---- Differential queue checking --------------------------------------- */

typedef struct model_qnode {
  int sess;
  long mid;
  long token;
  long due;
  int retry;
  int seq;
} model_qnode_t;

static int
model_qnode_cmp(const void *pa, const void *pb) {
  const model_qnode_t *a = (const model_qnode_t *)pa;
  const model_qnode_t *b = (const model_qnode_t *)pb;

  if (a->due != b->due)
    return a->due < b->due ? -1 : 1;
  if (a->seq != b->seq)
    return a->seq - b->seq;
  return 0;
}

static model_req_t *
model_req_at(model_runtime_t *rt, int sess, long mid) {
  int i;

  for (i = 0; i < rt->nreq; i++) {
    if (rt->req[i].sess == sess && rt->req[i].mid == mid)
      return &rt->req[i];
  }
  return NULL;
}

/* Compares the real differential queue against the independent model after
 * every operation. Absolute due points are reconstructed purely by summing
 * the real queue's deltas onto its basetime; node->t is never rewritten. */
static void
model_check_queue(model_runtime_t *rt, long tick) {
  model_qnode_t expect[MODEL_MAX_REQS];
  model_qnode_t actual[MODEL_MAX_REQS];
  int nexpect = 0, nactual = 0;
  coap_queue_t *q;
  coap_tick_t cumulative = 0;
  int i;

  for (i = 0; i < rt->nreq; i++) {
    model_req_t *r = &rt->req[i];
    if (r->status == REQ_PENDING) {
      model_qnode_t *n = &expect[nexpect++];
      n->sess = r->sess;
      n->mid = r->mid;
      n->token = r->token;
      n->due = r->due;
      n->retry = r->retry;
      n->seq = r->seq;
    }
  }
  qsort(expect, (size_t)nexpect, sizeof(expect[0]), model_qnode_cmp);

  for (q = rt->ctx->sendqueue; q; q = q->next) {
    model_qnode_t *n;
    int sess = -1;

    cumulative += q->t;
    for (i = 0; i < rt->cp->sessions; i++) {
      if (rt->sess[i] == q->session) {
        sess = i;
        break;
      }
    }
    if (sess < 0) {
      model_fail(rt, "queue", tick, 0, q->id, 0, 1);
      return;
    }
    if (nactual >= MODEL_MAX_REQS) {
      model_fail(rt, "queue", tick, sess, q->id, nexpect, nactual + 1);
      return;
    }
    n = &actual[nactual++];
    n->sess = sess;
    n->mid = q->id;
    n->token = q->pdu->actual_token.length ? q->pdu->actual_token.s[0] : -1;
    n->due = (long)(rt->ctx->sendqueue_basetime + cumulative);
    n->retry = q->retransmit_cnt;
    n->seq = 0;
  }

  if (nactual != nexpect) {
    model_fail(rt, "queue", tick, 0, 0, nexpect, nactual);
    return;
  }
  for (i = 0; i < nexpect; i++) {
    model_req_t *r;
    if (actual[i].sess != expect[i].sess ||
        actual[i].mid != expect[i].mid ||
        actual[i].token != expect[i].token) {
      model_fail(rt, "queue", tick, expect[i].sess, expect[i].mid,
                 expect[i].mid, actual[i].mid);
      return;
    }
    if (actual[i].due != expect[i].due) {
      model_fail(rt, "due", tick, expect[i].sess, expect[i].mid,
                 expect[i].due, actual[i].due);
      return;
    }
    r = model_req_at(rt, expect[i].sess, expect[i].mid);
    if (!r || actual[i].retry != r->retry) {
      model_fail(rt, "retry", tick, expect[i].sess, expect[i].mid,
                 r ? r->retry : -1, actual[i].retry);
      return;
    }
  }
}

/* Verifies response/nack callback identities, reasons and counts. */
static void
model_check_callbacks(model_runtime_t *rt, long tick) {
  int i;

  if (rt->unknown_resp) {
    model_fail(rt, "response", tick, 0, 0, 0, rt->unknown_resp);
    return;
  }
  if (rt->unknown_nack) {
    model_fail(rt, "nack", tick, 0, 0, 0, rt->unknown_nack);
    return;
  }
  for (i = 0; i < rt->nreq; i++) {
    model_req_t *r = &rt->req[i];
    long exp_resp = (r->status == REQ_ACKED) ? 1 : 0;
    long exp_nack = 0;
    int exp_reason = -1;

    if (r->status == REQ_TIMEOUT) {
      exp_nack = 1;
      exp_reason = COAP_NACK_TOO_MANY_RETRIES;
    } else if (r->status == REQ_CANCELED && r->cancel_via_session) {
      exp_nack = 1;
      exp_reason = COAP_NACK_NOT_DELIVERABLE;
    }
    if (rt->resp_hits[i] != exp_resp) {
      model_fail(rt, "response", tick, r->sess, r->mid,
                 exp_resp, rt->resp_hits[i]);
      return;
    }
    if (rt->nack_hits[i] != exp_nack) {
      model_fail(rt, "nack", tick, r->sess, r->mid,
                 exp_nack, rt->nack_hits[i]);
      return;
    }
    if (exp_nack && rt->nack_reason[i] != exp_reason) {
      model_fail(rt, "nack", tick, r->sess, r->mid,
                 exp_reason, rt->nack_reason[i]);
      return;
    }
  }
}

/* ---- Step construction ------------------------------------------------- */

static int
model_build_steps(const model_case_t *cp, model_step_t *steps, int max_steps) {
  int n = 0, s, i;

  for (i = 0; i < cp->nevents; i++) {
    const model_event_t *ev = &cp->events[i];
    model_step_t *st;

    if (n >= max_steps)
      return -1;
    st = &steps[n++];
    st->tick = ev->tick;
    st->ev = ev;
    st->sess = ev->sess;
    switch (ev->kind) {
    case EV_CLOSE:
      st->prio = 0;
      st->key = 0;
      st->kind = MODEL_OP_CLOSE;
      break;
    case EV_CANCEL:
      st->prio = 2;
      st->key = ev->token;
      st->kind = MODEL_OP_CANCEL;
      break;
    case EV_ACK:
      st->prio = 3;
      st->key = ev->mid;
      st->kind = MODEL_OP_ACK;
      break;
    case EV_SEND:
      st->prio = 4;
      st->key = ev->mid;
      st->kind = MODEL_OP_SEND;
      break;
    default:
      return -1;
    }
  }

  /* Implicit session deadlines sit between CLOSE and CANCEL. */
  for (s = 0; s < MODEL_MAX_SESSIONS; s++) {
    model_step_t *st;

    if (!cp->used[s])
      continue;
    if (n >= max_steps)
      return -1;
    st = &steps[n++];
    st->tick = cp->deadline[s];
    st->prio = 1;
    st->sess = s;
    st->key = 0;
    st->kind = MODEL_OP_DEADLINE;
    st->ev = NULL;
  }

  qsort(steps, (size_t)n, sizeof(steps[0]), model_step_cmp);
  return n;
}

/* ---- PDU construction (library API only) ------------------------------- */

static coap_pdu_t *
model_build_con_get(coap_mid_t mid, uint8_t token) {
  coap_pdu_t *pdu = coap_pdu_init(COAP_MESSAGE_CON, COAP_REQUEST_CODE_GET,
                                  mid, 128);
  if (!pdu)
    return NULL;
  if (!coap_add_token(pdu, 1, &token)) {
    coap_delete_pdu(pdu);
    return NULL;
  }
  if (!coap_add_option(pdu, COAP_OPTION_URI_PATH, 10,
                       (const uint8_t *)"regression")) {
    coap_delete_pdu(pdu);
    return NULL;
  }
  if (!coap_pdu_encode_header(pdu, COAP_PROTO_UDP)) {
    coap_delete_pdu(pdu);
    return NULL;
  }
  return pdu;
}

static coap_pdu_t *
model_build_ack(coap_mid_t mid, uint8_t token) {
  coap_pdu_t *pdu = coap_pdu_init(COAP_MESSAGE_ACK,
                                  COAP_RESPONSE_CODE_CONTENT, mid, 128);
  if (!pdu)
    return NULL;
  if (!coap_add_token(pdu, 1, &token)) {
    coap_delete_pdu(pdu);
    return NULL;
  }
  if (!coap_pdu_encode_header(pdu, COAP_PROTO_UDP)) {
    coap_delete_pdu(pdu);
    return NULL;
  }
  return pdu;
}

/* ---- Per-case execution ------------------------------------------------ */

static model_req_t *
model_find_req_token(model_runtime_t *rt, int sess, long token) {
  int i;

  for (i = 0; i < rt->nreq; i++) {
    if (rt->req[i].sess == sess && rt->req[i].token == token)
      return &rt->req[i];
  }
  return NULL;
}

static void
model_snapshot_states(model_runtime_t *rt) {
  int s;

  for (s = 0; s < rt->cp->sessions; s++) {
    model_state8_t st;

    model_state_of(rt, s, &st);
    model_record_state(rt, &st);
  }
}

static void
model_transition_from(model_runtime_t *rt, int sess, int op,
                      const model_state8_t *before) {
  model_state8_t after;

  model_record_state(rt, before);
  /* The request bookkeeping was updated before this call; read the tuple
   * again and register the edge if the operation changed anything. */
  model_state_of(rt, sess, &after);
  model_record_state(rt, &after);
  model_record_edge(rt, sess, op, before, &after);
}

static void
model_do_send(model_runtime_t *rt, const model_event_t *ev) {
  coap_session_t *s = rt->sess[ev->sess];
  coap_queue_t *node;
  coap_pdu_t *pdu;
  model_req_t *r;
  coap_mid_t ret;

  if (rt->closed[ev->sess])
    return; /* SKIP: no edges after a closed session */

  pdu = model_build_con_get((coap_mid_t)ev->mid, (uint8_t)ev->token);
  if (!pdu) {
    model_fail(rt, "queue", ev->tick, ev->sess, ev->mid, 1, 0);
    return;
  }
  node = coap_new_node();
  if (!node) {
    coap_delete_pdu(pdu);
    model_fail(rt, "queue", ev->tick, ev->sess, ev->mid, 1, 0);
    return;
  }
  node->pdu = pdu;
  node->id = (coap_mid_t)ev->mid;
  node->timeout = (unsigned int)ev->timeout;

  ret = coap_wait_ack_at(rt->ctx, s, node, (coap_tick_t)ev->tick);
  if (ret != (coap_mid_t)ev->mid) {
    model_fail(rt, "queue", ev->tick, ev->sess, ev->mid, ev->mid, ret);
    return;
  }

  if (rt->nreq >= MODEL_MAX_REQS) {
    model_fail(rt, "queue", ev->tick, ev->sess, ev->mid, 1, 0);
    return;
  }
  model_state8_t before;
  model_state_of(rt, ev->sess, &before);
  r = &rt->req[rt->nreq++];
  r->sess = ev->sess;
  r->mid = ev->mid;
  r->token = ev->token;
  r->timeout = ev->timeout;
  r->send_tick = ev->tick;
  r->due = ev->tick + ev->timeout;
  r->retry = 0;
  r->status = REQ_PENDING;
  r->seq = rt->nreq - 1;
  r->cancel_via_session = 0;

  model_transition_from(rt, ev->sess, MODEL_OP_SEND, &before);
  model_check_queue(rt, ev->tick);
  model_check_callbacks(rt, ev->tick);
}

static void
model_do_ack(model_runtime_t *rt, const model_event_t *ev) {
  model_req_t *r = model_req_at(rt, ev->sess, ev->mid);
  coap_pdu_t *pdu;
  model_state8_t before;

  if (!r || r->status != REQ_PENDING || rt->closed[ev->sess])
    return; /* late ACK for a terminal request is a legal SKIP */

  pdu = model_build_ack((coap_mid_t)ev->mid, (uint8_t)r->token);
  if (!pdu) {
    model_fail(rt, "response", ev->tick, ev->sess, ev->mid, 1, 0);
    return;
  }
  coap_dispatch(rt->ctx, rt->sess[ev->sess], pdu);
  coap_delete_pdu(pdu);

  model_state_of(rt, ev->sess, &before);
  r->status = REQ_ACKED;
  model_transition_from(rt, ev->sess, MODEL_OP_ACK, &before);
  model_check_queue(rt, ev->tick);
  model_check_callbacks(rt, ev->tick);
}

static void
model_do_cancel(model_runtime_t *rt, const model_event_t *ev) {
  model_req_t *r = model_find_req_token(rt, ev->sess, ev->token);
  coap_bin_const_t tok;
  uint8_t token_byte = (uint8_t)ev->token;
  model_state8_t before;

  tok.length = 1;
  tok.s = &token_byte;
  /* Always drive the real library entry point; it is a no-op when the
   * token no longer has a queued node. */
  coap_cancel_all_messages(rt->ctx, rt->sess[ev->sess], &tok);

  if (!r || r->status != REQ_PENDING || rt->closed[ev->sess])
    return; /* SKIP: token cancel without state change produces no edge */

  model_state_of(rt, ev->sess, &before);
  r->status = REQ_CANCELED;
  r->cancel_via_session = 0;
  model_transition_from(rt, ev->sess, MODEL_OP_CANCEL, &before);
  model_check_queue(rt, ev->tick);
  model_check_callbacks(rt, ev->tick);
}

static void
model_do_session_end(model_runtime_t *rt, const model_step_t *st) {
  int s = st->sess;
  int i;
  model_state8_t before;

  if (rt->closed[s])
    return; /* SKIP after the session terminated */

  model_state_of(rt, s, &before);
  coap_cancel_session_messages(rt->ctx, rt->sess[s],
                               COAP_NACK_NOT_DELIVERABLE);
  rt->closed[s] = 1;
  for (i = 0; i < rt->nreq; i++) {
    if (rt->req[i].sess == s && rt->req[i].status == REQ_PENDING) {
      rt->req[i].status = REQ_CANCELED;
      rt->req[i].cancel_via_session = 1;
    }
  }
  model_transition_from(rt, s, st->kind, &before);
  model_check_queue(rt, st->tick);
  model_check_callbacks(rt, st->tick);
}

/* Pops every node due at or before @p limit and runs the retransmit core.
 * Nodes sharing one due tick are handled in session/MID order as required by
 * the contract; re-queued nodes land at a later tick and are picked up by a
 * later iteration of the outer loop. */
static void
model_advance(model_runtime_t *rt, long limit) {
  while (rt->ctx->sendqueue && !rt->mismatch) {
    coap_queue_t *due[MODEL_MAX_REQS];
    struct model_due_id { int sess; long mid; } ids[MODEL_MAX_REQS];
    long due_tick;
    coap_tick_t cumulative;
    int ndue = 0;
    int i, j;

    due_tick = (long)(rt->ctx->sendqueue_basetime +
                      rt->ctx->sendqueue->t);
    if (due_tick > limit)
      break;

    /* Collect the whole group of nodes sharing this absolute due tick.
     * coap_pop_next() makes the successor's t relative to the old head, so
     * the running delta must be accumulated to compare absolute dues. */
    cumulative = rt->ctx->sendqueue->t;
    while (rt->ctx->sendqueue) {
      long head_due = (long)(rt->ctx->sendqueue_basetime + cumulative);
      if (head_due != due_tick)
        break;
      if (ndue >= MODEL_MAX_REQS) {
        model_fail(rt, "queue", due_tick, 0, 0, MODEL_MAX_REQS, ndue + 1);
        return;
      }
      due[ndue] = coap_pop_next(rt->ctx);
      ids[ndue].mid = due[ndue]->id;
      ids[ndue].sess = -1;
      for (j = 0; j < rt->cp->sessions; j++) {
        if (rt->sess[j] == due[ndue]->session) {
          ids[ndue].sess = j;
          break;
        }
      }
      ndue++;
      /* The successor's t is relative to the just removed head; adding it
       * keeps cumulative equal to the absolute offset of the new head. */
      if (rt->ctx->sendqueue)
        cumulative += rt->ctx->sendqueue->t;
    }

    /* Stable session/MID ordering for same-tick automatic retransmissions. */
    for (i = 1; i < ndue; i++) {
      for (j = i; j > 0; j--) {
        if (ids[j].sess < ids[j - 1].sess ||
            (ids[j].sess == ids[j - 1].sess &&
             ids[j].mid < ids[j - 1].mid)) {
          struct model_due_id tmp_id = ids[j];
          coap_queue_t *tmp_node = due[j];
          ids[j] = ids[j - 1];
          due[j] = due[j - 1];
          ids[j - 1] = tmp_id;
          due[j - 1] = tmp_node;
        } else {
          break;
        }
      }
    }

    for (i = 0; i < ndue; i++) {
      coap_queue_t *node = due[i];
      model_req_t *r = model_req_at(rt, ids[i].sess, ids[i].mid);
      coap_mid_t ret;
      model_state8_t before;

      if (!r) {
        model_fail(rt, "queue", due_tick, ids[i].sess, ids[i].mid, 1, 0);
        coap_delete_node(node);
        return;
      }
      model_state_of(rt, r->sess, &before);
      ret = coap_retransmit_at(rt->ctx, node, (coap_tick_t)due_tick);
      if (ret == (coap_mid_t)r->mid) {
        /* Re-queued: one retry stage consumed. */
        r->retry++;
        r->due = due_tick + (r->timeout << r->retry);
        model_transition_from(rt, r->sess, MODEL_OP_RETRY, &before);
      } else if (ret == COAP_INVALID_MID) {
        /* Retry budget exhausted; the core deleted the node and raised the
         * TOO_MANY_RETRIES nack. */
        r->status = REQ_TIMEOUT;
        model_transition_from(rt, r->sess, MODEL_OP_TIMEOUT, &before);
      } else {
        model_fail(rt, "retry", due_tick, r->sess, r->mid, r->mid, ret);
        return;
      }
      model_check_queue(rt, due_tick);
      model_check_callbacks(rt, due_tick);
      if (rt->mismatch)
        return;
    }
  }
}

static int
model_run_case(const model_plan_t *plan, int case_idx, int out_fd) {
  model_runtime_t rt_storage;
  model_runtime_t *rt = &rt_storage;
  model_step_t steps[MODEL_MAX_EVENTS + MODEL_MAX_SESSIONS];
  int nsteps, i;
  coap_address_t addr;
  int rc = 0;

  memset(rt, 0, sizeof(*rt));
  rt->plan = plan;
  rt->cp = &plan->cases[case_idx];
  g_rt = rt;

  coap_startup();
  coap_set_log_level(COAP_LOG_EMERG);
  /* 100% send packet loss: no datagram ever reaches a socket. */
  coap_debug_set_packet_loss("100%");

  rt->ctx = coap_new_context(NULL);
  if (!rt->ctx)
    return 2;
  coap_register_response_handler(rt->ctx, model_response_handler);
  coap_register_nack_handler(rt->ctx, model_nack_handler);

  coap_address_init(&addr);
  addr.size = sizeof(addr.addr.sin);
  addr.addr.sin.sin_family = AF_INET;
  addr.addr.sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.addr.sin.sin_port = htons(COAP_DEFAULT_PORT);

  for (i = 0; i < rt->cp->sessions; i++) {
    rt->sess[i] = model_new_session(rt->ctx, i, &addr);
    if (!rt->sess[i]) {
      coap_free_context(rt->ctx);
      coap_cleanup();
      return 2;
    }
  }

  nsteps = model_build_steps(rt->cp, steps,
                             (int)(sizeof(steps) / sizeof(steps[0])));
  if (nsteps < 0) {
    coap_free_context(rt->ctx);
    coap_cleanup();
    return 2;
  }

  /* Initial tuples are reachable states too. */
  model_snapshot_states(rt);

  /* Steps are globally sorted by (tick, priority, session, key). Process
   * them in tick groups so that at a shared tick every explicit event
   * (CLOSE, DEADLINE, CANCEL, ACK, SEND) runs before any automatic
   * retransmission, matching the contract's priority order. */
  i = 0;
  while (i < nsteps && !rt->mismatch) {
    long tick = steps[i].tick;
    int j;

    /* Retransmissions strictly earlier than this tick. */
    model_advance(rt, tick - 1);
    if (rt->mismatch)
      break;

    /* Every explicit event sharing this tick, in priority order. */
    j = i;
    while (j < nsteps && steps[j].tick == tick && !rt->mismatch) {
      const model_step_t *st = &steps[j];

      switch (st->kind) {
      case MODEL_OP_SEND:
        model_do_send(rt, st->ev);
        break;
      case MODEL_OP_ACK:
        model_do_ack(rt, st->ev);
        break;
      case MODEL_OP_CANCEL:
        model_do_cancel(rt, st->ev);
        break;
      case MODEL_OP_CLOSE:
      case MODEL_OP_DEADLINE:
        model_do_session_end(rt, st);
        break;
      default:
        break;
      }
      j++;
    }
    if (rt->mismatch)
      break;

    /* Same-tick automatic retransmissions have the lowest priority. */
    model_advance(rt, tick);
    i = j;
  }

  /* Drain anything still pending up to the latest deadline so normal
   * resource release always runs through the library teardown. */
  if (!rt->mismatch) {
    long last_tick = 0;
    for (i = 0; i < rt->cp->sessions; i++) {
      if (rt->cp->deadline[i] > last_tick)
        last_tick = rt->cp->deadline[i];
    }
    model_advance(rt, last_tick);
    model_snapshot_states(rt);
  }

  if (rt->mismatch) {
    if (model_ipc_emit_mismatch(rt, out_fd))
      rc = 2;
    else
      rc = 1;
  } else {
    rc = model_ipc_emit(rt, case_idx, out_fd);
    if (rc)
      rc = 2;
  }

  coap_free_context(rt->ctx);
  coap_cleanup();
  g_rt = NULL;
  return rc;
}

/* ---- IPC serialization ------------------------------------------------- */

static void
model_put_u16(unsigned char *p, uint16_t v) {
  p[0] = (unsigned char)(v >> 8);
  p[1] = (unsigned char)v;
}

static void
model_put_u32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)v;
}

static void
model_put_i32(unsigned char *p, int32_t v) {
  model_put_u32(p, (uint32_t)v);
}

static void
model_put_i64(unsigned char *p, int64_t v) {
  uint64_t u = (uint64_t)v;
  int i;

  for (i = 0; i < 8; i++)
    p[i] = (unsigned char)(u >> (56 - i * 8));
}

static int
model_write_all(int fd, const void *buf, size_t len) {
  const unsigned char *p = (const unsigned char *)buf;

  while (len) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static int
model_frame_write(int fd, unsigned char type, const unsigned char *payload,
                  uint16_t len) {
  unsigned char hdr[3];

  hdr[0] = type;
  model_put_u16(hdr + 1, len);
  if (model_write_all(fd, hdr, sizeof(hdr)))
    return -1;
  if (len && model_write_all(fd, payload, len))
    return -1;
  return 0;
}

static void
model_encode_state(const model_state8_t *st, unsigned char *p) {
  model_put_u32(p + 0, st->u);
  model_put_u32(p + 4, st->p0);
  model_put_u32(p + 8, st->p1);
  model_put_u32(p + 12, st->p2);
  model_put_u32(p + 16, st->a);
  model_put_u32(p + 20, st->c);
  model_put_u32(p + 24, st->t);
  model_put_u32(p + 28, st->z);
}

/* Builds the canonical single-case replay plan (the witness). */
static int
model_build_witness(const model_case_t *cp, const model_step_t *steps,
                    int nsteps, char *out, size_t out_size) {
  size_t used = 0;
  int s, i;
  int n;

#define MODEL_APP(...) do { \
    n = snprintf(out + used, out_size - used, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= out_size - used) return -1; \
    used += (size_t)n; \
  } while (0)

  MODEL_APP("COAP_MODEL_PLAN 1\n");
  MODEL_APP("CASE %s\n", cp->id);
  for (s = 0; s < MODEL_MAX_SESSIONS; s++) {
    if (cp->used[s])
      MODEL_APP("SESSION %d %ld\n", s, cp->deadline[s]);
  }
  for (i = 0; i < nsteps; i++) {
    const model_step_t *st = &steps[i];
    const model_event_t *ev = st->ev;

    if (!ev)
      continue; /* implicit DEADLINE steps are derived, not text events */
    switch (ev->kind) {
    case EV_SEND:
      MODEL_APP("SEND %ld %d %ld %ld %ld\n", ev->tick, ev->sess, ev->mid,
                ev->token, ev->timeout);
      break;
    case EV_ACK:
      MODEL_APP("ACK %ld %d %ld\n", ev->tick, ev->sess, ev->mid);
      break;
    case EV_CANCEL:
      MODEL_APP("CANCEL %ld %d %ld\n", ev->tick, ev->sess, ev->token);
      break;
    case EV_CLOSE:
      MODEL_APP("CLOSE %ld %d\n", ev->tick, ev->sess);
      break;
    default:
      break;
    }
  }
  MODEL_APP("END\n");
#undef MODEL_APP
  return (int)used;
}

static int
model_ipc_emit(model_runtime_t *rt, int case_idx, int out_fd) {
  const model_case_t *cp = rt->cp;
  model_step_t steps[MODEL_MAX_EVENTS + MODEL_MAX_SESSIONS];
  /* Static: the large witness buffer must not live on the worker stack.
   * Each worker process handles one CASE sequentially. */
  static char witness[MODEL_WITNESS_MAX];
  unsigned char payload[128];
  int nsteps, wlen;
  int i;
  unsigned long acked = 0, canceled = 0, timeouts = 0, retries = 0;

  nsteps = model_build_steps(cp, steps,
                             (int)(sizeof(steps) / sizeof(steps[0])));
  if (nsteps < 0)
    return -1;
  wlen = model_build_witness(cp, steps, nsteps, witness, sizeof(witness));
  if (wlen < 0 || wlen > 0xffff - (int)strlen(cp->id) - 1)
    return -1;

  for (i = 0; i < rt->nstates; i++) {
    model_encode_state(&rt->states[i], payload);
    if (model_frame_write(out_fd, MODEL_FRAME_STATE, payload, 32))
      return -1;
  }
  for (i = 0; i < rt->nedges; i++) {
    size_t idlen = strlen(cp->id);
    model_edge_t *e = &rt->edges[i];

    payload[0] = (unsigned char)idlen;
    memcpy(payload + 1, cp->id, idlen);
    model_encode_state(&e->from, payload + 1 + idlen);
    payload[1 + idlen + 32] = (unsigned char)e->op;
    model_encode_state(&e->to, payload + 1 + idlen + 33);
    if (model_frame_write(out_fd, MODEL_FRAME_EDGE, payload,
                          (uint16_t)(1 + idlen + 65)))
      return -1;
  }

  /* One canonical witness per case. */
  {
    size_t idlen = strlen(cp->id);
    static unsigned char wbuf[MODEL_WITNESS_MAX + MODEL_MAX_ID_LEN + 2];
    size_t frame_len = 1 + idlen + (size_t)wlen;

    if (frame_len > 0xffff || frame_len > sizeof(wbuf))
      return -1;
    wbuf[0] = (unsigned char)idlen;
    memcpy(wbuf + 1, cp->id, idlen);
    memcpy(wbuf + 1 + idlen, witness, (size_t)wlen);
    if (model_frame_write(out_fd, MODEL_FRAME_WITNESS, wbuf,
                          (uint16_t)frame_len))
      return -1;
  }

  for (i = 0; i < rt->nreq; i++) {
    switch (rt->req[i].status) {
    case REQ_ACKED:    acked++; break;
    case REQ_CANCELED: canceled++; break;
    case REQ_TIMEOUT:  timeouts++; break;
    default: break;
    }
    retries += (unsigned long)rt->req[i].retry;
  }
  {
    size_t idlen = strlen(cp->id);
    payload[0] = (unsigned char)idlen;
    memcpy(payload + 1, cp->id, idlen);
    model_put_u32(payload + 1 + idlen, (uint32_t)cp->sessions);
    model_put_u32(payload + 5 + idlen, (uint32_t)acked);
    model_put_u32(payload + 9 + idlen, (uint32_t)canceled);
    model_put_u32(payload + 13 + idlen, (uint32_t)timeouts);
    model_put_u32(payload + 17 + idlen, (uint32_t)retries);
    if (model_frame_write(out_fd, MODEL_FRAME_CASE, payload,
                          (uint16_t)(21 + idlen)))
      return -1;
  }

  (void)case_idx;
  if (model_frame_write(out_fd, MODEL_FRAME_END, NULL, 0))
    return -1;
  return 0;
}

static int
model_ipc_emit_mismatch(model_runtime_t *rt, int out_fd) {
  const char *id = rt->cp->id;
  size_t idlen = strlen(id);
  size_t flen = strlen(rt->mismatch_field);
  unsigned char payload[96];
  size_t off = 0;

  payload[off++] = (unsigned char)idlen;
  memcpy(payload + off, id, idlen);
  off += idlen;
  model_put_u32(payload + off, (uint32_t)rt->mismatch_tick); off += 4;
  model_put_u32(payload + off, (uint32_t)rt->mismatch_sess); off += 4;
  model_put_i32(payload + off, (int32_t)rt->mismatch_mid); off += 4;
  payload[off++] = (unsigned char)flen;
  memcpy(payload + off, rt->mismatch_field, flen); off += flen;
  model_put_i64(payload + off, (int64_t)rt->mismatch_expected); off += 8;
  model_put_i64(payload + off, (int64_t)rt->mismatch_actual); off += 8;
  return model_frame_write(out_fd, 'M', payload, (uint16_t)off);
}

static int
model_worker_run(const model_plan_t *plan, int case_idx, int out_fd) {
  return model_run_case(plan, case_idx, out_fd);
}

/* ---- Parent side merge ------------------------------------------------- */

#define MODEL_PARENT_STATES 256
#define MODEL_PARENT_EDGES  8192
#define MODEL_PARENT_CASES  MODEL_MAX_CASES

typedef struct model_parent_edge {
  model_state8_t from;
  int op;
  model_state8_t to;
  char hit_cases[MODEL_MAX_CASES][MODEL_MAX_ID_LEN + 1];
  int nhits;
  char witness_case[MODEL_MAX_ID_LEN + 1];
  unsigned char *witness_hex;
  size_t witness_hex_len;
  int witness_event_lines;
} model_parent_edge_t;

typedef struct model_parent_case {
  char id[MODEL_MAX_ID_LEN + 1];
  int present;
  unsigned sessions, acked, canceled, timeout, retries;
  unsigned char *witness_hex;
  size_t witness_hex_len;
  int event_lines;
} model_parent_case_t;

typedef struct model_worker_pipe {
  int fd;
  pid_t pid;
  int case_idx;
  size_t bytes;
  int eof;
  int exit_rc;
  int reaped;
} model_worker_pipe_t;

typedef struct model_mismatch_rec {
  int present;
  char case_id[MODEL_MAX_ID_LEN + 1];
  long tick;
  int sess;
  long mid;
  char field[16];
  long expected;
  long actual;
} model_mismatch_rec_t;

typedef struct model_parent {
  model_state8_t states[MODEL_PARENT_STATES];
  int nstates;
  model_parent_edge_t edges[MODEL_PARENT_EDGES];
  int nedges;
  model_parent_case_t cases[MODEL_PARENT_CASES];
  model_mismatch_rec_t mismatch;
  int io_failed;
} model_parent_t;

static int
model_parent_find_state(model_parent_t *p, const model_state8_t *st) {
  int i;

  for (i = 0; i < p->nstates; i++) {
    if (model_state_equal(&p->states[i], st))
      return i;
  }
  return -1;
}

static void
model_parent_add_state(model_parent_t *p, const model_state8_t *st) {
  if (p->nstates < MODEL_PARENT_STATES &&
      model_parent_find_state(p, st) < 0)
    p->states[p->nstates++] = *st;
}

static int
model_parent_find_edge(model_parent_t *p, const model_edge_t *e) {
  int i;

  for (i = 0; i < p->nedges; i++) {
    if (model_state_equal(&p->edges[i].from, &e->from) &&
        p->edges[i].op == e->op &&
        model_state_equal(&p->edges[i].to, &e->to))
      return i;
  }
  return -1;
}

static int
model_case_event_lines(const model_plan_t *plan, int case_idx) {
  return plan->cases[case_idx].nevents;
}

static void
model_parent_store_witness(model_parent_case_t *pc,
                           const unsigned char *raw, size_t raw_len,
                           int event_lines) {
  static const char hexd[] = "0123456789abcdef";
  size_t i;

  free(pc->witness_hex);
  pc->witness_hex = (unsigned char *)malloc(raw_len * 2 + 1);
  if (!pc->witness_hex)
    return;
  for (i = 0; i < raw_len; i++) {
    pc->witness_hex[2 * i] = (unsigned char)hexd[(raw[i] >> 4) & 0xf];
    pc->witness_hex[2 * i + 1] = (unsigned char)hexd[raw[i] & 0xf];
  }
  pc->witness_hex[2 * raw_len] = 0;
  pc->witness_hex_len = raw_len * 2;
  pc->event_lines = event_lines;
}

/* Reads exactly @p len bytes, tolerating EINTR and short reads. */
static int
model_read_exact(int fd, unsigned char *buf, size_t len) {
  size_t got = 0;

  while (got < len) {
    ssize_t n = read(fd, buf + got, len - got);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return got == 0 ? 1 : -1; /* clean EOF or truncated frame */
    got += (size_t)n;
  }
  return 0;
}

static uint16_t
model_get_u16(const unsigned char *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t
model_get_u32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int64_t
model_get_i64(const unsigned char *p) {
  uint64_t u = 0;
  int i;

  for (i = 0; i < 8; i++)
    u = (u << 8) | p[i];
  return (int64_t)u;
}

static void
model_decode_state(const unsigned char *p, model_state8_t *st) {
  st->u = model_get_u32(p + 0);
  st->p0 = model_get_u32(p + 4);
  st->p1 = model_get_u32(p + 8);
  st->p2 = model_get_u32(p + 12);
  st->a = model_get_u32(p + 16);
  st->c = model_get_u32(p + 20);
  st->t = model_get_u32(p + 24);
  st->z = model_get_u32(p + 28);
}

/* Consumes one frame from @p wp. Returns 0 on a frame, 1 on clean EOF,
 * -1 on truncated/oversized I/O. */
static int
model_parent_consume(model_parent_t *p, model_worker_pipe_t *wp,
                     const model_plan_t *plan) {
  unsigned char hdr[3];
  /* Must hold the largest witness frame plus every smaller frame type. */
  static unsigned char payload[MODEL_WITNESS_MAX + MODEL_MAX_ID_LEN + 2];
  uint16_t len;
  int r;
  unsigned char type;

  r = model_read_exact(wp->fd, hdr, sizeof(hdr));
  if (r == 1)
    return 1;
  if (r < 0)
    return -1;
  type = hdr[0];
  len = model_get_u16(hdr + 1);
  wp->bytes += 3u + len;
  if (wp->bytes > MODEL_IPC_LIMIT)
    return -1;
  if (len > sizeof(payload))
    return -1;
  if (len && model_read_exact(wp->fd, payload, len) < 0)
    return -1;

  switch (type) {
  case MODEL_FRAME_STATE: {
    model_state8_t st;

    if (len != 32)
      return -1;
    model_decode_state(payload, &st);
    model_parent_add_state(p, &st);
    break;
  }
  case MODEL_FRAME_EDGE: {
    model_edge_t e;
    size_t idlen;
    char case_id[MODEL_MAX_ID_LEN + 1];
    int idx, i;

    if (len < 1 + 33 + 32)
      return -1;
    idlen = payload[0];
    if (idlen > MODEL_MAX_ID_LEN || len != 1 + idlen + 65)
      return -1;
    memcpy(case_id, payload + 1, idlen);
    case_id[idlen] = 0;
    model_decode_state(payload + 1 + idlen, &e.from);
    e.op = payload[1 + idlen + 32];
    model_decode_state(payload + 1 + idlen + 33, &e.to);
    idx = model_parent_find_edge(p, &e);
    if (idx < 0) {
      model_parent_edge_t *pe;
      if (p->nedges >= MODEL_PARENT_EDGES)
        return -1;
      pe = &p->edges[p->nedges++];
      memset(pe, 0, sizeof(*pe));
      pe->from = e.from;
      pe->op = e.op;
      pe->to = e.to;
      idx = p->nedges - 1;
    }
    for (i = 0; i < p->edges[idx].nhits; i++) {
      if (strcmp(p->edges[idx].hit_cases[i], case_id) == 0)
        break;
    }
    if (i == p->edges[idx].nhits &&
        p->edges[idx].nhits < MODEL_MAX_CASES) {
      strcpy(p->edges[idx].hit_cases[p->edges[idx].nhits++], case_id);
    }
    break;
  }
  case MODEL_FRAME_WITNESS: {
    size_t idlen, raw_len;
    char case_id[MODEL_MAX_ID_LEN + 1];
    int ci;

    if (len < 2)
      return -1;
    idlen = payload[0];
    if (idlen > MODEL_MAX_ID_LEN || len <= 1 + idlen)
      return -1;
    memcpy(case_id, payload + 1, idlen);
    case_id[idlen] = 0;
    raw_len = len - 1 - idlen;
    for (ci = 0; ci < plan->ncases; ci++) {
      if (strcmp(plan->cases[ci].id, case_id) == 0)
        break;
    }
    if (ci >= plan->ncases)
      return -1;
    if (!p->cases[ci].present) {
      memset(&p->cases[ci], 0, sizeof(p->cases[ci]));
      strcpy(p->cases[ci].id, case_id);
      p->cases[ci].present = 1;
    }
    model_parent_store_witness(&p->cases[ci], payload + 1 + idlen, raw_len,
                               model_case_event_lines(plan, ci));
    break;
  }
  case MODEL_FRAME_CASE: {
    size_t idlen;
    char case_id[MODEL_MAX_ID_LEN + 1];
    int ci;

    if (len < 22)
      return -1;
    idlen = payload[0];
    if (idlen > MODEL_MAX_ID_LEN || len != 21 + idlen)
      return -1;
    memcpy(case_id, payload + 1, idlen);
    case_id[idlen] = 0;
    for (ci = 0; ci < plan->ncases; ci++) {
      if (strcmp(plan->cases[ci].id, case_id) == 0)
        break;
    }
    if (ci >= plan->ncases)
      return -1;
    if (!p->cases[ci].present) {
      memset(&p->cases[ci], 0, sizeof(p->cases[ci]));
      strcpy(p->cases[ci].id, case_id);
      p->cases[ci].present = 1;
    }
    p->cases[ci].sessions = model_get_u32(payload + 1 + idlen);
    p->cases[ci].acked = model_get_u32(payload + 5 + idlen);
    p->cases[ci].canceled = model_get_u32(payload + 9 + idlen);
    p->cases[ci].timeout = model_get_u32(payload + 13 + idlen);
    p->cases[ci].retries = model_get_u32(payload + 17 + idlen);
    break;
  }
  case 'M': {
    size_t idlen, off = 0, flen;
    model_mismatch_rec_t *m = &p->mismatch;

    if (len < 1 + 12 + 1)
      return -1;
    idlen = payload[off++];
    if (idlen > MODEL_MAX_ID_LEN || off + idlen + 17 > len)
      return -1;
    if (!m->present ||
        memcmp(payload + off, m->case_id, idlen) < 0 ||
        (idlen == strlen(m->case_id) &&
         memcmp(payload + off, m->case_id, idlen) == 0)) {
      memset(m, 0, sizeof(*m));
      m->present = 1;
      memcpy(m->case_id, payload + off, idlen);
      m->case_id[idlen] = 0;
      off += (int)idlen;
      m->tick = (long)model_get_u32(payload + off); off += 4;
      m->sess = (int)model_get_u32(payload + off); off += 4;
      m->mid = (long)(int32_t)model_get_u32(payload + off); off += 4;
      flen = payload[off++];
      if (flen >= sizeof(m->field) || off + flen + 16 > len)
        return -1;
      memcpy(m->field, payload + off, flen);
      m->field[flen] = 0;
      off += flen;
      m->expected = (long)model_get_i64(payload + off); off += 8;
      m->actual = (long)model_get_i64(payload + off);
    }
    break;
  }
  case MODEL_FRAME_END:
    if (len != 0)
      return -1;
    break;
  default:
    return -1;
  }
  return 0;
}

/* ---- Report ordering --------------------------------------------------- */

static int
model_edge_sort_cmp(const void *pa, const void *pb) {
  const model_parent_edge_t *a = (const model_parent_edge_t *)pa;
  const model_parent_edge_t *b = (const model_parent_edge_t *)pb;
  int c;

  c = model_state_cmp(&a->from, &b->from);
  if (c)
    return c;
  c = strcmp(model_op_name(a->op), model_op_name(b->op));
  if (c)
    return c;
  return model_state_cmp(&a->to, &b->to);
}

static int
model_case_sort_cmp(const void *pa, const void *pb) {
  const model_parent_case_t *a = (const model_parent_case_t *)pa;
  const model_parent_case_t *b = (const model_parent_case_t *)pb;

  return strcmp(a->id, b->id);
}

/* Picks the shortest witness (fewest event lines, then smallest case id)
 * for every edge once all workers have reported. */
static void
model_parent_resolve_witnesses(model_parent_t *p) {
  int i;

  for (i = 0; i < p->nedges; i++) {
    model_parent_edge_t *e = &p->edges[i];
    int h;

    for (h = 0; h < e->nhits; h++) {
      const char *cid = e->hit_cases[h];
      model_parent_case_t *pc = NULL;
      int k;

      for (k = 0; k < MODEL_PARENT_CASES; k++) {
        if (p->cases[k].present && strcmp(p->cases[k].id, cid) == 0) {
          pc = &p->cases[k];
          break;
        }
      }
      if (!pc || !pc->witness_hex)
        continue;
      if (!e->witness_hex ||
          pc->event_lines < e->witness_event_lines ||
          (pc->event_lines == e->witness_event_lines &&
           strcmp(pc->id, e->witness_case) < 0)) {
        free(e->witness_hex);
        e->witness_hex = (unsigned char *)malloc(pc->witness_hex_len + 1);
        if (e->witness_hex) {
          memcpy(e->witness_hex, pc->witness_hex, pc->witness_hex_len + 1);
          e->witness_hex_len = pc->witness_hex_len;
          e->witness_event_lines = pc->event_lines;
          strcpy(e->witness_case, pc->id);
        }
      }
    }
  }
}

static int
model_parent_write_report(model_parent_t *p, const model_plan_t *plan,
                          FILE *report) {
  model_parent_case_t sorted[MODEL_PARENT_CASES];
  int ncases = 0, i;

  qsort(p->states, (size_t)p->nstates, sizeof(p->states[0]),
        (int (*)(const void *, const void *))model_state_cmp);
  qsort(p->edges, (size_t)p->nedges, sizeof(p->edges[0]),
        (int (*)(const void *, const void *))model_edge_sort_cmp);

  for (i = 0; i < plan->ncases; i++) {
    if (p->cases[i].present)
      sorted[ncases++] = p->cases[i];
  }
  qsort(sorted, (size_t)ncases, sizeof(sorted[0]),
        (int (*)(const void *, const void *))model_case_sort_cmp);

  if (fputs("COAP_MODEL_COVERAGE 1\n", report) == EOF)
    return -1;
  for (i = 0; i < p->nstates; i++) {
    const model_state8_t *s = &p->states[i];
    if (fprintf(report, "STATE %u,%u,%u,%u,%u,%u,%u,%u\n",
                s->u, s->p0, s->p1, s->p2, s->a, s->c, s->t, s->z) < 0)
      return -1;
  }
  for (i = 0; i < p->nedges; i++) {
    const model_parent_edge_t *e = &p->edges[i];
    if (fprintf(report,
                "EDGE %u,%u,%u,%u,%u,%u,%u,%u %s "
                "%u,%u,%u,%u,%u,%u,%u,%u HITS %d CASE %s WITNESS %s\n",
                e->from.u, e->from.p0, e->from.p1, e->from.p2,
                e->from.a, e->from.c, e->from.t, e->from.z,
                model_op_name(e->op),
                e->to.u, e->to.p0, e->to.p1, e->to.p2,
                e->to.a, e->to.c, e->to.t, e->to.z,
                e->nhits, e->witness_case,
                e->witness_hex ? (char *)e->witness_hex : "") < 0)
      return -1;
  }
  for (i = 0; i < ncases; i++) {
    const model_parent_case_t *c = &sorted[i];
    if (fprintf(report,
                "CASE %s sessions=%u acked=%u canceled=%u timeout=%u "
                "retries=%u\n",
                c->id, c->sessions, c->acked, c->canceled, c->timeout,
                c->retries) < 0)
      return -1;
  }
  return 0;
}

/* ---- Worker process orchestration -------------------------------------- */

static int
model_spawn_workers(const model_plan_t *plan, model_parent_t *parent) {
  model_worker_pipe_t workers[MODEL_MAX_CASES];
  struct pollfd pfds[MODEL_MAX_CASES];
  int active = 0, i;

  memset(workers, 0, sizeof(workers));
  for (i = 0; i < plan->ncases; i++) {
    int pipefd[2];
    pid_t pid;

    if (pipe(pipefd) < 0)
      return 2;
    pid = fork();
    if (pid < 0) {
      close(pipefd[0]);
      close(pipefd[1]);
      return 2;
    }
    if (pid == 0) {
      int rc;
      int j;

      close(pipefd[0]);
      for (j = 0; j < i; j++)
        close(workers[j].fd);
      rc = model_worker_run(plan, i, pipefd[1]);
      close(pipefd[1]);
      _exit(rc);
    }
    close(pipefd[1]);
    workers[i].fd = pipefd[0];
    workers[i].pid = pid;
    workers[i].case_idx = i;
  }
  active = plan->ncases;

  /* Keep reading every worker until all pipes reach EOF so a full pipe can
   * never deadlock parent and child against each other. */
  while (active > 0) {
    int nready, k;

    for (i = 0, k = 0; i < plan->ncases; i++) {
      if (!workers[i].eof) {
        pfds[k].fd = workers[i].fd;
        pfds[k].events = POLLIN;
        pfds[k].revents = 0;
        k++;
      }
    }
    nready = poll(pfds, (nfds_t)k, -1);
    if (nready < 0) {
      if (errno == EINTR)
        continue;
      parent->io_failed = 1;
      break;
    }
    for (i = 0; i < plan->ncases; i++) {
      int j;

      if (workers[i].eof)
        continue;
      for (j = 0; j < k; j++) {
        if (pfds[j].fd == workers[i].fd)
          break;
      }
      if (j >= k || !(pfds[j].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;
      for (;;) {
        int r = model_parent_consume(parent, &workers[i], plan);
        if (r == 1) {
          workers[i].eof = 1;
          active--;
          break;
        }
        if (r < 0) {
          parent->io_failed = 1;
          workers[i].eof = 1;
          active--;
          break;
        }
      }
    }
  }

  for (i = 0; i < plan->ncases; i++) {
    int status = 0;

    if (workers[i].fd >= 0)
      close(workers[i].fd);
    while (waitpid(workers[i].pid, &status, 0) < 0) {
      if (errno != EINTR) {
        parent->io_failed = 1;
        break;
      }
    }
    if (WIFEXITED(status))
      workers[i].exit_rc = WEXITSTATUS(status);
    else
      parent->io_failed = 1;
    workers[i].reaped = 1;
  }

  if (parent->io_failed)
    return 2;
  /* The frame stream and the exit status must agree for every worker. */
  for (i = 0; i < plan->ncases; i++) {
    int expected = parent->mismatch.present ? 0 : 0;
    (void)expected;
    if (workers[i].exit_rc != 0 && workers[i].exit_rc != 1)
      return 2;
  }
  if (parent->mismatch.present) {
    /* Ensure the reported mismatch belongs to a worker that actually
     * returned 1. */
    int found = 0;
    for (i = 0; i < plan->ncases; i++) {
      if (workers[i].exit_rc == 1) {
        found = 1;
        break;
      }
    }
    if (!found)
      return 2;
    return 1;
  }
  for (i = 0; i < plan->ncases; i++) {
    if (workers[i].exit_rc != 0)
      return 2;
    if (!parent->cases[i].present || !parent->cases[i].witness_hex)
      return 2;
  }
  return 0;
}

/* ---- Public entry point ------------------------------------------------ */

int
coap_model_run(const uint8_t *data, size_t size, FILE *report) {
  static model_plan_t plan_storage;
  model_plan_t *plan = &plan_storage;
  /* Static storage keeps the large merge tables off the worker/parent stack.
   * The executor is entered sequentially and fully reset on every call. */
  static model_parent_t parent;
  int rc;

  if (!report || !data)
    return 2;
  if (!model_parse(data, size, plan))
    return 2;

  memset(&parent, 0, sizeof(parent));
  rc = model_spawn_workers(plan, &parent);
  if (rc == 2 || parent.io_failed)
    return 2;
  if (rc == 1 || parent.mismatch.present) {
    const model_mismatch_rec_t *m = &parent.mismatch;
    if (fprintf(report,
                "MISMATCH %s %ld %d %ld %s %ld %ld\n",
                m->case_id, m->tick, m->sess, m->mid, m->field,
                m->expected, m->actual) < 0)
      return 2;
    if (fflush(report) == EOF)
      return 2;
    return 1;
  }

  model_parent_resolve_witnesses(&parent);
  if (model_parent_write_report(&parent, plan, report) < 0)
    return 2;
  if (fflush(report) == EOF)
    return 2;

  for (int i = 0; i < MODEL_PARENT_CASES; i++) {
    free(parent.cases[i].witness_hex);
  }
  for (int i = 0; i < parent.nedges; i++) {
    free(parent.edges[i].witness_hex);
  }
  return 0;
}
