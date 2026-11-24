#ifndef FUZZ_STATEFUL_MODEL_H
#define FUZZ_STATEFUL_MODEL_H

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif /* !defined(_POSIX_C_SOURCE) */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include <nghttp3/nghttp3.h>
#include "nghttp3_qpack.h"

#ifdef __cplusplus
extern "C" {
#endif /* defined(__cplusplus) */

/*
 * Public entry point used by the replay CLI and task_support/api_probe.c.
 * A successful replay writes the final COMPLETE/CONSISTENT record to
 * |output|.  An invalid script or a semantic mismatch returns nonzero
 * and writes an ERROR record.
 */
int qpack_model_replay(FILE *input, FILE *output);

/* Deterministically write a legal random schedule. */
int qpack_model_generate(uint64_t seed, unsigned int steps, FILE *output);

/* Byte queue.  Consuming from the front memmoves the unconsumed tail. */
typedef struct qm_bytes {
  uint8_t *data;
  size_t len;
  size_t cap;
} qm_bytes;

void qm_bytes_init(qm_bytes *b);
void qm_bytes_free(qm_bytes *b);
int qm_bytes_reserve(qm_bytes *b, size_t extra);
int qm_bytes_append(qm_bytes *b, const void *data, size_t len);
void qm_bytes_consume(qm_bytes *b, size_t len);

/* Independent QPACK variable integer reader. */
int qm_varint_decode(const uint8_t *p, size_t n, unsigned prefix_bits,
                     uint64_t *value, size_t *consumed);

/* Incremental variable integer used by frontier parsers. */
typedef struct qm_varint_state {
  uint64_t value;
  unsigned shift;
  unsigned prefix_bits;
  int complete;
  int started;
} qm_varint_state;

void qm_vi_reset(qm_varint_state *v);
int qm_vi_first(qm_varint_state *v, unsigned prefix_bits, uint8_t b);
int qm_vi_next(qm_varint_state *v, uint8_t b);

/*
 * Independent parser for complete encoder-stream instructions.  It counts
 * the insertion frontier only when a complete insert/duplicate instruction
 * boundary has been consumed.
 */
typedef enum qm_es_state {
  QM_ES_OPCODE = 0,
  QM_ES_INDEX,
  QM_ES_NAME_LEN,
  QM_ES_NAME_DATA,
  QM_ES_VALUE_LEN,
  QM_ES_VALUE_DATA
} qm_es_state;

typedef struct qm_es_parser {
  int state;
  int opcode;
  qm_varint_state vi;
  uint64_t skip;
} qm_es_parser;

void qm_es_parser_init(qm_es_parser *p);
int qm_es_parser_feed(qm_es_parser *p, const uint8_t *data, size_t len,
                      uint64_t *insertions);

/* Independent decoder-feedback instruction parser. */
typedef enum qm_fb_state {
  QM_FB_OPCODE = 0,
  QM_FB_NUMBER
} qm_fb_state;

typedef enum qm_fb_opcode {
  QM_FB_ICNT = 0,
  QM_FB_CANCEL = 1,
  QM_FB_ACK = 2
} qm_fb_opcode;

typedef struct qm_fb_parser {
  int state;
  int opcode;
  unsigned prefix_bits;
  qm_varint_state vi;
} qm_fb_parser;

void qm_fb_parser_init(qm_fb_parser *p);

typedef struct qm_diag {
  int category; /* 1 = script, 2 = model, 3 = resource/runtime */
  unsigned long step;
  unsigned long line;
  int has_stream;
  int64_t stream;
  char op[24];
  char kind[64];
  char expected[160];
  char actual[160];
  char detail[320];
} qm_diag;

typedef struct qm_ref {
  uint64_t section_id;
  uint64_t ric;
} qm_ref;

typedef struct qm_section {
  uint64_t id;
  int64_t stream_id;
  uint8_t *name;
  size_t namelen;
  uint8_t *value;
  size_t valuelen;

  size_t section_len;
  size_t prefix_len;
  size_t remain;

  uint64_t encoded_ric;
  uint64_t ric;
  uint64_t base;
  int base_sign;

  int emit_count;
  uint8_t *got_name;
  size_t got_namelen;
  uint8_t *got_value;
  size_t got_valuelen;
  int field_matches;
} qm_section;

typedef struct qm_stream {
  int64_t id;
  nghttp3_qpack_stream_context *sctx;

  qm_bytes request_q;

  qm_section **sections;
  size_t nsections;
  size_t section_cap;

  qm_ref *refs;
  size_t nrefs;
  size_t ref_cap;
  size_t ref_head;

  int canceled;
  int decoder_blocked;

  unsigned submitted;
  unsigned completed;
} qm_stream;

typedef struct qm_model {
  const nghttp3_mem *mem;
  nghttp3_qpack_encoder *enc;
  nghttp3_qpack_decoder *dec;

  size_t capacity;
  size_t blocked_limit;
  uint64_t map_seed;

  qm_stream **streams;
  size_t nstreams;
  size_t stream_cap;

  qm_bytes encoder_q;
  qm_bytes feedback_q;

  qm_es_parser es_parser;
  qm_fb_parser fb_parser;

  /* Independent frontiers. */
  uint64_t produced_icnt;
  uint64_t decoder_icnt;
  uint64_t known_icnt;

  unsigned completed_sections;
  unsigned canceled_streams;
  uint64_t next_section_id;

  unsigned long step;
  unsigned long line;

  unsigned long blocked_records;
  unsigned long zero_records;

  FILE *out;
  qm_diag diag;
} qm_model;

#define QM_CATEGORY_SCRIPT 1
#define QM_CATEGORY_MODEL 2
#define QM_CATEGORY_RUNTIME 3

/* model.c */
int qm_model_init(qm_model *m, size_t capacity, size_t blocked_limit,
                  FILE *out);
void qm_model_destroy(qm_model *m);
qm_stream *qm_model_find_stream(qm_model *m, int64_t id);
qm_stream *qm_model_get_stream(qm_model *m, int64_t id);

void qm_diag_set(qm_model *m, int category, const char *op,
                 const char *kind, int has_stream, int64_t stream,
                 const char *expected, const char *actual,
                 const char *detail);
int qm_fail(qm_model *m, int category, const char *op, const char *kind,
            int has_stream, int64_t stream, const char *expected,
            const char *actual, const char *detail);
int qm_check_state(qm_model *m, const char *where);
int qm_reconstruct_ric(const qm_model *m, uint64_t encoded_ric,
                       uint64_t decoder_icnt, uint64_t *ric);

size_t qm_stream_ref_count(const qm_stream *st);
int qm_stream_append_ref(qm_stream *st, uint64_t section_id, uint64_t ric);
int qm_stream_pop_oldest_ref(qm_stream *st, qm_ref *out);
void qm_stream_clear_refs(qm_stream *st);
void qm_section_free(qm_section *sec);

/* transport.c */
int qm_cmd_section(qm_model *m, int64_t stream_id,
                   const uint8_t *name, size_t namelen,
                   const uint8_t *value, size_t valuelen);
int qm_cmd_encoder(qm_model *m, uint64_t max_bytes);
int qm_cmd_request(qm_model *m, int64_t stream_id, uint64_t max_bytes);
int qm_cmd_feedback(qm_model *m, uint64_t max_bytes);
int qm_cmd_cancel(qm_model *m, int64_t stream_id);
int qm_cmd_end(qm_model *m);

/* replay.c */
int qpack_model_replay(FILE *input, FILE *output);
int qpack_model_generate(uint64_t seed, unsigned int steps, FILE *output);
int qpack_model_shrink_text(const char *script, FILE *report);

#ifdef __cplusplus
}
#endif /* defined(__cplusplus) */

#endif /* FUZZ_STATEFUL_MODEL_H */
