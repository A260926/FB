#include "fuzz/stateful/model.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------- byte queue ----------------------------- */

void qm_bytes_init(qm_bytes *b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

void qm_bytes_free(qm_bytes *b) {
  free(b->data);
  qm_bytes_init(b);
}

int qm_bytes_reserve(qm_bytes *b, size_t extra) {
  size_t need;
  uint8_t *p;

  if (extra > SIZE_MAX - b->len) {
    return -1;
  }
  need = b->len + extra;
  if (need <= b->cap) {
    return 0;
  }
  if (b->cap == 0) {
    b->cap = 64;
  }
  while (b->cap < need) {
    if (b->cap > SIZE_MAX / 2) {
      b->cap = need;
      break;
    }
    b->cap *= 2;
  }
  p = realloc(b->data, b->cap);
  if (p == NULL) {
    return -1;
  }
  b->data = p;
  return 0;
}

int qm_bytes_append(qm_bytes *b, const void *data, size_t len) {
  if (len == 0) {
    return 0;
  }
  if (qm_bytes_reserve(b, len) != 0) {
    return -1;
  }
  memcpy(b->data + b->len, data, len);
  b->len += len;
  return 0;
}

void qm_bytes_consume(qm_bytes *b, size_t len) {
  if (len == 0) {
    return;
  }
  if (len >= b->len) {
    b->len = 0;
    return;
  }
  memmove(b->data, b->data + len, b->len - len);
  b->len -= len;
}

/* ----------------------------- varint parser ---------------------------- */

int qm_varint_decode(const uint8_t *p, size_t n, unsigned prefix_bits,
                     uint64_t *value, size_t *consumed) {
  uint64_t mask, v, add;
  unsigned shift = 0;
  size_t i;

  if (prefix_bits == 0 || prefix_bits > 8) {
    return -1;
  }
  if (n == 0) {
    return 0;
  }

  mask = prefix_bits == 8 ? 0xffULL : ((1ULL << prefix_bits) - 1);
  v = p[0] & mask;
  i = 1;

  if (v != mask) {
    *value = v;
    *consumed = 1;
    return 1;
  }

  v = mask;
  for (; i < n; ++i, shift += 7) {
    add = p[i] & 0x7fU;
    if (shift > 62 || add > (NGHTTP3_QPACK_INT_MAX >> shift) ||
        v > NGHTTP3_QPACK_INT_MAX - (add << shift)) {
      return -1;
    }
    v += add << shift;
    if ((p[i] & 0x80U) == 0) {
      *value = v;
      *consumed = i + 1;
      return 1;
    }
  }

  *value = v;
  *consumed = i;
  return 0;
}

void qm_vi_reset(qm_varint_state *v) {
  memset(v, 0, sizeof(*v));
}

int qm_vi_first(qm_varint_state *v, unsigned prefix_bits, uint8_t b) {
  uint64_t mask;

  qm_vi_reset(v);
  v->prefix_bits = prefix_bits;
  v->started = 1;
  if (prefix_bits == 8) {
    mask = 0xffULL;
  } else {
    mask = (1ULL << prefix_bits) - 1;
  }
  v->value = b & mask;
  if (v->value != mask) {
    v->complete = 1;
    return 1;
  }
  v->value = mask;
  return 0;
}

int qm_vi_next(qm_varint_state *v, uint8_t b) {
  uint64_t add;

  if (v->complete) {
    return 1;
  }
  add = b & 0x7fU;
  if (v->shift > 62 || add > (NGHTTP3_QPACK_INT_MAX >> v->shift) ||
      v->value > NGHTTP3_QPACK_INT_MAX - (add << v->shift)) {
    return -1;
  }
  v->value += add << v->shift;
  v->shift += 7;
  if ((b & 0x80U) == 0) {
    v->complete = 1;
    return 1;
  }
  return 0;
}

/* -------------------------- encoder instruction ------------------------- */

enum {
  QM_ES_OP_DUPLICATE,
  QM_ES_OP_SET_CAP,
  QM_ES_OP_INSERT_LITERAL,
  QM_ES_OP_INSERT_INDEXED
};

void qm_es_parser_init(qm_es_parser *p) {
  memset(p, 0, sizeof(*p));
  p->state = QM_ES_OPCODE;
  qm_vi_reset(&p->vi);
}

static int qm_es_value_len_done(qm_es_parser *p) {
  p->skip = p->vi.value;
  if (p->skip == 0) {
    if (p->opcode == QM_ES_OP_INSERT_LITERAL ||
        p->opcode == QM_ES_OP_INSERT_INDEXED) {
      return 1;
    }
  }
  p->state = QM_ES_VALUE_DATA;
  return 0;
}

int qm_es_parser_feed(qm_es_parser *p, const uint8_t *data, size_t len,
                      uint64_t *insertions) {
  size_t i;
  int fin;

  *insertions = 0;

  for (i = 0; i < len; ++i) {
    uint8_t b = data[i];

    switch (p->state) {
    case QM_ES_OPCODE:
      switch (b >> 5) {
      case 0:
        p->opcode = QM_ES_OP_DUPLICATE;
        fin = qm_vi_first(&p->vi, 5, b);
        if (fin < 0) return -1;
        if (fin) {
          ++*insertions;
          qm_es_parser_init(p);
        } else {
          p->state = QM_ES_INDEX;
        }
        break;
      case 1:
        p->opcode = QM_ES_OP_SET_CAP;
        fin = qm_vi_first(&p->vi, 5, b);
        if (fin < 0) return -1;
        p->state = fin ? QM_ES_OPCODE : QM_ES_INDEX;
        if (fin) qm_vi_reset(&p->vi);
        break;
      case 2:
      case 3:
        p->opcode = QM_ES_OP_INSERT_LITERAL;
        fin = qm_vi_first(&p->vi, 5, b);
        if (fin < 0) return -1;
        if (fin) {
          p->skip = p->vi.value;
          p->state = QM_ES_NAME_DATA;
          if (p->skip == 0) {
            qm_vi_reset(&p->vi);
            p->state = QM_ES_VALUE_LEN;
          }
        } else {
          p->state = QM_ES_NAME_LEN;
        }
        break;
      default:
        p->opcode = QM_ES_OP_INSERT_INDEXED;
        fin = qm_vi_first(&p->vi, 6, b);
        if (fin < 0) return -1;
        if (fin) {
          qm_vi_reset(&p->vi);
          p->state = QM_ES_VALUE_LEN;
        } else {
          p->state = QM_ES_INDEX;
        }
        break;
      }
      break;

    case QM_ES_INDEX:
      fin = qm_vi_next(&p->vi, b);
      if (fin < 0) return -1;
      if (!fin) break;

      if (p->opcode == QM_ES_OP_DUPLICATE) {
        ++*insertions;
        qm_es_parser_init(p);
      } else if (p->opcode == QM_ES_OP_SET_CAP) {
        qm_es_parser_init(p);
      } else if (p->opcode == QM_ES_OP_INSERT_INDEXED) {
        qm_vi_reset(&p->vi);
        p->state = QM_ES_VALUE_LEN;
      } else {
        return -1;
      }
      break;

    case QM_ES_NAME_LEN:
      fin = qm_vi_next(&p->vi, b);
      if (fin < 0) return -1;
      if (!fin) break;
      p->skip = p->vi.value;
      p->state = QM_ES_NAME_DATA;
      if (p->skip == 0) {
        qm_vi_reset(&p->vi);
        p->state = QM_ES_VALUE_LEN;
      }
      break;

    case QM_ES_NAME_DATA:
      if (p->skip == 0) return -1;
      --p->skip;
      if (p->skip == 0) {
        qm_vi_reset(&p->vi);
        p->state = QM_ES_VALUE_LEN;
      }
      break;

    case QM_ES_VALUE_LEN:
      if (!p->vi.started) {
        fin = qm_vi_first(&p->vi, 7, b);
      } else {
        fin = qm_vi_next(&p->vi, b);
      }
      if (fin < 0) return -1;
      if (!fin) break;
      if (qm_es_value_len_done(p)) {
        ++*insertions;
        qm_es_parser_init(p);
      }
      break;

    case QM_ES_VALUE_DATA:
      if (p->skip == 0) return -1;
      --p->skip;
      if (p->skip == 0) {
        ++*insertions;
        qm_es_parser_init(p);
      }
      break;
    }
  }

  return 0;
}

void qm_fb_parser_init(qm_fb_parser *p) {
  memset(p, 0, sizeof(*p));
  p->state = QM_FB_OPCODE;
  qm_vi_reset(&p->vi);
}

/* ------------------------------ diagnostics ----------------------------- */

void qm_diag_set(qm_model *m, int category, const char *op,
                 const char *kind, int has_stream, int64_t stream,
                 const char *expected, const char *actual,
                 const char *detail) {
  qm_diag *d = &m->diag;
  memset(d, 0, sizeof(*d));
  d->category = category;
  d->step = m->step;
  d->line = m->line;
  d->has_stream = has_stream;
  d->stream = stream;
  if (op) snprintf(d->op, sizeof(d->op), "%s", op);
  if (kind) snprintf(d->kind, sizeof(d->kind), "%s", kind);
  if (expected) snprintf(d->expected, sizeof(d->expected), "%s", expected);
  if (actual) snprintf(d->actual, sizeof(d->actual), "%s", actual);
  if (detail) snprintf(d->detail, sizeof(d->detail), "%s", detail);
}

int qm_fail(qm_model *m, int category, const char *op, const char *kind,
            int has_stream, int64_t stream, const char *expected,
            const char *actual, const char *detail) {
  qm_diag_set(m, category, op, kind, has_stream, stream, expected, actual,
              detail);
  return category == QM_CATEGORY_SCRIPT ? 1 : 2;
}

/* ------------------------------- streams -------------------------------- */

qm_stream *qm_model_find_stream(qm_model *m, int64_t id) {
  size_t i;
  for (i = 0; i < m->nstreams; ++i) {
    if (m->streams[i]->id == id) return m->streams[i];
  }
  return NULL;
}

qm_stream *qm_model_get_stream(qm_model *m, int64_t id) {
  qm_stream *st;
  qm_stream **grown;

  st = qm_model_find_stream(m, id);
  if (st) return st;

  st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  st->id = id;
  qm_bytes_init(&st->request_q);
  if (nghttp3_qpack_stream_context_new(&st->sctx, id, m->mem) != 0) {
    free(st);
    return NULL;
  }

  if (m->nstreams == m->stream_cap) {
    size_t ncap = m->stream_cap ? m->stream_cap * 2 : 8;
    grown = realloc(m->streams, ncap * sizeof(*grown));
    if (!grown) {
      nghttp3_qpack_stream_context_del(st->sctx);
      free(st);
      return NULL;
    }
    m->streams = grown;
    m->stream_cap = ncap;
  }
  m->streams[m->nstreams++] = st;
  return st;
}

size_t qm_stream_ref_count(const qm_stream *st) {
  return st->nrefs - st->ref_head;
}

int qm_stream_append_ref(qm_stream *st, uint64_t section_id, uint64_t ric) {
  qm_ref *grown;

  if (st->nrefs == st->ref_cap) {
    size_t ncap = st->ref_cap ? st->ref_cap * 2 : 4;
    grown = realloc(st->refs, ncap * sizeof(*grown));
    if (!grown) return -1;
    st->refs = grown;
    st->ref_cap = ncap;
  }
  st->refs[st->nrefs].section_id = section_id;
  st->refs[st->nrefs].ric = ric;
  ++st->nrefs;
  return 0;
}

int qm_stream_pop_oldest_ref(qm_stream *st, qm_ref *out) {
  if (qm_stream_ref_count(st) == 0) return -1;
  if (out) *out = st->refs[st->ref_head];
  ++st->ref_head;
  if (st->ref_head == st->nrefs) {
    st->ref_head = 0;
    st->nrefs = 0;
  }
  return 0;
}

void qm_stream_clear_refs(qm_stream *st) {
  st->ref_head = 0;
  st->nrefs = 0;
}

void qm_section_free(qm_section *s) {
  if (!s) return;
  free(s->name);
  free(s->value);
  free(s->got_name);
  free(s->got_value);
  free(s);
}

/* ------------------------------- lifecycle ------------------------------ */

int qm_model_init(qm_model *m, size_t capacity, size_t blocked_limit,
                  FILE *out) {
  int rv;

  memset(m, 0, sizeof(*m));
  m->mem = nghttp3_mem_default();
  m->capacity = capacity;
  m->blocked_limit = blocked_limit;
  m->map_seed = 7;
  m->out = out;
  m->next_section_id = 1;

  qm_bytes_init(&m->encoder_q);
  qm_bytes_init(&m->feedback_q);
  qm_es_parser_init(&m->es_parser);
  qm_fb_parser_init(&m->fb_parser);

  rv = nghttp3_qpack_encoder_new2(&m->enc, capacity, m->map_seed, m->mem);
  if (rv != 0) return QM_CATEGORY_RUNTIME;
  rv = nghttp3_qpack_decoder_new(&m->dec, capacity, blocked_limit, m->mem);
  if (rv != 0) {
    nghttp3_qpack_encoder_del(m->enc);
    return QM_CATEGORY_RUNTIME;
  }

  nghttp3_qpack_encoder_set_max_dtable_capacity(m->enc, capacity);
  nghttp3_qpack_encoder_set_max_blocked_streams(m->enc, blocked_limit);
  nghttp3_qpack_encoder_set_indexing_strat(
    m->enc, NGHTTP3_QPACK_INDEXING_STRAT_EAGER);
  nghttp3_qpack_decoder_set_max_dtable_capacity(m->dec, capacity);

  return 0;
}

void qm_model_destroy(qm_model *m) {
  size_t i;

  if (!m) return;

  for (i = 0; i < m->nstreams; ++i) {
    qm_stream *st = m->streams[i];
    size_t j;
    for (j = 0; j < st->nsections; ++j) {
      qm_section_free(st->sections[j]);
    }
    free(st->sections);
    free(st->refs);
    qm_bytes_free(&st->request_q);
    nghttp3_qpack_stream_context_del(st->sctx);
    free(st);
  }
  free(m->streams);

  qm_bytes_free(&m->encoder_q);
  qm_bytes_free(&m->feedback_q);

  if (m->enc) nghttp3_qpack_encoder_del(m->enc);
  if (m->dec) nghttp3_qpack_decoder_del(m->dec);
}

/* ----------------------------- RIC reconstruction ----------------------- */

int qm_reconstruct_ric(const qm_model *m, uint64_t encoded_ric,
                       uint64_t current_icnt, uint64_t *ric) {
  uint64_t max_entries, full, max_count, wrapped, value;

  if (encoded_ric == 0) {
    *ric = 0;
    return 0;
  }
  max_entries = m->capacity / NGHTTP3_QPACK_ENTRY_OVERHEAD;
  if (max_entries == 0) return -1;
  full = 2 * max_entries;
  if (encoded_ric > full) return -1;

  max_count = current_icnt + max_entries;
  wrapped = max_count / full * full;
  value = wrapped + encoded_ric - 1;
  if (value > max_count) {
    if (value <= full) return -1;
    value -= full;
  }
  if (value == 0) return -1;
  *ric = value;
  return 0;
}

/* ------------------------------ state check ----------------------------- */

static int qm_check_refs(qm_model *m, const char *where) {
  size_t i;
  size_t actual_stream_count = nghttp3_map_size(&m->enc->streams);
  size_t expected_live_streams = 0;

  for (i = 0; i < m->nstreams; ++i) {
    qm_stream *st = m->streams[i];
    size_t expected_refs = qm_stream_ref_count(st);
    nghttp3_qpack_stream *ps =
      nghttp3_qpack_encoder_find_stream(m->enc, st->id);
    char exp[160], act[160];

    if (expected_refs > 0) ++expected_live_streams;

    if (expected_refs == 0) {
      if (ps != NULL) {
        snprintf(exp, sizeof(exp), "no encoder stream object");
        snprintf(act, sizeof(act), "stream %" PRId64 " retains %zu refs",
                 st->id, nghttp3_ringbuf_len(&ps->refs));
        return qm_fail(m, QM_CATEGORY_MODEL, where, "encoder_ref_stream",
                       1, st->id, exp, act,
                       "independent queue is empty but production retains it");
      }
      continue;
    }

    if (!ps) {
      snprintf(exp, sizeof(exp), "%zu live refs", expected_refs);
      snprintf(act, sizeof(act), "missing encoder stream");
      return qm_fail(m, QM_CATEGORY_MODEL, where, "encoder_ref_missing",
                     1, st->id, exp, act, NULL);
    }

    if (nghttp3_ringbuf_len(&ps->refs) != expected_refs) {
      snprintf(exp, sizeof(exp), "%zu refs", expected_refs);
      snprintf(act, sizeof(act), "%zu refs",
               nghttp3_ringbuf_len(&ps->refs));
      return qm_fail(m, QM_CATEGORY_MODEL, where, "encoder_ref_count",
                     1, st->id, exp, act, NULL);
    }

    for (size_t j = 0; j < expected_refs; ++j) {
      nghttp3_qpack_header_block_ref *ref =
        *(nghttp3_qpack_header_block_ref **)nghttp3_ringbuf_get(&ps->refs, j);
      uint64_t expected_ric = st->refs[st->ref_head + j].ric;
      if (ref->max_cnt != expected_ric) {
        snprintf(exp, sizeof(exp), "ref %zu RIC %" PRIu64, j, expected_ric);
        snprintf(act, sizeof(act), "ref %zu RIC %" PRIu64, j, ref->max_cnt);
        return qm_fail(m, QM_CATEGORY_MODEL, where, "encoder_ref_ric",
                       1, st->id, exp, act, NULL);
      }
    }
  }

  if (actual_stream_count != expected_live_streams) {
    char exp[64], act[64];
    snprintf(exp, sizeof(exp), "%zu live streams", expected_live_streams);
    snprintf(act, sizeof(act), "%zu live streams", actual_stream_count);
    return qm_fail(m, QM_CATEGORY_MODEL, where, "encoder_stream_set",
                   0, 0, exp, act, NULL);
  }

  return 0;
}

static int qm_check_frontiers(qm_model *m, const char *where) {
  char exp[128], act[128];

  if (m->enc->ctx.next_absidx != m->produced_icnt) {
    snprintf(exp, sizeof(exp), "produced insert count %" PRIu64,
             m->produced_icnt);
    snprintf(act, sizeof(act), "encoder next_absidx %" PRIu64,
             m->enc->ctx.next_absidx);
    return qm_fail(m, QM_CATEGORY_MODEL, where, "produced_insert_frontier",
                   0, 0, exp, act, NULL);
  }

  if (m->dec->ctx.next_absidx != m->decoder_icnt) {
    snprintf(exp, sizeof(exp), "delivered insert count %" PRIu64,
             m->decoder_icnt);
    snprintf(act, sizeof(act), "decoder insert count %" PRIu64,
             m->dec->ctx.next_absidx);
    return qm_fail(m, QM_CATEGORY_MODEL, where, "decoder_insert_frontier",
                   0, 0, exp, act, NULL);
  }

  if (m->enc->krcnt != m->known_icnt) {
    snprintf(exp, sizeof(exp), "known received count %" PRIu64,
             m->known_icnt);
    snprintf(act, sizeof(act), "encoder krcnt %" PRIu64, m->enc->krcnt);
    return qm_fail(m, QM_CATEGORY_MODEL, where, "known_received_frontier",
                   0, 0, exp, act,
                   "computed independently from ACK/ICI/CANCEL feedback");
  }

  if (m->enc->ctx.bad || m->dec->ctx.bad) {
    return qm_fail(m, QM_CATEGORY_MODEL, where, "qpack_bad_state", 0, 0,
                   "healthy endpoints", "bad/fatal endpoint", NULL);
  }

  return 0;
}

static int qm_check_blocked(qm_model *m, const char *where) {
  size_t i;
  size_t expected_encoder_blocked = 0;
  size_t expected_decoder_blocked = 0;
  size_t actual_encoder_blocked =
    nghttp3_qpack_encoder_get_num_blocked_streams2(m->enc);

  for (i = 0; i < m->nstreams; ++i) {
    qm_stream *st = m->streams[i];
    nghttp3_qpack_stream *ps =
      nghttp3_qpack_encoder_find_stream(m->enc, st->id);
    int actual_dec_blocked =
      st->sctx->state == NGHTTP3_QPACK_RS_STATE_BLOCKED;
    int expected_enc_blocked = 0;
    int actual_enc_blocked = 0;
    uint64_t max_ric = 0;

    if (st->decoder_blocked) ++expected_decoder_blocked;

    if (st->decoder_blocked != actual_dec_blocked) {
      char exp[64], act[96];
      snprintf(exp, sizeof(exp), "%sblocked",
               st->decoder_blocked ? "" : "not ");
      snprintf(act, sizeof(act), "sctx state %d", (int)st->sctx->state);
      return qm_fail(m, QM_CATEGORY_MODEL, where, "decoder_blocked_stream",
                     1, st->id, exp, act, NULL);
    }

    if (qm_stream_ref_count(st) > 0) {
      size_t j;
      max_ric = 0;
      for (j = st->ref_head; j < st->nrefs; ++j) {
        if (st->refs[j].ric > max_ric) max_ric = st->refs[j].ric;
      }
      expected_enc_blocked = max_ric > m->known_icnt;
      if (expected_enc_blocked) ++expected_encoder_blocked;
    }

    if (ps) {
      actual_enc_blocked =
        nghttp3_qpack_encoder_stream_is_blocked(m->enc, ps) != 0;
    }
    if (actual_enc_blocked != expected_enc_blocked) {
      char exp[96], act[96];
      snprintf(exp, sizeof(exp), "%sblocked (max RIC %" PRIu64
               ", known %" PRIu64 ")",
               expected_enc_blocked ? "" : "not ", max_ric, m->known_icnt);
      snprintf(act, sizeof(act), "%sblocked by production predicate",
               actual_enc_blocked ? "" : "not ");
      return qm_fail(m, QM_CATEGORY_MODEL, where, "encoder_blocked_stream",
                     1, st->id, exp, act, NULL);
    }
  }

  if (actual_encoder_blocked != expected_encoder_blocked) {
    char exp[64], act[64];
    snprintf(exp, sizeof(exp), "%zu encoder-blocked streams",
             expected_encoder_blocked);
    snprintf(act, sizeof(act), "%zu encoder-blocked streams",
             actual_encoder_blocked);
    return qm_fail(m, QM_CATEGORY_MODEL, where, "encoder_blocked_set",
                   0, 0, exp, act, NULL);
  }

  (void)expected_decoder_blocked;
  return 0;
}

static int qm_check_prefix_ric(qm_model *m, const char *where) {
  size_t i;

  for (i = 0; i < m->nstreams; ++i) {
    qm_stream *st = m->streams[i];
    qm_section *sec;
    size_t consumed;
    uint64_t actual_ric;
    char exp[96], act[96];

    if (st->nsections == 0 || st->canceled) continue;
    sec = st->sections[0];
    consumed = sec->section_len - sec->remain;
    if (consumed < sec->prefix_len) continue;

    actual_ric = nghttp3_qpack_stream_context_get_ricnt2(st->sctx);
    if (actual_ric != sec->ric) {
      snprintf(exp, sizeof(exp), "independent prefix RIC %" PRIu64,
               sec->ric);
      snprintf(act, sizeof(act), "decoder sctx RIC %" PRIu64, actual_ric);
      return qm_fail(m, QM_CATEGORY_MODEL, where, "prefix_ric_frontier",
                     1, st->id, exp, act, NULL);
    }
  }
  return 0;
}

int qm_check_state(qm_model *m, const char *where) {
  int rv;

  rv = qm_check_frontiers(m, where);
  if (rv) return rv;
  rv = qm_check_prefix_ric(m, where);
  if (rv) return rv;
  rv = qm_check_refs(m, where);
  if (rv) return rv;
  rv = qm_check_blocked(m, where);
  if (rv) return rv;
  return 0;
}
