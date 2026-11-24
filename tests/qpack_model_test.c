#include "task_support/check.h"
#include "fuzz/stateful/model.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B(s) ((const uint8_t *)(s))

struct replay_result {
  int rc;
  char out[16384];
};

static int replay_text(const char *script, struct replay_result *r) {
  FILE *in = tmpfile();
  FILE *out = tmpfile();
  size_t n;

  if (!in || !out) {
    if (in) fclose(in);
    if (out) fclose(out);
    return 1;
  }
  fputs(script, in);
  rewind(in);
  r->out[0] = '\0';
  r->rc = qpack_model_replay(in, out);
  rewind(out);
  n = fread(r->out, 1, sizeof(r->out) - 1, out);
  r->out[n] = '\0';
  fclose(in);
  fclose(out);
  return 0;
}

static int replay_generated(unsigned seed, unsigned steps,
                            struct replay_result *r, char *script,
                            size_t scriptsz) {
  FILE *sf = tmpfile();
  FILE *out = tmpfile();
  size_t n;

  if (!sf || !out) return 1;
  qpack_model_generate(seed, steps, sf);
  rewind(sf);
  script[0] = '\0';
  n = fread(script, 1, scriptsz - 1, sf);
  script[n] = '\0';
  rewind(sf);

  r->out[0] = '\0';
  r->rc = qpack_model_replay(sf, out);
  fclose(sf);
  rewind(out);
  n = fread(r->out, 1, sizeof(r->out) - 1, out);
  r->out[n] = '\0';
  fclose(out);
  return 0;
}

static int test_sections_same_stream(void) {
  qm_model m;
  FILE *log = tmpfile();
  qm_stream *st;

  TASK_CHECK(log != NULL);
  TASK_CHECK(qm_model_init(&m, 4096, 4, log) == 0);

  TASK_CHECK(qm_cmd_section(&m, 0, B("link"), 4, B("foo"), 3) == 0);
  TASK_CHECK(qm_cmd_section(&m, 0, B("link"), 4, B("bar"), 3) == 0);
  st = qm_model_find_stream(&m, 0);
  TASK_CHECK(st != NULL);
  TASK_CHECK(st->nsections == 2);
  TASK_CHECK(qm_stream_ref_count(st) == 2);

  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(qm_cmd_request(&m, 0, 4096) == 0);
  TASK_CHECK(m.completed_sections == 2);
  TASK_CHECK(st->nsections == 0);
  TASK_CHECK(st->completed == 2);

  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_sections_shared_dynamic(void) {
  qm_model m;
  FILE *log = tmpfile();
  qm_stream *a, *b, *c;

  TASK_CHECK(log != NULL);
  TASK_CHECK(qm_model_init(&m, 4096, 4, log) == 0);

  TASK_CHECK(qm_cmd_section(&m, 0, B("link"), 4, B("foo"), 3) == 0);
  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(qm_cmd_request(&m, 0, 4096) == 0);
  TASK_CHECK(qm_cmd_feedback(&m, 4096) == 0);

  TASK_CHECK(qm_cmd_section(&m, 4, B("link"), 4, B("foo"), 3) == 0);
  TASK_CHECK(qm_cmd_section(&m, 8, B("link"), 4, B("foo"), 3) == 0);
  a = qm_model_find_stream(&m, 4);
  b = qm_model_find_stream(&m, 8);
  TASK_CHECK(a && b);
  TASK_CHECK(qm_stream_ref_count(a) == 1);
  TASK_CHECK(qm_stream_ref_count(b) == 1);
  TASK_CHECK(a->refs[a->ref_head].ric == b->refs[b->ref_head].ric);

  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(qm_cmd_request(&m, 4, 4096) == 0);
  TASK_CHECK(qm_cmd_request(&m, 8, 4096) == 0);
  TASK_CHECK(m.completed_sections == 3);

  c = qm_model_find_stream(&m, 0);
  TASK_CHECK(c != NULL);
  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_sections_request_before_encoder(void) {
  qm_model m;
  FILE *log = tmpfile();
  qm_stream *st;

  TASK_CHECK(log != NULL);
  TASK_CHECK(qm_model_init(&m, 4096, 4, log) == 0);
  TASK_CHECK(qm_cmd_section(&m, 0, B("link"), 4, B("foo"), 3) == 0);

  /* The two-byte prefix is enough to reconstruct RIC 1 and block. */
  TASK_CHECK(qm_cmd_request(&m, 0, 2) == 0);
  st = qm_model_find_stream(&m, 0);
  TASK_CHECK(st != NULL);
  TASK_CHECK(st->decoder_blocked);
  TASK_CHECK(m.decoder_icnt == 0);
  TASK_CHECK(m.completed_sections == 0);

  TASK_CHECK(qm_cmd_encoder(&m, 1) == 0);
  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(m.decoder_icnt == 1);
  TASK_CHECK(qm_cmd_request(&m, 0, 1) == 0);
  TASK_CHECK(qm_cmd_request(&m, 0, 4096) == 0);
  TASK_CHECK(m.completed_sections == 1);

  TASK_CHECK(qm_cmd_feedback(&m, 1) == 0);
  TASK_CHECK(qm_cmd_feedback(&m, 4096) == 0);
  TASK_CHECK(m.known_icnt == 1);

  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_sections_zero_ric_has_no_ref(void) {
  qm_model m;
  FILE *log = tmpfile();
  qm_stream *st;

  TASK_CHECK(log != NULL);
  /* authorization is never indexed, so this normal section has RIC 0. */
  TASK_CHECK(qm_model_init(&m, 4096, 4, log) == 0);
  TASK_CHECK(qm_cmd_section(&m, 0, B("authorization"), 13, B("x"), 1) == 0);
  st = qm_model_find_stream(&m, 0);
  TASK_CHECK(st != NULL);
  TASK_CHECK(qm_stream_ref_count(st) == 0);
  TASK_CHECK(nghttp3_qpack_encoder_find_stream(m.enc, 0) == NULL);
  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(qm_cmd_request(&m, 0, 4096) == 0);
  TASK_CHECK(m.completed_sections == 1);

  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_feedback_ack_oldest(void) {
  qm_model m;
  FILE *log = tmpfile();
  qm_stream *st;
  uint64_t oldest_ric;

  TASK_CHECK(log != NULL);
  TASK_CHECK(qm_model_init(&m, 4096, 4, log) == 0);
  TASK_CHECK(qm_cmd_section(&m, 0, B("link"), 4, B("foo"), 3) == 0);
  TASK_CHECK(qm_cmd_section(&m, 0, B("link"), 4, B("bar"), 3) == 0);
  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(qm_cmd_request(&m, 0, 4096) == 0);

  st = qm_model_find_stream(&m, 0);
  TASK_CHECK(st != NULL);
  TASK_CHECK(qm_stream_ref_count(st) == 2);
  oldest_ric = st->refs[st->ref_head].ric;

  TASK_CHECK(qm_cmd_feedback(&m, 1) == 0);
  TASK_CHECK(qm_stream_ref_count(st) == 1);
  TASK_CHECK(m.known_icnt == oldest_ric);
  TASK_CHECK(nghttp3_qpack_encoder_find_stream(m.enc, 0) != NULL);

  TASK_CHECK(qm_cmd_feedback(&m, 1) == 0);
  TASK_CHECK(qm_stream_ref_count(st) == 0);
  TASK_CHECK(nghttp3_qpack_encoder_find_stream(m.enc, 0) == NULL);

  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_feedback_insert_count_increment(void) {
  qm_model m;
  FILE *log = tmpfile();

  TASK_CHECK(log != NULL);
  TASK_CHECK(qm_model_init(&m, 4096, 4, log) == 0);
  TASK_CHECK(qm_cmd_section(&m, 0, B("a"), 1, B("1"), 1) == 0);
  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(qm_cmd_request(&m, 0, 4096) == 0);
  TASK_CHECK(qm_cmd_feedback(&m, 4096) == 0);
  TASK_CHECK(m.known_icnt == 1);

  /* An undecoded new insertion gives decoder stream an ICI byte. */
  TASK_CHECK(qm_cmd_section(&m, 4, B("b"), 1, B("2"), 1) == 0);
  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(m.decoder_icnt == 2);
  TASK_CHECK(nghttp3_qpack_decoder_get_decoder_streamlen2(m.dec) >= 1);
  TASK_CHECK(qm_cmd_feedback(&m, 1) == 0);
  TASK_CHECK(m.known_icnt == 2);
  TASK_CHECK(m.feedback_q.len == 0);

  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_feedback_multibyte_insert_count(void) {
  qm_model m;
  FILE *log = tmpfile();
  unsigned i;

  TASK_CHECK(log != NULL);
  TASK_CHECK(qm_model_init(&m, 4096, 8, log) == 0);

  /* Eighty distinct names create eighty unread insert instructions.  No
     request reaches FINAL, so decoder feedback is a two-byte ICI only. */
  for (i = 0; i < 80; ++i) {
    char name[8];
    snprintf(name, sizeof(name), "n%03u", i);
    TASK_CHECK(qm_cmd_section(&m, 0, B(name), strlen(name), B("v"), 1) == 0);
  }
  TASK_CHECK(m.produced_icnt == 80);
  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(m.decoder_icnt == 80);
  TASK_CHECK(nghttp3_qpack_decoder_get_decoder_streamlen2(m.dec) >= 2);

  TASK_CHECK(qm_cmd_feedback(&m, 1) == 0);
  TASK_CHECK(m.known_icnt == 0);
  TASK_CHECK(m.fb_parser.state == QM_FB_NUMBER);
  TASK_CHECK(m.feedback_q.len == 1);

  TASK_CHECK(qm_cmd_feedback(&m, 1) == 0);
  TASK_CHECK(m.known_icnt == 80);
  TASK_CHECK(m.fb_parser.state == QM_FB_OPCODE);
  TASK_CHECK(m.feedback_q.len == 0);

  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_feedback_persistent_half(void) {
  qm_model m;
  FILE *log = tmpfile();
  qm_stream *st;

  TASK_CHECK(log != NULL);
  TASK_CHECK(qm_model_init(&m, 4096, 4, log) == 0);
  TASK_CHECK(qm_cmd_section(&m, 128, B("x-k"), 3, B("v"), 1) == 0);
  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(qm_cmd_request(&m, 128, 4096) == 0);

  TASK_CHECK(qm_cmd_feedback(&m, 1) == 0);
  st = qm_model_find_stream(&m, 128);
  TASK_CHECK(st != NULL);
  TASK_CHECK(qm_stream_ref_count(st) == 1);
  TASK_CHECK(m.feedback_q.len >= 1);
  TASK_CHECK(m.fb_parser.state == QM_FB_NUMBER);

  /* N=0 must not overwrite or discard the persistent tail. */
  TASK_CHECK(qm_cmd_feedback(&m, 0) == 0);
  TASK_CHECK(m.feedback_q.len >= 1);
  TASK_CHECK(qm_stream_ref_count(st) == 1);

  TASK_CHECK(qm_cmd_feedback(&m, 4096) == 0);
  TASK_CHECK(qm_stream_ref_count(st) == 0);

  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_cancel_blocked(void) {
  qm_model m;
  FILE *log = tmpfile();
  qm_stream *st;
  uint64_t known_after_cancel_feedback;

  TASK_CHECK(log != NULL);
  TASK_CHECK(qm_model_init(&m, 4096, 4, log) == 0);
  TASK_CHECK(qm_cmd_section(&m, 0, B("link"), 4, B("foo"), 3) == 0);
  TASK_CHECK(qm_cmd_request(&m, 0, 2) == 0);
  st = qm_model_find_stream(&m, 0);
  TASK_CHECK(st != NULL);
  TASK_CHECK(st->decoder_blocked);

  TASK_CHECK(qm_cmd_cancel(&m, 0) == 0);
  TASK_CHECK(st->canceled);
  TASK_CHECK(st->request_q.len == 0);
  TASK_CHECK(st->nsections == 0);
  /* Encoder retains refs until the real CANCEL feedback is consumed. */
  TASK_CHECK(qm_stream_ref_count(st) == 1);
  TASK_CHECK(nghttp3_qpack_encoder_find_stream(m.enc, 0) != NULL);

  TASK_CHECK(qm_cmd_feedback(&m, 4096) == 0);
  TASK_CHECK(qm_stream_ref_count(st) == 0);
  TASK_CHECK(nghttp3_qpack_encoder_find_stream(m.enc, 0) == NULL);
  known_after_cancel_feedback = m.known_icnt;
  TASK_CHECK(known_after_cancel_feedback == 0);

  /* Another stream still completes. */
  TASK_CHECK(qm_cmd_section(&m, 4, B("cat"), 3, B("z"), 1) == 0);
  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(qm_cmd_request(&m, 4, 4096) == 0);
  TASK_CHECK(qm_cmd_feedback(&m, 4096) == 0);
  TASK_CHECK(m.completed_sections == 1);
  TASK_CHECK(m.canceled_streams == 1);

  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_cancel_partial_ack_ordering(void) {
  qm_model m;
  FILE *log = tmpfile();
  qm_stream *st;
  uint64_t known_after_ack;

  TASK_CHECK(log != NULL);
  TASK_CHECK(qm_model_init(&m, 4096, 4, log) == 0);
  TASK_CHECK(qm_cmd_section(&m, 128, B("x-k"), 3, B("v"), 1) == 0);
  TASK_CHECK(qm_cmd_encoder(&m, 4096) == 0);
  TASK_CHECK(qm_cmd_request(&m, 128, 4096) == 0);
  TASK_CHECK(qm_cmd_feedback(&m, 1) == 0);

  st = qm_model_find_stream(&m, 128);
  TASK_CHECK(st != NULL);
  TASK_CHECK(m.fb_parser.state == QM_FB_NUMBER);
  TASK_CHECK(qm_stream_ref_count(st) == 1);

  /* A second unfinished dynamic section makes CANCEL legal. */
  TASK_CHECK(qm_cmd_section(&m, 128, B("qq"), 2, B("w"), 1) == 0);
  TASK_CHECK(qm_stream_ref_count(st) == 2);
  TASK_CHECK(qm_cmd_cancel(&m, 128) == 0);

  /* First post-cancel byte completes the old ACK, not CANCEL. */
  TASK_CHECK(qm_cmd_feedback(&m, 1) == 0);
  TASK_CHECK(qm_stream_ref_count(st) == 1);
  known_after_ack = m.known_icnt;
  TASK_CHECK(known_after_ack > 0);

  /* Stream 128 uses a two-byte CANCEL varint; observe its split boundary. */
  TASK_CHECK(qm_cmd_feedback(&m, 1) == 0);
  TASK_CHECK(qm_stream_ref_count(st) == 1);
  TASK_CHECK(m.fb_parser.state == QM_FB_NUMBER);
  TASK_CHECK(qm_cmd_feedback(&m, 1) == 0);
  TASK_CHECK(qm_stream_ref_count(st) == 0);
  TASK_CHECK(m.known_icnt == known_after_ack);
  TASK_CHECK(nghttp3_qpack_encoder_find_stream(m.enc, 128) == NULL);

  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_cancel_rejects_new_section(void) {
  qm_model m;
  FILE *log = tmpfile();

  TASK_CHECK(log != NULL);
  TASK_CHECK(qm_model_init(&m, 256, 4, log) == 0);
  TASK_CHECK(qm_cmd_section(&m, 0, B("a"), 1, B("1"), 1) == 0);
  TASK_CHECK(qm_cmd_cancel(&m, 0) == 0);
  TASK_CHECK(qm_cmd_section(&m, 0, B("b"), 1, B("2"), 1) != 0);
  TASK_CHECK(qm_cmd_cancel(&m, 0) != 0);

  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_replay_api_probe_output(void) {
  struct replay_result r;
  static const char *script =
    "INIT 256 4\n"
    "SECTION 0 6c696e6b 666f6f\n"
    "ENCODER 4096\n"
    "REQUEST 0 4096\n"
    "FEEDBACK 4096\n"
    "END\n";

  TASK_CHECK(replay_text(script, &r) == 0);
  TASK_CHECK(r.rc == 0);
  TASK_CHECK(strcmp(r.out,
                    "COMPLETE 1 CANCELED 0\nCONSISTENT\n") == 0);
  return 0;
}

static int test_replay_example_file(void) {
  FILE *f = fopen("examples/qpack_schedule.txt", "r");
  FILE *out = tmpfile();
  char buf[32768];
  size_t n;
  int rc;

  if (!f) f = fopen("../examples/qpack_schedule.txt", "r");
  TASK_CHECK(f != NULL);
  TASK_CHECK(out != NULL);
  rc = qpack_model_replay(f, out);
  TASK_CHECK(rc == 0);
  rewind(out);
  n = fread(buf, 1, sizeof(buf) - 1, out);
  buf[n] = '\0';
  TASK_CHECK(strstr(buf, "COMPLETE ") != NULL);
  TASK_CHECK(strstr(buf, "CANCELED 1") != NULL);
  TASK_CHECK(strstr(buf, "CONSISTENT\n") != NULL);
  fclose(f);
  fclose(out);
  return 0;
}

static int test_replay_invalid_scripts(void) {
  struct replay_result r;

  TASK_CHECK(replay_text("INIT 16 4\nEND\n", &r) == 0);
  TASK_CHECK(r.rc != 0);
  TASK_CHECK(strstr(r.out, "category=script") != NULL);

  TASK_CHECK(replay_text("INIT 256 9\nEND\n", &r) == 0);
  TASK_CHECK(r.rc != 0);

  TASK_CHECK(replay_text(
               "INIT 256 4\nENCODER -1\nEND\n", &r) == 0);
  TASK_CHECK(r.rc != 0);

  /* 41 is uppercase 'A'. */
  TASK_CHECK(replay_text(
               "INIT 256 4\nSECTION 0 41 78\nEND\n", &r) == 0);
  TASK_CHECK(r.rc != 0);
  TASK_CHECK(strstr(r.out, "bad_name") != NULL);

  TASK_CHECK(replay_text(
               "INIT 256 4\nSECTION 1 61 78\nEND\n", &r) == 0);
  TASK_CHECK(r.rc != 0);

  TASK_CHECK(replay_text(
               "INIT 256 4\n"
               "SECTION 0 61 31\n"
               "ENCODER 4096\n"
               "REQUEST 0 4096\n"
               "FEEDBACK 4096\n"
               "CANCEL 0\n"
               "END\n", &r) == 0);
  TASK_CHECK(r.rc != 0);
  TASK_CHECK(strstr(r.out, "no_unfinished_section") != NULL);

  TASK_CHECK(replay_text("INIT 256 4\n", &r) == 0);
  TASK_CHECK(r.rc != 0);
  TASK_CHECK(strstr(r.out, "missing_end") != NULL);

  return 0;
}

static int test_replay_zero_is_not_complete(void) {
  qm_model m;
  FILE *log = tmpfile();

  TASK_CHECK(log != NULL);
  TASK_CHECK(qm_model_init(&m, 256, 4, log) == 0);
  TASK_CHECK(qm_cmd_section(&m, 0, B("a"), 1, B("1"), 1) == 0);
  TASK_CHECK(qm_cmd_request(&m, 0, 0) == 0);
  TASK_CHECK(m.completed_sections == 0);
  TASK_CHECK(m.zero_records >= 1);
  TASK_CHECK(qm_stream_ref_count(qm_model_find_stream(&m, 0)) == 1);

  qm_model_destroy(&m);
  fclose(log);
  return 0;
}

static int test_replay_generated_sequences(void) {
  unsigned seed;
  struct replay_result r;
  char script[65536];

  for (seed = 1; seed <= 120; ++seed) {
    TASK_CHECK(replay_generated(seed, 90, &r, script, sizeof(script)) == 0);
    if (r.rc != 0) {
      fputs("Generated failing script:\n", stderr);
      fputs(script, stderr);
      fputs(r.out, stderr);
      qpack_model_shrink_text(script, stderr);
    }
    TASK_CHECK(r.rc == 0);
    TASK_CHECK(strstr(r.out, "CONSISTENT\n") != NULL);
    TASK_CHECK(strstr(r.out, "COMPLETE ") != NULL);
  }
  return 0;
}

static int test_replay_generate_deterministic(void) {
  FILE *a = tmpfile();
  FILE *b = tmpfile();
  char ba[8192], bb[8192];
  size_t na, nb;

  TASK_CHECK(a && b);
  TASK_CHECK(qpack_model_generate(987654321ULL, 75, a) == 0);
  TASK_CHECK(qpack_model_generate(987654321ULL, 75, b) == 0);
  rewind(a);
  rewind(b);
  na = fread(ba, 1, sizeof(ba) - 1, a);
  nb = fread(bb, 1, sizeof(bb) - 1, b);
  ba[na] = '\0';
  bb[nb] = '\0';
  TASK_CHECK(na == nb);
  TASK_CHECK(strcmp(ba, bb) == 0);
  TASK_CHECK(strstr(ba, "INIT ") == ba);
  TASK_CHECK(strstr(ba, "\nEND\n") != NULL);
  fclose(a);
  fclose(b);
  return 0;
}

const struct task_case task_cases[] = {
  {"sections", "same_stream_consecutive", test_sections_same_stream},
  {"sections", "shared_dynamic_item", test_sections_shared_dynamic},
  {"sections", "request_before_encoder_blocks",
   test_sections_request_before_encoder},
  {"sections", "zero_ric_has_no_reference",
   test_sections_zero_ric_has_no_ref},

  {"feedback", "ack_removes_oldest_only", test_feedback_ack_oldest},
  {"feedback", "insert_count_increment",
   test_feedback_insert_count_increment},
  {"feedback", "multibyte_insert_count_one_byte_at_a_time",
   test_feedback_multibyte_insert_count},
  {"feedback", "persistent_half_instruction",
   test_feedback_persistent_half},

  {"cancel", "blocked_stream_and_other_survives", test_cancel_blocked},
  {"cancel", "partial_ack_then_cancel_ordering",
   test_cancel_partial_ack_ordering},
  {"cancel", "canceled_stream_rejects_new_work",
   test_cancel_rejects_new_section},

  {"replay", "api_probe_exact_output", test_replay_api_probe_output},
  {"replay", "example_schedule_file", test_replay_example_file},
  {"replay", "invalid_scripts_report_step", test_replay_invalid_scripts},
  {"replay", "zero_progress_is_not_completion",
   test_replay_zero_is_not_complete},
  {"replay", "generated_legal_sequences",
   test_replay_generated_sequences},
  {"replay", "generate_is_deterministic",
   test_replay_generate_deterministic},
};

const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
