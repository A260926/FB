/*
                            __  __            _
                         ___\ \/ /_ __   __ _| |_
                        / _ \\  /| '_ \ / _` | __|
                       /  __//  \\ | | | (_| | |_
                        \___/_/\_\\_| |_|\__,_|\__|

                               |_| XML parser

   Reparse deferral contract tests.  These exercise the public parser
   entry points (XML_Parse, XML_ParseBuffer, XML_GetBuffer,
   XML_StopParser/XML_ResumeParser, XML_ParserReset and
   XML_ExternalEntityParserCreate) only.  The scan-count observations
   rely on the CHECK_SCAN_PROBE build instrumentation, which counts
   iterations of the original attribute value scan loop in
   xmltok_impl.c without changing any parsing decision; when that
   instrumentation is absent, the functional checks still run.

   Licensed under the MIT license, see the file COPYING for details.
*/

#include <expat.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CHECK_SCAN_PROBE
extern unsigned long long unit_scan_work;
#endif

#define UNUSED(p) (void)(p)

/* --------------------------------------------------------------- */
/* Small event recorder                                            */
/* --------------------------------------------------------------- */

enum { EV_START = 0, EV_END, EV_CHAR, EV_COMMENT, EV_DOCTYPE_START,
       EV_DOCTYPE_END };

#define MAX_EVENTS 256

struct event_log {
  int kind[MAX_EVENTS];
  char name[MAX_EVENTS][32];
  int count;
};

static void
log_event(struct event_log *log, int kind, const XML_Char *name) {
  assert(log->count < MAX_EVENTS);
  log->kind[log->count] = kind;
  if (name) {
    size_t n = strlen(name);
    assert(n < sizeof(log->name[0]));
    memcpy(log->name[log->count], name, n + 1);
  } else {
    log->name[log->count][0] = '\0';
  }
  log->count++;
}

static void XMLCALL
on_start(void *ud, const XML_Char *name, const XML_Char **atts) {
  UNUSED(atts);
  log_event((struct event_log *)ud, EV_START, name);
}

static void XMLCALL
on_end(void *ud, const XML_Char *name) {
  log_event((struct event_log *)ud, EV_END, name);
}

static void XMLCALL
on_char(void *ud, const XML_Char *s, int len) {
  UNUSED(s);
  UNUSED(len);
  log_event((struct event_log *)ud, EV_CHAR, NULL);
}

static void XMLCALL
on_comment(void *ud, const XML_Char *data) {
  UNUSED(data);
  log_event((struct event_log *)ud, EV_COMMENT, NULL);
}

static void XMLCALL
on_doctype_start(void *ud, const XML_Char *name, const XML_Char *sysid,
                 const XML_Char *pubid, int has_internal) {
  UNUSED(sysid);
  UNUSED(pubid);
  UNUSED(has_internal);
  log_event((struct event_log *)ud, EV_DOCTYPE_START, name);
}

static void XMLCALL
on_doctype_end(void *ud) {
  log_event((struct event_log *)ud, EV_DOCTYPE_END, NULL);
}

static void
attach_handlers(XML_Parser p, struct event_log *log) {
  XML_SetUserData(p, log);
  XML_SetElementHandler(p, on_start, on_end);
  XML_SetCharacterDataHandler(p, on_char);
  XML_SetCommentHandler(p, on_comment);
  XML_SetDoctypeDeclHandler(p, on_doctype_start, on_doctype_end);
}

/* --------------------------------------------------------------- */
/* Chunked feeding through both public APIs                        */
/* --------------------------------------------------------------- */

/* Drives the whole feed in fixed-size chunks; the final chunk carries
   isFinal=1.  Returns the final XML_Status. */
static enum XML_Status
feed_all(XML_Parser p, const char *data, size_t len, int chunk,
         int buffered) {
  size_t pos = 0;
  enum XML_Status status = XML_STATUS_OK;
  while (pos < len) {
    int take = (int)((len - pos < (size_t)chunk) ? len - pos
                                                 : (size_t)chunk);
    int is_final = (pos + (size_t)take == len);
    if (buffered) {
      void *buf = XML_GetBuffer(p, take);
      if (buf == NULL)
        return XML_STATUS_ERROR;
      memcpy(buf, data + pos, (size_t)take);
      status = XML_ParseBuffer(p, take, is_final);
    } else {
      status = XML_Parse(p, data + pos, take, is_final);
    }
    if (status != XML_STATUS_OK)
      return status;
    pos += (size_t)take;
  }
  return status;
}

/* Like feed_all but every chunk is non-final; the caller ends the
   document separately (e.g. with a zero-length final feed). */
static enum XML_Status
feed_nonfinal(XML_Parser p, const char *data, size_t len, int chunk,
              int buffered) {
  size_t pos = 0;
  enum XML_Status status = XML_STATUS_OK;
  while (pos < len) {
    int take = (int)((len - pos < (size_t)chunk) ? len - pos
                                                 : (size_t)chunk);
    if (buffered) {
      void *buf = XML_GetBuffer(p, take);
      if (buf == NULL)
        return XML_STATUS_ERROR;
      memcpy(buf, data + pos, (size_t)take);
      status = XML_ParseBuffer(p, take, 0);
    } else {
      status = XML_Parse(p, data + pos, take, 0);
    }
    if (status != XML_STATUS_OK)
      return status;
    pos += (size_t)take;
  }
  return status;
}

static char *
make_attr_doc(size_t value_len, size_t *total_len) {
  static const char prefix[] = "<r a='";
  static const char suffix[] = "'/>";
  size_t len = sizeof(prefix) - 1 + value_len + sizeof(suffix) - 1;
  char *doc = (char *)malloc(len + 1);
  assert(doc);
  memcpy(doc, prefix, sizeof(prefix) - 1);
  memset(doc + sizeof(prefix) - 1, 'x', value_len);
  memcpy(doc + sizeof(prefix) - 1 + value_len, suffix, sizeof(suffix));
  if (total_len)
    *total_len = len;
  return doc;
}

/* --------------------------------------------------------------- */
/* 1. Scan-count contract for the attribute scan loop              */
/* --------------------------------------------------------------- */

static unsigned long long
count_attr_scan(size_t n, int chunk, int buffered, int enabled) {
  size_t len;
  char *doc = make_attr_doc(n, &len);
  XML_Parser p = XML_ParserCreate(NULL);
  unsigned long long work;
  assert(p);
  XML_SetReparseDeferralEnabled(p, enabled ? XML_TRUE : XML_FALSE);
#ifdef CHECK_SCAN_PROBE
  unit_scan_work = 0;
#endif
  assert(feed_all(p, doc, len, chunk, buffered) == XML_STATUS_OK);
#ifdef CHECK_SCAN_PROBE
  work = unit_scan_work;
#else
  work = 0;
#endif
  XML_ParserFree(p);
  free(doc);
  return work;
}

static void
test_api_basics(void) {
  XML_Parser p = XML_ParserCreate(NULL);
  assert(p);
  assert(XML_SetReparseDeferralEnabled(NULL, XML_TRUE) == XML_FALSE);
  assert(XML_SetReparseDeferralEnabled(NULL, XML_FALSE) == XML_FALSE);
  assert(XML_SetReparseDeferralEnabled(p, XML_FALSE) == XML_TRUE);
  assert(XML_SetReparseDeferralEnabled(p, XML_TRUE) == XML_TRUE);
  XML_ParserFree(p);
  printf("PASS api_basics\n");
}

static void
test_scan_count_bounds(void) {
#ifdef CHECK_SCAN_PROBE
  static const size_t sizes[] = {32768, 65536, 131072};
  static const int chunks[] = {1, 17, 64};
  size_t i;
  int j, mode;
  /* Enabled: the full matrix must stay within the linear bound. */
  for (mode = 0; mode < 2; mode++) {
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
      for (j = 0; j < 3; j++) {
        size_t n = sizes[i];
        unsigned long long bound = 8ULL * n + 4096ULL;
        unsigned long long on = count_attr_scan(n, chunks[j], mode, 1);
        assert(on <= bound);
      }
    }
  }
  /* Disabled: the visible repeated rescans stay quadratic.  One
     representative per size keeps the witness cheap while covering
     both public feeding APIs. */
  for (mode = 0; mode < 2; mode++) {
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
      size_t n = sizes[i];
      unsigned long long bound = 8ULL * n + 4096ULL;
      unsigned long long on = count_attr_scan(n, 64, mode, 1);
      unsigned long long off = count_attr_scan(n, 64, mode, 0);
      assert(off > bound);
      assert(off > on * 4ULL);
    }
  }
  /* And the smallest size keeps all three chunk sizes as witnesses. */
  for (j = 0; j < 3; j++) {
    size_t n = sizes[0];
    unsigned long long bound = 8ULL * n + 4096ULL;
    unsigned long long on = count_attr_scan(n, chunks[j], 0, 1);
    unsigned long long off = count_attr_scan(n, chunks[j], 0, 0);
    assert(on <= bound);
    assert(off > bound);
  }
  printf("PASS scan_count_bounds (parse and parsebuffer, %zu sizes)\n",
         sizeof(sizes) / sizeof(sizes[0]));
#else
  printf("SKIP scan_count_bounds (CHECK_SCAN_PROBE not enabled)\n");
#endif
}

/* A parser that never toggles the switch must behave linearly, i.e.
   deferral defaults to enabled. */
static void
test_default_enabled(void) {
#ifdef CHECK_SCAN_PROBE
  static const size_t n = 65536;
  size_t len;
  char *doc = make_attr_doc(n, &len);
  XML_Parser p = XML_ParserCreate(NULL);
  unsigned long long work;
  assert(p);
  unit_scan_work = 0;
  assert(feed_all(p, doc, len, 64, 0) == XML_STATUS_OK);
  work = unit_scan_work;
  assert(work <= 8ULL * n + 4096ULL);
  XML_ParserFree(p);
  free(doc);
  printf("PASS default_enabled (work=%llu)\n", work);
#else
  printf("SKIP default_enabled (CHECK_SCAN_PROBE not enabled)\n");
#endif
}

/* --------------------------------------------------------------- */
/* 2. Event sequence: large token then small tokens, many chunks   */
/* --------------------------------------------------------------- */

static void
test_event_sequence(void) {
  static const int chunks[] = {1, 2, 7, 17, 64, 100, 4096};
  static const char doc[] =
      "<r><big a='xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'>y</big><s>z</s></r>";
  size_t i;
  for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
    int mode, enabled;
    for (mode = 0; mode < 2; mode++) {
      for (enabled = 0; enabled < 2; enabled++) {
        struct event_log log;
        XML_Parser p = XML_ParserCreate(NULL);
        assert(p);
        memset(&log, 0, sizeof(log));
        attach_handlers(p, &log);
        XML_SetReparseDeferralEnabled(p, enabled ? XML_TRUE : XML_FALSE);
        assert(feed_all(p, doc, strlen(doc), chunks[i], mode)
               == XML_STATUS_OK);
        assert(log.count == 8);
        assert(log.kind[0] == EV_START && ! strcmp(log.name[0], "r"));
        assert(log.kind[1] == EV_START && ! strcmp(log.name[1], "big"));
        assert(log.kind[2] == EV_CHAR);
        assert(log.kind[3] == EV_END && ! strcmp(log.name[3], "big"));
        assert(log.kind[4] == EV_START && ! strcmp(log.name[4], "s"));
        assert(log.kind[5] == EV_CHAR);
        assert(log.kind[6] == EV_END && ! strcmp(log.name[6], "s"));
        assert(log.kind[7] == EV_END && ! strcmp(log.name[7], "r"));
        XML_ParserFree(p);
      }
    }
  }
  printf("PASS event_sequence (%zu chunk sizes, both APIs)\n",
         sizeof(chunks) / sizeof(chunks[0]));
}

/* --------------------------------------------------------------- */
/* 3. Final feeds around the growth threshold                      */
/* --------------------------------------------------------------- */

static void
test_final_boundaries(void) {
  static const size_t n = 1024;
  static const int last_chunks[] = {1, 2, 3, 5, 8, 13, 64, 128, 256};
  size_t len;
  char *doc = make_attr_doc(n, &len);
  size_t i;
  for (i = 0; i < sizeof(last_chunks) / sizeof(last_chunks[0]); i++) {
    int mode, enabled;
    for (mode = 0; mode < 2; mode++) {
      for (enabled = 0; enabled < 2; enabled++) {
        struct event_log log;
        XML_ParsingStatus status;
        XML_Parser p = XML_ParserCreate(NULL);
        size_t k = (size_t)last_chunks[i];
        size_t cut = (k < len) ? len - k : 0;
        assert(p);
        memset(&log, 0, sizeof(log));
        attach_handlers(p, &log);
        XML_SetReparseDeferralEnabled(p, enabled ? XML_TRUE : XML_FALSE);
        if (cut)
          assert(XML_Parse(p, doc, (int)cut, 0) == XML_STATUS_OK);
        assert(XML_Parse(p, doc + cut, (int)(len - cut), 1)
               == XML_STATUS_OK);
        XML_GetParsingStatus(p, &status);
        assert(status.parsing == XML_FINISHED);
        /* The deliverable prefix must be delivered exactly once. */
        assert(log.count == 2);
        assert(log.kind[0] == EV_START);
        assert(log.kind[1] == EV_END);
        XML_ParserFree(p);
      }
    }
  }
  free(doc);
  printf("PASS final_boundaries (%zu final chunk sizes)\n",
         sizeof(last_chunks) / sizeof(last_chunks[0]));
}

/* Zero-length final feeds, both public APIs, including the
   XML_Parse(parser, NULL, 0, 1) form. */
static void
test_zero_length_final(void) {
  static const size_t n = 512;
  size_t len;
  char *doc = make_attr_doc(n, &len);
  struct event_log log;
  XML_ParsingStatus status;
  XML_Parser p = XML_ParserCreate(NULL);
  assert(p);
  memset(&log, 0, sizeof(log));
  attach_handlers(p, &log);
  assert(feed_nonfinal(p, doc, len, 64, 0) == XML_STATUS_OK);
  assert(XML_Parse(p, NULL, 0, 1) == XML_STATUS_OK);
  XML_GetParsingStatus(p, &status);
  assert(status.parsing == XML_FINISHED);
  assert(log.count == 2);
  XML_ParserFree(p);

  /* ParseBuffer zero-length final without another GetBuffer call. */
  p = XML_ParserCreate(NULL);
  assert(p);
  memset(&log, 0, sizeof(log));
  attach_handlers(p, &log);
  assert(feed_nonfinal(p, doc, len, 64, 1) == XML_STATUS_OK);
  assert(XML_ParseBuffer(p, 0, 1) == XML_STATUS_OK);
  XML_GetParsingStatus(p, &status);
  assert(status.parsing == XML_FINISHED);
  assert(log.count == 2);
  XML_ParserFree(p);
  free(doc);
  printf("PASS zero_length_final\n");
}

/* --------------------------------------------------------------- */
/* 4. Buffer moves and UTF-16 boundaries                           */
/* --------------------------------------------------------------- */

static void
test_buffer_moves(void) {
#ifdef CHECK_SCAN_PROBE
  static const size_t n = 32768;
  size_t len;
  char *doc = make_attr_doc(n, &len);
  XML_Parser p = XML_ParserCreate(NULL);
  size_t pos = 0;
  unsigned long long work;
  int calls = 0;
  assert(p);
  unit_scan_work = 0;
  /* Regularly ask for far more room than needed so XML_GetBuffer has
     to grow, reallocate and move the unconsumed input repeatedly. */
  while (pos < len) {
    int take = (int)((len - pos < 64U) ? len - pos : 64U);
    int ask = (calls % 4 == 0) ? take + 8192 : take;
    void *buf = XML_GetBuffer(p, ask);
    assert(buf);
    memcpy(buf, doc + pos, (size_t)take);
    assert(XML_ParseBuffer(p, take, pos + (size_t)take == len)
           == XML_STATUS_OK);
    pos += (size_t)take;
    calls++;
  }
  work = unit_scan_work;
  assert(work <= 8ULL * n + 4096ULL);
  XML_ParserFree(p);
  free(doc);
  printf("PASS buffer_moves (%d growing GetBuffer calls, work=%llu)\n",
         calls, work);
#else
  printf("SKIP buffer_moves (CHECK_SCAN_PROBE not enabled)\n");
#endif
}

static void
test_utf16_boundaries(void) {
  static const int value_chars = 1024;
  static const int chunks[] = {64, 65, 128};
  XML_Parser p;
  size_t ascii_len = (size_t)(6 + value_chars + 3);
  size_t cap = 2 + ascii_len * 2U;
  char *ascii_doc = (char *)malloc(ascii_len + 1);
  char *doc16 = (char *)malloc(cap);
  size_t byte_len, i;
  int j;
  assert(ascii_doc && doc16);
  /* Build the ASCII source "<r a='xxx...'/>" then encode it as
     UTF-16LE with a BOM, avoiding wide-character literals (wchar_t is
     not 16 bits on every platform). */
  {
    size_t pos = 0;
    memcpy(ascii_doc, "<r a='", 6);
    pos += 6;
    memset(ascii_doc + pos, 'x', (size_t)value_chars);
    pos += (size_t)value_chars;
    memcpy(ascii_doc + pos, "'/>", 3);
    pos += 3;
    ascii_doc[pos] = '\0';
    assert(pos == ascii_len);
  }
  doc16[0] = (char)0xFF;
  doc16[1] = (char)0xFE;
  for (i = 0; i < ascii_len; i++) {
    doc16[2 + 2 * i] = ascii_doc[i];
    doc16[2 + 2 * i + 1] = '\0';
  }
  byte_len = cap;

  for (j = 0; j < 3; j++) {
    int mode, enabled;
    for (mode = 0; mode < 2; mode++) {
      for (enabled = 0; enabled < 2; enabled++) {
        struct event_log log;
        p = XML_ParserCreate(NULL);
        assert(p);
        memset(&log, 0, sizeof(log));
        attach_handlers(p, &log);
        XML_SetReparseDeferralEnabled(p, enabled ? XML_TRUE : XML_FALSE);
        assert(feed_all(p, doc16, byte_len, chunks[j], mode)
               == XML_STATUS_OK);
        assert(log.count == 2);
        XML_ParserFree(p);
      }
    }
  }
  free(ascii_doc);
  free(doc16);
  printf("PASS utf16_boundaries (odd and even chunk sizes, both APIs)\n");
}

/* --------------------------------------------------------------- */
/* 5. Long comments and DTD declarations                           */
/* --------------------------------------------------------------- */

static void
test_long_comment_and_dtd(void) {
  static const int chunks[] = {17, 64, 257};
  static const size_t comment_len = 8192;
  static const size_t dtd_len = 4096;
  char *comment = (char *)malloc(comment_len + 32);
  char *dtd = (char *)malloc(dtd_len + 256);
  char *body;
  size_t clen, dlen, blen;
  int i;
  assert(comment && dtd);
  clen = (size_t)sprintf(comment, "<r><!--");
  memset(comment + clen, 'q', comment_len);
  clen += comment_len;
  clen += (size_t)sprintf(comment + clen, "--></r>");

  dlen = (size_t)sprintf(dtd, "<!DOCTYPE r [<!ENTITY long '");
  memset(dtd + dlen, 'z', dtd_len);
  dlen += dtd_len;
  dlen += (size_t)sprintf(dtd + dlen, "'>]><r>&long;<!--");
  body = (char *)malloc(comment_len + 32);
  assert(body);
  memset(body, 'w', comment_len);
  blen = comment_len;
  blen += (size_t)sprintf(body + blen, "--></r>");
  dtd = (char *)realloc(dtd, dlen + blen + 1);
  assert(dtd);
  memcpy(dtd + dlen, body, blen + 1);
  dlen += blen;
  free(body);

  for (i = 0; i < 3; i++) {
    int mode, enabled;
    for (mode = 0; mode < 2; mode++) {
      for (enabled = 0; enabled < 2; enabled++) {
        struct event_log log;
        XML_Parser p = XML_ParserCreate(NULL);
        assert(p);
        memset(&log, 0, sizeof(log));
        attach_handlers(p, &log);
        XML_SetReparseDeferralEnabled(p, enabled ? XML_TRUE : XML_FALSE);
        assert(feed_all(p, comment, clen, chunks[i], mode)
               == XML_STATUS_OK);
        assert(log.kind[1] == EV_COMMENT);
        XML_ParserFree(p);

        p = XML_ParserCreate(NULL);
        assert(p);
        memset(&log, 0, sizeof(log));
        attach_handlers(p, &log);
        XML_SetReparseDeferralEnabled(p, enabled ? XML_TRUE : XML_FALSE);
        assert(feed_all(p, dtd, dlen, chunks[i], mode) == XML_STATUS_OK);
        assert(log.kind[0] == EV_DOCTYPE_START);
        assert(log.kind[1] == EV_DOCTYPE_END);
        XML_ParserFree(p);
      }
    }
  }
  free(comment);
  free(dtd);
  printf("PASS long_comment_and_dtd\n");
}

/* --------------------------------------------------------------- */
/* 6. Stop / resume without duplicated events                      */
/* --------------------------------------------------------------- */

struct stop_state {
  struct event_log log;
  int stop_armed;
  const char *stop_name;
  int resumable;
};

static void XMLCALL
stop_start_real(void *ud, const XML_Char *name, const XML_Char **atts) {
  XML_Parser p = (XML_Parser)ud;
  struct stop_state *st = (struct stop_state *)XML_GetUserData(p);
  UNUSED(atts);
  log_event(&st->log, EV_START, name);
  if (st->stop_armed && st->stop_name && ! strcmp(name, st->stop_name)) {
    st->stop_armed = 0;
    assert(XML_StopParser(p, st->resumable ? XML_TRUE : XML_FALSE)
           == XML_STATUS_OK);
  }
}

static void XMLCALL
stop_end_real(void *ud, const XML_Char *name) {
  XML_Parser p = (XML_Parser)ud;
  struct stop_state *st = (struct stop_state *)XML_GetUserData(p);
  log_event(&st->log, EV_END, name);
}

static void
test_stop_resume(void) {
  static const char doc[] = "<r><a/><b/><c/></r>";
  struct event_log reference;
  struct stop_state st;
  XML_ParsingStatus parsing_status;
  XML_Parser p;
  size_t pos = 0;
  const size_t doc_len = strlen(doc);
  int chunk = 3;
  enum XML_Status status;

  /* Reference sequence from an uninterrupted parse. */
  p = XML_ParserCreate(NULL);
  assert(p);
  memset(&reference, 0, sizeof(reference));
  attach_handlers(p, &reference);
  assert(feed_all(p, doc, doc_len, 4, 0) == XML_STATUS_OK);
  XML_ParserFree(p);

  p = XML_ParserCreate(NULL);
  assert(p);
  memset(&st, 0, sizeof(st));
  st.stop_armed = 1;
  st.stop_name = "a";
  st.resumable = 1;
  XML_SetUserData(p, &st);
  XML_UseParserAsHandlerArg(p);
  XML_SetElementHandler(p, stop_start_real, stop_end_real);

  while (pos < doc_len) {
    int take = (int)((doc_len - pos < (size_t)chunk) ? doc_len - pos
                                                     : (size_t)chunk);
    int events_before = st.log.count;
    status = XML_Parse(p, doc + pos, take, 0);
    assert(status == XML_STATUS_OK || status == XML_STATUS_SUSPENDED);
    /* Resume with no new bytes: buffered content must be processed
       without throttling and without re-emitting earlier events. */
    while (status == XML_STATUS_SUSPENDED) {
      int resume_events = st.log.count;
      status = XML_ResumeParser(p);
      assert(status == XML_STATUS_OK || status == XML_STATUS_SUSPENDED);
      assert(st.log.count >= resume_events);
    }
    /* No event may be emitted twice. */
    assert(st.log.count >= events_before);
    XML_GetParsingStatus(p, &parsing_status);
    assert(parsing_status.parsing == XML_PARSING);
    pos += (size_t)take;
  }
  assert(XML_Parse(p, NULL, 0, 1) == XML_STATUS_OK);
  assert(st.log.count == reference.count);
  assert(! memcmp(st.log.kind, reference.kind,
                  sizeof(int) * (size_t)reference.count));
  for (pos = 0; pos < (size_t)reference.count; pos++)
    assert(! strcmp(st.log.name[pos], reference.name[pos]));
  XML_ParserFree(p);
  printf("PASS stop_resume\n");
}

/* Suspension while a long token is under deferral: the rescan that
   crosses the geometric threshold completes the start tag inside a
   single processor call, a handler suspends, and XML_ResumeParser must
   keep going with no new bytes available -- without being throttled and
   without re-emitting the events already delivered. */
static void
test_stop_resume_long_token(void) {
  static const size_t n = 2048;
  static const char head[] = "<r a='";
  static const char middle[] = "'>";
  static const char tail[] = "<b/></r>";
  size_t len = (sizeof(head) - 1) + n + (sizeof(middle) - 1)
               + (sizeof(tail) - 1);
  size_t first_feed = 600;
  char *doc = (char *)malloc(len + 1);
  struct stop_state st;
  XML_ParsingStatus parsing_status;
  XML_Parser p;
  enum XML_Status status;
  assert(doc);
  memcpy(doc, head, sizeof(head) - 1);
  memset(doc + (sizeof(head) - 1), 'x', n);
  memcpy(doc + (sizeof(head) - 1) + n, middle, sizeof(middle) - 1);
  memcpy(doc + (sizeof(head) - 1) + n + (sizeof(middle) - 1), tail,
         sizeof(tail));
  doc[len] = '\0';
  assert(first_feed < len);

  p = XML_ParserCreate(NULL);
  assert(p);
  memset(&st, 0, sizeof(st));
  st.stop_armed = 1; /* suspend from the start handler of element b */
  st.stop_name = "b";
  st.resumable = 1;
  XML_SetUserData(p, &st);
  XML_UseParserAsHandlerArg(p);
  XML_SetElementHandler(p, stop_start_real, stop_end_real);

  /* Partial tag, non-final: deferral arms a water mark, no event. */
  assert(XML_Parse(p, doc, (int)first_feed, 0) == XML_STATUS_OK);
  assert(st.log.count == 0);
  /* The remaining bytes cross the geometric threshold in one feed;
     the rescan completes <r> and reaches <b/>, which suspends.  The
     empty element's end handler still follows its start handler. */
  status = XML_Parse(p, doc + first_feed, (int)(len - first_feed), 0);
  assert(status == XML_STATUS_SUSPENDED);
  assert(st.log.count == 3);
  assert(st.log.kind[0] == EV_START && ! strcmp(st.log.name[0], "r"));
  assert(st.log.kind[1] == EV_START && ! strcmp(st.log.name[1], "b"));
  assert(st.log.kind[2] == EV_END && ! strcmp(st.log.name[2], "b"));

  /* Resume without any new byte: the rest of the buffered input is
     processed immediately and nothing is repeated. */
  status = XML_ResumeParser(p);
  assert(status == XML_STATUS_OK);
  assert(st.log.count == 4);
  assert(st.log.kind[3] == EV_END && ! strcmp(st.log.name[3], "r"));
  XML_GetParsingStatus(p, &parsing_status);
  assert(parsing_status.parsing == XML_PARSING);

  assert(XML_Parse(p, NULL, 0, 1) == XML_STATUS_OK);
  XML_GetParsingStatus(p, &parsing_status);
  assert(parsing_status.parsing == XML_FINISHED);
  assert(st.log.count == 4);
  XML_ParserFree(p);
  free(doc);
  printf("PASS stop_resume_long_token\n");
}

/* A non-resumable stop terminates the parser and must not leave any
   token water mark behind; XML_ParserReset then yields a fresh,
   deferral-enabled parser usable for another document. */
static void
test_nonresumable_stop(void) {
  static const size_t n = 2048;
  size_t len;
  char *doc = make_attr_doc(n, &len);
  struct stop_state st;
  XML_ParsingStatus parsing_status;
  struct event_log log;
  XML_Parser p = XML_ParserCreate(NULL);
  assert(p);
  memset(&st, 0, sizeof(st));
  st.stop_armed = 1;
  st.stop_name = "r";
  XML_SetUserData(p, &st);
  XML_UseParserAsHandlerArg(p);
  XML_SetElementHandler(p, stop_start_real, stop_end_real);
  /* Feed enough to cross the threshold so the start tag is delivered
     and the handler aborts the parser non-resumably. */
  assert(XML_Parse(p, doc, (int)len, 0) == XML_STATUS_ERROR);
  assert(XML_GetErrorCode(p) == XML_ERROR_ABORTED);
  XML_GetParsingStatus(p, &parsing_status);
  assert(parsing_status.parsing == XML_FINISHED);

  /* Reset the same parser and parse a different, smaller document: no
     leftover water mark may throttle or otherwise affect it, and
     deferral is re-enabled by the reset. */
  assert(XML_ParserReset(p, NULL) == XML_TRUE);
  memset(&log, 0, sizeof(log));
  attach_handlers(p, &log);
  assert(feed_all(p, "<a><b/></a>", (size_t)11, 3, 0) == XML_STATUS_OK);
  assert(log.count == 4);
  XML_ParserFree(p);
  free(doc);
  printf("PASS nonresumable_stop\n");
}

/* --------------------------------------------------------------- */
/* 7. Runtime toggle                                               */
/* --------------------------------------------------------------- */

static void
test_runtime_toggle(void) {
  static const size_t n = 4096;
  size_t len;
  char *doc = make_attr_doc(n, &len);
  struct event_log log;
  XML_Parser p = XML_ParserCreate(NULL);
  size_t first_half = 64 * 8;
  assert(p);
  assert(first_half < len);
  memset(&log, 0, sizeof(log));
  attach_handlers(p, &log);
  /* Deferral is on by default; feed part of the value. */
  assert(XML_Parse(p, doc, (int)first_half, 0) == XML_STATUS_OK);
  assert(log.count == 0);
  /* Disable: the next feed would attempt the token immediately. */
  assert(XML_SetReparseDeferralEnabled(p, XML_FALSE) == XML_TRUE);
  /* Re-enable before completion: state is rebuilt from the bytes still
     unconsumed, never inherited from the earlier token's water mark. */
  assert(XML_SetReparseDeferralEnabled(p, XML_TRUE) == XML_TRUE);
  assert(feed_all(p, doc + first_half, len - first_half, 64, 0)
         == XML_STATUS_OK);
  assert(log.count == 2);
  XML_ParserFree(p);
  free(doc);
  printf("PASS runtime_toggle\n");
}

/* Deterministic callback timing.  A long start-tag attribute is fed
   non-final.  The quote that closes the start tag and the rest of the
   single-root document arrive in further NON-final feeds: with
   deferral enabled no callback may fire until the zero-length final
   feed, with it disabled the completed start tag is reported as soon
   as its closing quote is parsed. */
static void
test_callback_deferral_timing(void) {
  static const size_t n = 2048;
  static const char head[] = "<r a='";
  static const char close_tag[] = "'>";
  static const char rest[] = "<s/></r>";
  size_t open_len = sizeof(head) - 1 + n; /* no closing quote yet */
  size_t full_len = open_len + (sizeof(close_tag) - 1) + (sizeof(rest) - 1);
  char *doc = (char *)malloc(full_len + 1);
  struct event_log log;
  XML_Parser p;
  size_t pos;
  assert(doc);
  memcpy(doc, head, sizeof(head) - 1);
  memset(doc + (sizeof(head) - 1), 'x', n);
  memcpy(doc + open_len, close_tag, sizeof(close_tag) - 1);
  memcpy(doc + open_len + (sizeof(close_tag) - 1), rest, sizeof(rest));
  doc[full_len] = '\0';

  /* Deferral enabled: nothing may be reported before the final feed. */
  p = XML_ParserCreate(NULL);
  assert(p);
  memset(&log, 0, sizeof(log));
  attach_handlers(p, &log);
  pos = 0;
  while (pos < open_len) {
    int take = (int)((open_len - pos < 64U) ? open_len - pos : 64U);
    assert(XML_Parse(p, doc + pos, take, 0) == XML_STATUS_OK);
    pos += (size_t)take;
  }
  assert(log.count == 0);
  /* Closing the start tag on a non-final feed still stays deferred:
     the geometric threshold cannot be reached by this ~2 KB token. */
  assert(XML_Parse(p, doc + open_len, (int)(sizeof(close_tag) - 1), 0)
         == XML_STATUS_OK);
  assert(log.count == 0);
  /* The remaining small tokens stay deferred as well. */
  assert(XML_Parse(p, doc + open_len + (sizeof(close_tag) - 1),
                   (int)(sizeof(rest) - 1), 0)
         == XML_STATUS_OK);
  assert(log.count == 0);
  /* The zero-length final feed drains everything, exactly once. */
  assert(XML_Parse(p, NULL, 0, 1) == XML_STATUS_OK);
  assert(log.count == 4);
  assert(log.kind[0] == EV_START && ! strcmp(log.name[0], "r"));
  assert(log.kind[1] == EV_START && ! strcmp(log.name[1], "s"));
  assert(log.kind[2] == EV_END && ! strcmp(log.name[2], "s"));
  assert(log.kind[3] == EV_END && ! strcmp(log.name[3], "r"));
  XML_ParserFree(p);

  /* Deferral disabled: the completed start tag is reported on the
     non-final feed that contains its closing quote. */
  p = XML_ParserCreate(NULL);
  assert(p);
  memset(&log, 0, sizeof(log));
  attach_handlers(p, &log);
  XML_SetReparseDeferralEnabled(p, XML_FALSE);
  pos = 0;
  while (pos < open_len) {
    int take = (int)((open_len - pos < 64U) ? open_len - pos : 64U);
    assert(XML_Parse(p, doc + pos, take, 0) == XML_STATUS_OK);
    pos += (size_t)take;
  }
  assert(log.count == 0);
  assert(XML_Parse(p, doc + open_len, (int)(sizeof(close_tag) - 1), 0)
         == XML_STATUS_OK);
  assert(log.count == 1);
  assert(log.kind[0] == EV_START && ! strcmp(log.name[0], "r"));
  assert(XML_Parse(p, doc + open_len + (sizeof(close_tag) - 1),
                   (int)(sizeof(rest) - 1), 0)
         == XML_STATUS_OK);
  assert(log.count == 4);
  assert(XML_Parse(p, NULL, 0, 1) == XML_STATUS_OK);
  XML_ParserFree(p);

  free(doc);
  printf("PASS callback_deferral_timing\n");
}

/* --------------------------------------------------------------- */
/* 8. Reset clears token state and re-enables deferral             */
/* --------------------------------------------------------------- */

static void
test_reset(void) {
  static const size_t n = 2048;
  size_t len;
  char *doc = make_attr_doc(n, &len);
  struct event_log log;
  XML_Parser p = XML_ParserCreate(NULL);
  assert(p);
  XML_SetReparseDeferralEnabled(p, XML_FALSE);
  /* Leave an incomplete token pending (non-final). */
  assert(XML_Parse(p, doc, (int)(len < 256U ? len : 256), 0)
         == XML_STATUS_OK);
  assert(XML_ParserReset(p, NULL) == XML_TRUE);

  /* Reset must re-enable deferral and drop all pending token state. */
#ifdef CHECK_SCAN_PROBE
  {
    size_t rlen;
    char *rdoc = make_attr_doc(65536, &rlen);
    unit_scan_work = 0;
    memset(&log, 0, sizeof(log));
    attach_handlers(p, &log);
    assert(feed_all(p, rdoc, rlen, 64, 0) == XML_STATUS_OK);
    assert(unit_scan_work <= 8ULL * 65536ULL + 4096ULL);
    assert(log.count == 2);
    free(rdoc);
  }
#else
  memset(&log, 0, sizeof(log));
  attach_handlers(p, &log);
  assert(feed_all(p, doc, len, 64, 0) == XML_STATUS_OK);
  assert(log.count == 2);
#endif
  XML_ParserFree(p);
  free(doc);
  printf("PASS reset\n");
}

/* --------------------------------------------------------------- */
/* 9. External entity inheritance via in-memory callbacks          */
/* --------------------------------------------------------------- */

struct ext_case {
  const char *entity;
  size_t entity_len;
  int flip_child_off;
  int observed;
  unsigned long long work;
};

static int XMLCALL
external_entity_handler(XML_Parser parent, const XML_Char *context,
                        const XML_Char *base, const XML_Char *systemId,
                        const XML_Char *publicId) {
  struct ext_case *tc = (struct ext_case *)XML_GetUserData(parent);
  XML_Parser child;
  size_t pos = 0;
  UNUSED(base);
  UNUSED(systemId);
  UNUSED(publicId);
  child = XML_ExternalEntityParserCreate(parent, context, NULL);
  if (child == NULL)
    return 0;
  if (tc->flip_child_off)
    XML_SetReparseDeferralEnabled(child, XML_FALSE);
#ifdef CHECK_SCAN_PROBE
  unit_scan_work = 0;
#endif
  while (pos < tc->entity_len) {
    int take = (int)((tc->entity_len - pos < 64U) ? tc->entity_len - pos
                                                  : 64U);
    if (XML_Parse(child, tc->entity + pos, take,
                  pos + (size_t)take == tc->entity_len)
        != XML_STATUS_OK) {
      XML_ParserFree(child);
      return 0;
    }
    pos += (size_t)take;
  }
#ifdef CHECK_SCAN_PROBE
  tc->work = unit_scan_work;
  tc->observed = 1;
#endif
  XML_ParserFree(child);
  return 1;
}

static void
run_external_case(int parent_enabled, int flip_child_off,
                  unsigned long long *work_out) {
  static const size_t n = 16384;
  static const char wrapper[] =
      "<!DOCTYPE r [<!ENTITY ext SYSTEM \"mem:entity\">]><r>&ext;</r>";
  char *entity = (char *)malloc(32 + n);
  struct ext_case tc;
  XML_Parser p;
  size_t elen = 0;
  assert(entity);
  elen += (size_t)sprintf(entity + elen, "<inner a='");
  memset(entity + elen, 'x', n);
  elen += n;
  elen += (size_t)sprintf(entity + elen, "'>z</inner>");

  tc.entity = entity;
  tc.entity_len = elen;
  tc.flip_child_off = flip_child_off;
  tc.observed = 0;
  tc.work = 0;

  p = XML_ParserCreate(NULL);
  assert(p);
  XML_SetUserData(p, &tc);
  /* Default external entity ref handler arg is the parser itself. */
  XML_SetExternalEntityRefHandler(p, external_entity_handler);
  XML_SetReparseDeferralEnabled(p, parent_enabled ? XML_TRUE : XML_FALSE);
  assert(feed_all(p, wrapper, strlen(wrapper), 32, 0) == XML_STATUS_OK);
  XML_ParserFree(p);
#ifdef CHECK_SCAN_PROBE
  assert(tc.observed);
  *work_out = tc.work;
#else
  *work_out = 0;
#endif
  free(entity);
}

static void
test_external_entities(void) {
  unsigned long long work;
#ifdef CHECK_SCAN_PROBE
  const unsigned long long bound = 8ULL * 16384ULL + 4096ULL;

  /* Parent enabled: child inherits the enabled switch. */
  run_external_case(1, 0, &work);
  assert(work <= bound);

  /* Parent disabled: child inherits the disabled switch and keeps the
     visible repeated rescans. */
  run_external_case(0, 0, &work);
  assert(work > bound);

  /* Parent enabled but the child switch is toggled independently. */
  run_external_case(1, 1, &work);
  assert(work > bound);
#else
  run_external_case(1, 0, &work);
  run_external_case(0, 0, &work);
  run_external_case(1, 1, &work);
#endif
  printf("PASS external_entities (inheritance and independent toggle)\n");
}

/* --------------------------------------------------------------- */
/* 10. Final error categories and byte positions                   */
/* --------------------------------------------------------------- */

struct error_result {
  enum XML_Error code;
  XML_Index byte_index;
};

static struct error_result
final_error(const char *doc, size_t len, int enabled, int buffered) {
  struct error_result r;
  XML_Parser p = XML_ParserCreate(NULL);
  assert(p);
  XML_SetReparseDeferralEnabled(p, enabled ? XML_TRUE : XML_FALSE);
  if (feed_all(p, doc, len, 37, buffered) != XML_STATUS_OK) {
    r.code = XML_GetErrorCode(p);
    r.byte_index = XML_GetCurrentByteIndex(p);
  } else {
    /* The document may only fail on a zero-length final feed. */
    if (XML_Parse(p, NULL, 0, 1) != XML_STATUS_OK) {
      r.code = XML_GetErrorCode(p);
      r.byte_index = XML_GetCurrentByteIndex(p);
    } else {
      r.code = XML_ERROR_NONE;
      r.byte_index = -1;
    }
  }
  XML_ParserFree(p);
  return r;
}

static void
assert_error_matches(const char *label, const char *doc, size_t len,
                     enum XML_Error expected) {
  int mode, enabled;
  struct error_result baseline = {XML_ERROR_NONE, -1};
  for (mode = 0; mode < 2; mode++) {
    for (enabled = 0; enabled < 2; enabled++) {
      struct error_result r = final_error(doc, len, enabled, mode);
      assert(r.code == expected);
      if (mode == 0 && enabled == 0)
        baseline = r;
      else
        assert(r.byte_index == baseline.byte_index);
    }
  }
  printf("PASS final_error:%s (code=%d index=%lld)\n", label,
         (int)expected, (long long)baseline.byte_index);
}

static void
test_final_errors(void) {
  /* Truncated attribute value (no closing quote). */
  {
    static const char doc[] = "<r a='abcdefghijklmnopqrstuvwxyz";
    assert_error_matches("truncated_attribute", doc, strlen(doc),
                         XML_ERROR_UNCLOSED_TOKEN);
  }
  /* Truncated comment. */
  {
    static const char doc[] = "<r><!-- abcdefghijklmnopqrstuvwxyz";
    assert_error_matches("truncated_comment", doc, strlen(doc),
                         XML_ERROR_UNCLOSED_TOKEN);
  }
  /* Truncated DOCTYPE declaration. */
  {
    static const char doc[] = "<!DOCTYPE r [<!ENTITY long 'abcdefgh";
    assert_error_matches("truncated_doctype", doc, strlen(doc),
                         XML_ERROR_UNCLOSED_TOKEN);
  }
  /* Truncated multibyte UTF-8 character: lead byte 0xC2 with no
     continuation byte. */
  {
    char doc[16];
    size_t len = (size_t)sprintf(doc, "<r>%c", (char)0xC2);
    assert_error_matches("truncated_multibyte", doc, len,
                         XML_ERROR_PARTIAL_CHAR);
  }
  /* Invalid byte inside the long attribute: deferral must not change
     the error category or its byte position. */
  {
    size_t n = 512;
    size_t len;
    char *doc = make_attr_doc(n, &len);
    doc[20] = (char)0x01; /* illegal XML character */
    assert_error_matches("invalid_in_attribute", doc, len,
                         XML_ERROR_INVALID_TOKEN);
    free(doc);
  }
}

/* --------------------------------------------------------------- */
/* Driver                                                          */
/* --------------------------------------------------------------- */

int
main(void) {
  test_api_basics();
  test_default_enabled();
  test_scan_count_bounds();
  test_event_sequence();
  test_final_boundaries();
  test_zero_length_final();
  test_buffer_moves();
  test_utf16_boundaries();
  test_long_comment_and_dtd();
  test_stop_resume();
  test_stop_resume_long_token();
  test_nonresumable_stop();
  test_runtime_toggle();
  test_callback_deferral_timing();
  test_reset();
  test_external_entities();
  test_final_errors();
  puts("reparse_contract: ALL TESTS PASSED");
  return 0;
}
