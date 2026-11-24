#include "fuzz/stateful/model.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define QM_PUMP_LIMIT 1000000U
#define QM_END_ITER_LIMIT 4096U
#define QM_ALL_BYTES UINT64_MAX

static const uint8_t qm_empty_bytes[1] = {0};

static void qm_record_zero(qm_model *m, const char *op, int64_t stream,
                           int has_stream, uint64_t requested,
                           size_t queued, const char *reason) {
  ++m->zero_records;
  if (has_stream) {
    fprintf(m->out,
            "ZERO_PROGRESS step=%lu line=%lu op=%s stream=%" PRId64
            " requested=%" PRIu64 " consumed=0 queued=%zu reason=%s\n",
            m->step, m->line, op, stream, requested, queued,
            reason ? reason : "no_bytes");
  } else {
    fprintf(m->out,
            "ZERO_PROGRESS step=%lu line=%lu op=%s stream=- requested=%" PRIu64
            " consumed=0 queued=%zu reason=%s\n",
            m->step, m->line, op, requested, queued,
            reason ? reason : "no_bytes");
  }
}

static void qm_record_blocked(qm_model *m, int64_t stream, uint64_t ric,
                              size_t consumed) {
  ++m->blocked_records;
  fprintf(m->out,
          "BLOCKED step=%lu line=%lu op=REQUEST stream=%" PRId64
          " ric=%" PRIu64 " decoder_icnt=%" PRIu64 " consumed=%zu\n",
          m->step, m->line, stream, ric, m->decoder_icnt, consumed);
}

static int qm_section_push(qm_stream *st, qm_section *sec) {
  qm_section **grown;
  if (st->nsections == st->section_cap) {
    size_t ncap = st->section_cap ? st->section_cap * 2 : 4;
    grown = realloc(st->sections, ncap * sizeof(*grown));
    if (!grown) return -1;
    st->sections = grown;
    st->section_cap = ncap;
  }
  st->sections[st->nsections++] = sec;
  return 0;
}

/* ------------------------------- SECTION -------------------------------- */

int qm_cmd_section(qm_model *m, int64_t stream_id,
                   const uint8_t *name, size_t namelen,
                   const uint8_t *value, size_t valuelen) {
  qm_stream *st;
  qm_section *sec = NULL;
  nghttp3_qpack_encoder *e = m->enc;
  nghttp3_nv nv;
  nghttp3_buf pbuf, rbuf, ebuf;
  size_t prefix_consumed1 = 0, prefix_consumed2 = 0;
  const uint8_t *prefix_p;
  uint64_t encoded_ric = 0, delta_base = 0;
  uint64_t produced_insertions = 0;
  qm_es_parser produced_parser;
  int rv;

  st = qm_model_find_stream(m, stream_id);
  if (st && st->canceled) {
    return qm_fail(m, QM_CATEGORY_SCRIPT, "SECTION", "section_on_canceled",
                   1, stream_id, "non-canceled stream", "canceled stream",
                   "canceled streams cannot receive new sections");
  }
  st = qm_model_get_stream(m, stream_id);
  if (!st) {
    return qm_fail(m, QM_CATEGORY_RUNTIME, "SECTION", "alloc_stream",
                   1, stream_id, "allocation", "NULL", NULL);
  }

  sec = calloc(1, sizeof(*sec));
  if (!sec) {
    return qm_fail(m, QM_CATEGORY_RUNTIME, "SECTION", "alloc_section",
                   1, stream_id, "allocation", "NULL", NULL);
  }
  sec->id = m->next_section_id++;
  sec->stream_id = stream_id;
  sec->name = malloc(namelen ? namelen : 1);
  sec->value = malloc(valuelen ? valuelen : 1);
  if (!sec->name || !sec->value) {
    free(sec->name);
    free(sec->value);
    free(sec);
    return qm_fail(m, QM_CATEGORY_RUNTIME, "SECTION", "alloc_field",
                   1, stream_id, "allocation", "NULL", NULL);
  }
  memcpy(sec->name, name, namelen);
  sec->namelen = namelen;
  memcpy(sec->value, value, valuelen);
  sec->valuelen = valuelen;

  nv.name = sec->name;
  nv.value = sec->value;
  nv.namelen = namelen;
  nv.valuelen = valuelen;
  nv.flags = NGHTTP3_NV_FLAG_NONE;

  nghttp3_buf_init(&pbuf);
  nghttp3_buf_init(&rbuf);
  nghttp3_buf_init(&ebuf);

  rv = nghttp3_qpack_encoder_encode(e, &pbuf, &rbuf, &ebuf, stream_id,
                                    &nv, 1);
  if (rv != 0) {
    qm_section_free(sec);
    nghttp3_buf_free(&pbuf, m->mem);
    nghttp3_buf_free(&rbuf, m->mem);
    nghttp3_buf_free(&ebuf, m->mem);
    return qm_fail(m, QM_CATEGORY_RUNTIME, "SECTION", "encoder_encode",
                   1, stream_id, "0", "negative qpack error",
                   "normal validated field was rejected by real encoder");
  }

  /* Independently find complete encoder-stream instruction boundaries and
     advance the produced-insert frontier only by insert instructions. */
  qm_es_parser_init(&produced_parser);
  if (qm_es_parser_feed(&produced_parser, ebuf.pos,
                        nghttp3_buf_len(&ebuf),
                        &produced_insertions) != 0 ||
      produced_parser.state != QM_ES_OPCODE) {
    qm_section_free(sec);
    nghttp3_buf_free(&pbuf, m->mem);
    nghttp3_buf_free(&rbuf, m->mem);
    nghttp3_buf_free(&ebuf, m->mem);
    return qm_fail(m, QM_CATEGORY_MODEL, "SECTION",
                   "encoder_instruction_boundary", 1, stream_id,
                   "complete instructions", "partial/invalid boundary parse",
                   NULL);
  }
  m->produced_icnt += produced_insertions;

  if (qm_bytes_append(&m->encoder_q, ebuf.pos, nghttp3_buf_len(&ebuf)) != 0) {
    qm_section_free(sec);
    rv = QM_CATEGORY_RUNTIME;
    goto section_cleanup_buffers;
  }

  /* Parse the real Encoded Field Section Prefix independently. */
  if (qm_varint_decode(pbuf.pos, nghttp3_buf_len(&pbuf), 8, &encoded_ric,
                       &prefix_consumed1) != 1 ||
      prefix_consumed1 >= nghttp3_buf_len(&pbuf)) {
    rv = qm_fail(m, QM_CATEGORY_MODEL, "SECTION", "prefix_ric",
                 1, stream_id, "complete RIC varint", "truncated prefix",
                 NULL);
    qm_section_free(sec);
    goto section_cleanup_buffers;
  }
  prefix_p = pbuf.pos + prefix_consumed1;
  sec->base_sign = (*prefix_p & 0x80U) != 0;
  if (qm_varint_decode(prefix_p,
                       nghttp3_buf_len(&pbuf) - prefix_consumed1, 7,
                       &delta_base, &prefix_consumed2) != 1) {
    rv = qm_fail(m, QM_CATEGORY_MODEL, "SECTION", "prefix_base",
                 1, stream_id, "complete Base varint", "truncated prefix",
                 NULL);
    qm_section_free(sec);
    goto section_cleanup_buffers;
  }
  sec->encoded_ric = encoded_ric;
  if (qm_reconstruct_ric(m, encoded_ric, m->produced_icnt, &sec->ric) != 0) {
    rv = qm_fail(m, QM_CATEGORY_MODEL, "SECTION", "prefix_ric_reconstruct",
                 1, stream_id, "valid RIC", "wrap reconstruction failed",
                 NULL);
    qm_section_free(sec);
    goto section_cleanup_buffers;
  }
  if (sec->base_sign) {
    sec->base = sec->ric - delta_base - 1;
  } else {
    sec->base = sec->ric + delta_base;
  }

  sec->section_len = nghttp3_buf_len(&pbuf) + nghttp3_buf_len(&rbuf);
  sec->prefix_len = prefix_consumed1 + prefix_consumed2;
  sec->remain = sec->section_len;
  if (qm_bytes_append(&st->request_q, pbuf.pos,
                       nghttp3_buf_len(&pbuf)) != 0 ||
      qm_bytes_append(&st->request_q, rbuf.pos,
                      nghttp3_buf_len(&rbuf)) != 0 ||
      qm_section_push(st, sec) != 0) {
    rv = qm_fail(m, QM_CATEGORY_RUNTIME, "SECTION", "queue_append",
                 1, stream_id, "allocation", "failure", NULL);
    qm_section_free(sec);
    goto section_cleanup_buffers;
  }

  /* A pending reference exists only for a nonzero Required Insert Count. */
  if (sec->ric != 0) {
    if (qm_stream_append_ref(st, sec->id, sec->ric) != 0) {
      rv = qm_fail(m, QM_CATEGORY_RUNTIME, "SECTION", "ref_append",
                   1, stream_id, "allocation", "failure", NULL);
      goto section_cleanup_buffers;
    }
  }
  ++st->submitted;

  nghttp3_buf_free(&pbuf, m->mem);
  nghttp3_buf_free(&rbuf, m->mem);
  nghttp3_buf_free(&ebuf, m->mem);

  return qm_check_state(m, "SECTION");

section_cleanup_buffers:
  nghttp3_buf_free(&pbuf, m->mem);
  nghttp3_buf_free(&rbuf, m->mem);
  nghttp3_buf_free(&ebuf, m->mem);
  return rv ? rv : 3;
}

/* ------------------------------- ENCODER -------------------------------- */

int qm_cmd_encoder(qm_model *m, uint64_t max_bytes) {
  size_t queued = m->encoder_q.len;
  size_t want = max_bytes >= queued ? queued : (size_t)max_bytes;
  nghttp3_ssize nread;
  uint64_t insertions = 0;

  const uint8_t *src = m->encoder_q.data ? m->encoder_q.data
                                         : qm_empty_bytes;
  nread = nghttp3_qpack_decoder_read_encoder(m->dec, src, want);
  if (nread < 0) {
    return qm_fail(m, QM_CATEGORY_MODEL, "ENCODER", "read_encoder", 0, 0,
                   "nonnegative consumed length", "negative QPACK error",
                   "normal encoder output was rejected by decoder");
  }
  if ((size_t)nread > want) {
    return qm_fail(m, QM_CATEGORY_MODEL, "ENCODER", "consumed_bounds", 0, 0,
                   "nread <= requested", "nread beyond request", NULL);
  }

  if (qm_es_parser_feed(&m->es_parser, m->encoder_q.data, (size_t)nread,
                        &insertions) != 0) {
    return qm_fail(m, QM_CATEGORY_MODEL, "ENCODER",
                   "delivered_instruction_boundary", 0, 0,
                   "parseable complete/partial instruction stream",
                   "independent parser failed", NULL);
  }
  m->decoder_icnt += insertions;
  qm_bytes_consume(&m->encoder_q, (size_t)nread);

  if (nread == 0) {
    qm_record_zero(m, "ENCODER", 0, 0, max_bytes, queued,
                   max_bytes == 0 ? "paused" : "empty_queue");
  }

  return qm_check_state(m, "ENCODER");
}

/* ------------------------------- REQUEST -------------------------------- */

static int qm_copy_emit(qm_model *m, qm_stream *st, qm_section *sec,
                        nghttp3_qpack_nv *nv) {
  nghttp3_vec name = nghttp3_rcbuf_get_buf(nv->name);
  nghttp3_vec value = nghttp3_rcbuf_get_buf(nv->value);
  int ok;

  free(sec->got_name);
  free(sec->got_value);
  sec->got_name = malloc(name.len ? name.len : 1);
  sec->got_value = malloc(value.len ? value.len : 1);
  if (!sec->got_name || !sec->got_value) {
    /* EMIT ownership still has to be released exactly once. */
    nghttp3_rcbuf_decref(nv->name);
    nghttp3_rcbuf_decref(nv->value);
    return qm_fail(m, QM_CATEGORY_RUNTIME, "REQUEST", "emit_copy",
                   1, st->id, "allocation", "NULL", NULL);
  }
  memcpy(sec->got_name, name.base, name.len);
  sec->got_namelen = name.len;
  memcpy(sec->got_value, value.base, value.len);
  sec->got_valuelen = value.len;

  /* The copies own no production reference now. */
  nghttp3_rcbuf_decref(nv->name);
  nghttp3_rcbuf_decref(nv->value);

  ++sec->emit_count;
  ok = sec->got_namelen == sec->namelen &&
       sec->got_valuelen == sec->valuelen &&
       memcmp(sec->got_name, sec->name, sec->namelen) == 0 &&
       memcmp(sec->got_value, sec->value, sec->valuelen) == 0;
  sec->field_matches = ok;
  if (!ok) {
    return qm_fail(m, QM_CATEGORY_MODEL, "REQUEST", "emitted_field",
                   1, st->id, "emitted name/value equals SECTION",
                   "emitted bytes differ", NULL);
  }
  return 0;
}

static int qm_finish_section(qm_model *m, qm_stream *st) {
  qm_section *sec = st->sections[0];

  if (sec->remain != 0) {
    return qm_fail(m, QM_CATEGORY_MODEL, "REQUEST", "final_with_tail",
                   1, st->id, "all section bytes consumed",
                   "FINAL with unconsumed tail", NULL);
  }
  if (sec->emit_count != 1 || !sec->field_matches) {
    return qm_fail(m, QM_CATEGORY_MODEL, "REQUEST", "final_emit_count",
                   1, st->id, "one matching EMIT before FINAL",
                   sec->emit_count == 0 ? "FINAL without EMIT"
                                        : "bad EMIT state",
                   "zero-byte progress is not completion without FINAL");
  }

  qm_section_free(sec);
  memmove(st->sections, st->sections + 1,
          (st->nsections - 1) * sizeof(*st->sections));
  --st->nsections;
  ++st->completed;
  ++m->completed_sections;

  /* Reuse the real decoder context for the next same-stream section. */
  nghttp3_qpack_stream_context_reset(st->sctx);
  st->decoder_blocked = 0;
  return 0;
}

int qm_cmd_request(qm_model *m, int64_t stream_id, uint64_t max_bytes) {
  qm_stream *st = qm_model_find_stream(m, stream_id);
  size_t queued;
  size_t budget;
  size_t used = 0;
  unsigned pumps = 0;
  int rv;

  if (!st) {
    return qm_fail(m, QM_CATEGORY_SCRIPT, "REQUEST", "unknown_stream",
                   1, stream_id, "SECTION first", "missing stream context",
                   NULL);
  }
  if (st->canceled) {
    qm_record_zero(m, "REQUEST", stream_id, 1, max_bytes, 0,
                   "canceled_bytes_discarded");
    return qm_check_state(m, "REQUEST");
  }

  queued = st->request_q.len;
  budget = max_bytes >= queued ? queued : (size_t)max_bytes;

  for (;;) {
    qm_section *sec;
    size_t want;
    int fin;
    uint8_t flags = 0;
    nghttp3_qpack_nv nv;
    nghttp3_ssize nread;

    if (st->nsections == 0) break;
    sec = st->sections[0];

    want = sec->remain;
    if (want > st->request_q.len) want = st->request_q.len;
    if (used < budget) {
      size_t left = budget - used;
      if (want > left) want = left;
    } else {
      want = 0;
    }
    fin = want == sec->remain;

    const uint8_t *request_src = st->request_q.data
      ? st->request_q.data : qm_empty_bytes;

    nread = nghttp3_qpack_decoder_read_request(
      m->dec, st->sctx, &nv, &flags, request_src, want, fin);
    if (nread < 0) {
      return qm_fail(m, QM_CATEGORY_MODEL, "REQUEST", "read_request",
                     1, stream_id, "nonnegative consumed length",
                     "negative QPACK decode error",
                     "normal segmented input was rejected");
    }
    if ((size_t)nread > want) {
      return qm_fail(m, QM_CATEGORY_MODEL, "REQUEST", "consumed_bounds",
                     1, stream_id, "nread <= requested",
                     "nread beyond request", NULL);
    }

    qm_bytes_consume(&st->request_q, (size_t)nread);
    sec->remain -= (size_t)nread;
    used += (size_t)nread;
    ++pumps;

    if (flags & NGHTTP3_QPACK_DECODE_FLAG_EMIT) {
      rv = qm_copy_emit(m, st, sec, &nv);
      if (rv) return rv;
    }

    if (flags & NGHTTP3_QPACK_DECODE_FLAG_BLOCKED) {
      st->decoder_blocked = 1;
      qm_record_blocked(m, stream_id,
                        nghttp3_qpack_stream_context_get_ricnt2(st->sctx),
                        used);
      break;
    }
    st->decoder_blocked = 0;

    if (flags & NGHTTP3_QPACK_DECODE_FLAG_FINAL) {
      rv = qm_finish_section(m, st);
      if (rv) return rv;
      /* A section boundary can make more queued bytes eligible. */
      if (st->nsections > 0 && pumps < QM_PUMP_LIMIT) continue;
      break;
    }

    if (nread == 0) {
      /* Explicit zero-byte progress.  It is never itself completion. */
      break;
    }

    if (pumps > QM_PUMP_LIMIT) {
      return qm_fail(m, QM_CATEGORY_MODEL, "REQUEST", "pump_bound",
                     1, stream_id, "bounded decoder progress",
                     "pump limit exceeded", NULL);
    }
  }

  if (used == 0) {
    const char *reason = max_bytes == 0 ? "paused" :
                         st->decoder_blocked ? "blocked" : "no_progress";
    qm_record_zero(m, "REQUEST", stream_id, 1, max_bytes, queued, reason);
  }

  return qm_check_state(m, "REQUEST");
}

/* ------------------------------- FEEDBACK ------------------------------- */

static int qm_feedback_apply(qm_model *m, int opcode, uint64_t value) {
  qm_stream *st;
  qm_ref ref;

  switch (opcode) {
  case QM_FB_ICNT:
    /* Insert Count Increment confirms insertion frontier only. */
    m->known_icnt += value;
    return 0;

  case QM_FB_ACK:
    st = qm_model_find_stream(m, (int64_t)value);
    if (!st || qm_stream_ref_count(st) == 0) {
      return qm_fail(m, QM_CATEGORY_MODEL, "FEEDBACK", "unexpected_ack",
                     1, (int64_t)value, "oldest dynamic RIC reference",
                     "no independent pending reference",
                     "ACK must remove only the oldest dynamic section");
    }
    if (qm_stream_pop_oldest_ref(st, &ref) != 0) {
      return qm_fail(m, QM_CATEGORY_MODEL, "FEEDBACK", "ack_pop",
                     1, (int64_t)value, "oldest ref", "pop failed", NULL);
    }
    if (m->known_icnt < ref.ric) m->known_icnt = ref.ric;
    return 0;

  case QM_FB_CANCEL:
    st = qm_model_find_stream(m, (int64_t)value);
    if (st) {
      /* CANCEL removes every reference on this stream, but deliberately
         leaves known_icnt unchanged. */
      qm_stream_clear_refs(st);
    }
    return 0;
  }

  return qm_fail(m, QM_CATEGORY_MODEL, "FEEDBACK", "bad_opcode", 0, 0,
                 "ICI/ACK/CANCEL", "unknown opcode", NULL);
}

static int qm_feedback_consume(qm_model *m, const uint8_t *data,
                               size_t len) {
  size_t i;
  int fin;

  for (i = 0; i < len; ++i) {
    uint8_t b = data[i];
    qm_fb_parser *p = &m->fb_parser;

    if (p->state == QM_FB_OPCODE) {
      unsigned top = b >> 6;
      if (top == 0) {
        p->opcode = QM_FB_ICNT;
        p->prefix_bits = 6;
      } else if (top == 1) {
        p->opcode = QM_FB_CANCEL;
        p->prefix_bits = 6;
      } else {
        p->opcode = QM_FB_ACK;
        p->prefix_bits = 7;
      }
      fin = qm_vi_first(&p->vi, p->prefix_bits, b);
      if (fin < 0) {
        return qm_fail(m, QM_CATEGORY_MODEL, "FEEDBACK",
                       "feedback_varint_boundary", 0, 0,
                       "valid feedback varint", "overflow", NULL);
      }
      if (fin) {
        int rv = qm_feedback_apply(m, p->opcode, p->vi.value);
        qm_fb_parser_init(p);
        if (rv) return rv;
      } else {
        p->state = QM_FB_NUMBER;
      }
    } else {
      fin = qm_vi_next(&p->vi, b);
      if (fin < 0) {
        return qm_fail(m, QM_CATEGORY_MODEL, "FEEDBACK",
                       "feedback_varint_boundary", 0, 0,
                       "valid continuation", "overflow", NULL);
      }
      if (fin) {
        int rv = qm_feedback_apply(m, p->opcode, p->vi.value);
        qm_fb_parser_init(p);
        if (rv) return rv;
      }
    }
  }
  return 0;
}

int qm_cmd_feedback(qm_model *m, uint64_t max_bytes) {
  size_t pending;
  uint8_t *tmp;
  nghttp3_buf wb;
  size_t written;
  size_t queued;
  size_t want;
  nghttp3_ssize nread;
  int rv;

  /* First drain everything the real decoder currently wants to say into
     the persistent feedback queue.  Appending never overwrites a half
     instruction already waiting at the front. */
  pending = nghttp3_qpack_decoder_get_decoder_streamlen2(m->dec);
  if (pending > 0) {
    tmp = malloc(pending ? pending : 1);
    if (!tmp) {
      return qm_fail(m, QM_CATEGORY_RUNTIME, "FEEDBACK", "alloc_pull", 0, 0,
                     "allocation", "NULL", NULL);
    }
    nghttp3_buf_wrap_init(&wb, tmp, pending);
    nghttp3_qpack_decoder_write_decoder(m->dec, &wb);
    written = nghttp3_buf_len(&wb);
    rv = qm_bytes_append(&m->feedback_q, tmp, written);
    free(tmp);
    if (rv != 0) {
      return qm_fail(m, QM_CATEGORY_RUNTIME, "FEEDBACK", "append_pull", 0, 0,
                     "allocation", "failure", NULL);
    }
  }

  queued = m->feedback_q.len;
  want = max_bytes >= queued ? queued : (size_t)max_bytes;
  const uint8_t *feedback_src = m->feedback_q.data
    ? m->feedback_q.data : qm_empty_bytes;
  nread = nghttp3_qpack_encoder_read_decoder(m->enc, feedback_src, want);
  if (nread < 0) {
    return qm_fail(m, QM_CATEGORY_MODEL, "FEEDBACK", "read_decoder", 0, 0,
                   "nonnegative consumed length",
                   "negative encoder feedback error",
                   "normal decoder feedback was rejected");
  }
  if ((size_t)nread > want) {
    return qm_fail(m, QM_CATEGORY_MODEL, "FEEDBACK", "consumed_bounds", 0, 0,
                   "nread <= requested", "nread beyond request", NULL);
  }

  rv = qm_feedback_consume(m, m->feedback_q.data, (size_t)nread);
  if (rv) return rv;
  qm_bytes_consume(&m->feedback_q, (size_t)nread);

  if (nread == 0) {
    qm_record_zero(m, "FEEDBACK", 0, 0, max_bytes, queued,
                   max_bytes == 0 ? "paused" : "empty_queue");
  }

  return qm_check_state(m, "FEEDBACK");
}

/* -------------------------------- CANCEL -------------------------------- */

int qm_cmd_cancel(qm_model *m, int64_t stream_id) {
  qm_stream *st = qm_model_find_stream(m, stream_id);
  size_t i;
  int rv;

  if (!st) {
    return qm_fail(m, QM_CATEGORY_SCRIPT, "CANCEL", "unknown_stream",
                   1, stream_id, "SECTION first", "missing stream", NULL);
  }
  if (st->canceled) {
    return qm_fail(m, QM_CATEGORY_SCRIPT, "CANCEL", "cancel_twice",
                   1, stream_id, "one CANCEL per stream",
                   "stream already canceled", NULL);
  }
  if (st->nsections == 0) {
    return qm_fail(m, QM_CATEGORY_SCRIPT, "CANCEL", "no_unfinished_section",
                   1, stream_id, "at least one unfinished section",
                   "FIFO has no unfinished section",
                   "completed-but-unacked sections do not satisfy CANCEL rule");
  }

  rv = nghttp3_qpack_decoder_cancel_stream(m->dec, stream_id);
  if (rv != 0) {
    return qm_fail(m, QM_CATEGORY_RUNTIME, "CANCEL", "decoder_cancel",
                   1, stream_id, "0", "negative QPACK error", NULL);
  }

  st->canceled = 1;

  /* Discard this stream's undelivered request bytes and unfinished FIFO.
     Expected refs intentionally survive until the real CANCEL feedback has
     been independently parsed at the encoder byte frontier. */
  qm_bytes_consume(&st->request_q, st->request_q.len);
  for (i = 0; i < st->nsections; ++i) {
    qm_section_free(st->sections[i]);
  }
  st->nsections = 0;
  st->decoder_blocked = 0;
  nghttp3_qpack_stream_context_reset(st->sctx);

  ++m->canceled_streams;
  return qm_check_state(m, "CANCEL");
}

/* --------------------------------- END ---------------------------------- */

static int qm_end_drained(qm_model *m) {
  size_t i;

  if (m->encoder_q.len != 0 || m->feedback_q.len != 0) return 0;
  if (nghttp3_qpack_decoder_get_decoder_streamlen2(m->dec) != 0) return 0;

  for (i = 0; i < m->nstreams; ++i) {
    qm_stream *st = m->streams[i];
    if (st->canceled) continue;
    if (st->nsections != 0 || st->request_q.len != 0) return 0;
    if (st->decoder_blocked ||
        st->sctx->state == NGHTTP3_QPACK_RS_STATE_BLOCKED) {
      return 0;
    }
  }
  return 1;
}

int qm_cmd_end(qm_model *m) {
  unsigned iter;
  size_t i;
  int rv;

  for (iter = 0; iter < QM_END_ITER_LIMIT; ++iter) {
    if (qm_end_drained(m)) {
      return qm_check_state(m, "END");
    }

    if (m->encoder_q.len != 0) {
      rv = qm_cmd_encoder(m, QM_ALL_BYTES);
      if (rv) return rv;
    }

    for (i = 0; i < m->nstreams; ++i) {
      qm_stream *st = m->streams[i];
      if (st->canceled) continue;
      if (st->request_q.len == 0 && st->nsections == 0) continue;
      rv = qm_cmd_request(m, st->id, QM_ALL_BYTES);
      if (rv) return rv;
    }

    if (m->feedback_q.len != 0 ||
        nghttp3_qpack_decoder_get_decoder_streamlen2(m->dec) != 0) {
      rv = qm_cmd_feedback(m, QM_ALL_BYTES);
      if (rv) return rv;
    }

    /* A blocked context may need a zero-length pump after encoder bytes
       were processed.  qm_cmd_request(ALL) already does that while bytes
       remain; this covers the prefix-only/resume corner explicitly. */
    for (i = 0; i < m->nstreams; ++i) {
      qm_stream *st = m->streams[i];
      if (st->canceled || !st->decoder_blocked) continue;
      rv = qm_cmd_request(m, st->id, 0);
      if (rv) return rv;
    }
  }

  return qm_fail(m, QM_CATEGORY_MODEL, "END", "termination_bound", 0, 0,
                 "bounded schedule drains all completable work",
                 "iteration limit reached",
                 "zero progress or blocked state persisted");
}
