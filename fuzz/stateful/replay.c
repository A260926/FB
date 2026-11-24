#include "fuzz/stateful/model.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------ parsing --------------------------------- */

static int parse_u64(const char *s, uint64_t *out) {
  char *end;
  unsigned long long v;
  size_t i;

  if (!s || !*s) return 0;
  /* The grammar uses non-negative decimal integers and no signs. */
  for (i = 0; s[i] != '\0'; ++i) {
    if (s[i] < '0' || s[i] > '9') return 0;
  }
  errno = 0;
  v = strtoull(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return 0;
  *out = (uint64_t)v;
  return 1;
}

static int parse_stream(const char *s, int64_t *out) {
  uint64_t v;
  if (!parse_u64(s, &v)) return 0;
  if (v > NGHTTP3_MAX_VARINT || (v % 4U) != 0) return 0;
  *out = (int64_t)v;
  return 1;
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int decode_hex(const char *s, uint8_t **out, size_t *outlen) {
  size_t chars, i;
  uint8_t *buf;

  if (!s) return 0;
  chars = strlen(s);
  if (chars == 0 || (chars % 2U) != 0) return 0;
  buf = malloc(chars / 2U ? chars / 2U : 1);
  if (!buf) return -1;

  for (i = 0; i < chars; i += 2) {
    int hi = hex_nibble(s[i]);
    int lo = hex_nibble(s[i + 1]);
    if (hi < 0 || lo < 0) {
      free(buf);
      return 0;
    }
    buf[i / 2] = (uint8_t)((hi << 4) | lo);
  }
  *out = buf;
  *outlen = chars / 2U;
  return 1;
}

static int valid_lower_name(const uint8_t *p, size_t n) {
  size_t i;
  if (n == 0 || n > NGHTTP3_QPACK_MAX_NAMELEN) return 0;
  for (i = 0; i < n; ++i) {
    uint8_t c = p[i];
    if (c <= 0x20U || c >= 0x7fU) return 0;
    if (c >= 'A' && c <= 'Z') return 0;
  }
  return 1;
}

static int valid_visible_value(const uint8_t *p, size_t n) {
  size_t i;
  if (n > NGHTTP3_QPACK_MAX_VALUELEN) return 0;
  for (i = 0; i < n; ++i) {
    if (p[i] < 0x20U || p[i] > 0x7eU) return 0;
  }
  return 1;
}

static void print_diag(FILE *out, const qm_model *m) {
  const qm_diag *d = &m->diag;
  const char *category = d->category == QM_CATEGORY_SCRIPT ? "script"
                       : d->category == QM_CATEGORY_MODEL ? "model"
                       : "runtime";

  fprintf(out, "ERROR category=%s step=%lu line=%lu op=%s kind=%s",
          category, d->step, d->line,
          d->op[0] ? d->op : "?", d->kind[0] ? d->kind : "unspecified");
  if (d->has_stream) {
    fprintf(out, " stream=%" PRId64, d->stream);
  } else {
    fputs(" stream=-", out);
  }
  if (d->expected[0]) fprintf(out, " expected='%s'", d->expected);
  if (d->actual[0]) fprintf(out, " actual='%s'", d->actual);
  if (d->detail[0]) fprintf(out, " detail='%s'", d->detail);
  fputc('\n', out);
}

static void report_raw(FILE *out, unsigned long line, const char *op,
                       const char *kind, const char *detail) {
  fprintf(out,
          "ERROR category=script step=1 line=%lu op=%s kind=%s stream=- "
          "detail='%s'\n",
          line, op ? op : "INIT", kind ? kind : "invalid_script",
          detail ? detail : "malformed command");
}

static int count_tokens(char **tokens, int max_tokens, char *line) {
  char *save = NULL;
  char *tok;
  int n = 0;

  tok = strtok_r(line, " \t\r\n", &save);
  while (tok && n < max_tokens) {
    tokens[n++] = tok;
    tok = strtok_r(NULL, " \t\r\n", &save);
  }
  /* If another token remains, signal arity overflow by returning max+1. */
  if (tok) return max_tokens + 1;
  return n;
}

int qpack_model_replay(FILE *input, FILE *output) {
  qm_model m;
  int started = 0;
  int ended = 0;
  int rc = 0;
  char *line = NULL;
  size_t line_cap = 0;
  ssize_t line_len;
  unsigned long line_no = 0;

  if (!input || !output) return 1;

  while ((line_len = getline(&line, &line_cap, input)) >= 0) {
    char *tok[8];
    int ntok;
    char *cmd;

    (void)line_len;
    ++line_no;

    if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r' ||
        line[0] == '#') {
      continue;
    }

    ntok = count_tokens(tok, (int)(sizeof(tok) / sizeof(tok[0])), line);
    if (ntok <= 0) continue;
    cmd = tok[0];

    if (strcmp(cmd, "INIT") == 0) {
      uint64_t capacity = 0, blocked = 0;
      if (started) {
        report_raw(output, line_no, "INIT", "duplicate_init",
                   "INIT may appear exactly once");
        rc = 1;
        goto done;
      }
      if (ntok != 3 || !parse_u64(tok[1], &capacity) ||
          !parse_u64(tok[2], &blocked) || capacity < 64 ||
          capacity > 4096 || blocked < 1 || blocked > 8) {
        report_raw(output, line_no, "INIT", "bad_arguments",
                   "expected INIT CAPACITY(64..4096) BLOCKED_LIMIT(1..8)");
        rc = 1;
        goto done;
      }
      if (qm_model_init(&m, (size_t)capacity, (size_t)blocked, output) != 0) {
        report_raw(output, line_no, "INIT", "resource_error",
                   "could not create real QPACK endpoints");
        rc = 2;
        goto done;
      }
      started = 1;
      m.line = line_no;
      m.step = 1;
      continue;
    }

    if (!started) {
      report_raw(output, line_no, cmd, "init_required",
                 "first command must be INIT");
      rc = 1;
      goto done;
    }

    m.line = line_no;
    ++m.step;

    if (ended) {
      rc = qm_fail(&m, QM_CATEGORY_SCRIPT, cmd, "after_end", 0, 0,
                   "no commands after END", "command after END", NULL);
      print_diag(output, &m);
      goto done;
    }

    if (strcmp(cmd, "SECTION") == 0) {
      int64_t stream_id;
      uint8_t *name = NULL, *value = NULL;
      size_t namelen = 0, valuelen = 0;
      int decoded;

      if (ntok != 4 || !parse_stream(tok[1], &stream_id)) {
        rc = qm_fail(&m, QM_CATEGORY_SCRIPT, "SECTION", "bad_arguments",
                     1, ntok >= 2 ? (int64_t)strtoll(tok[1], NULL, 10) : 0,
                     "SECTION STREAM(>=0,mult4) NAME_HEX VALUE_HEX|-",
                     "malformed arguments", NULL);
        print_diag(output, &m);
        goto done;
      }
      decoded = decode_hex(tok[2], &name, &namelen);
      if (decoded <= 0 || !valid_lower_name(name, namelen)) {
        free(name);
        rc = qm_fail(&m, QM_CATEGORY_SCRIPT, "SECTION", "bad_name",
                     1, stream_id, "nonempty lowercase ASCII name hex",
                     "invalid NAME_HEX", NULL);
        print_diag(output, &m);
        goto done;
      }
      if (strcmp(tok[3], "-") == 0) {
        value = NULL;
        valuelen = 0;
      } else {
        decoded = decode_hex(tok[3], &value, &valuelen);
        if (decoded <= 0 || !valid_visible_value(value, valuelen)) {
          free(name);
          free(value);
          rc = qm_fail(&m, QM_CATEGORY_SCRIPT, "SECTION", "bad_value",
                       1, stream_id, "visible ASCII value hex or -",
                       "invalid VALUE_HEX", NULL);
          print_diag(output, &m);
          goto done;
        }
      }

      rc = qm_cmd_section(&m, stream_id, name, namelen, value, valuelen);
      free(name);
      free(value);
      if (rc) {
        print_diag(output, &m);
        goto done;
      }
      continue;
    }

    if (strcmp(cmd, "ENCODER") == 0) {
      uint64_t n;
      if (ntok != 2 || !parse_u64(tok[1], &n)) {
        rc = qm_fail(&m, QM_CATEGORY_SCRIPT, "ENCODER", "bad_arguments", 0, 0,
                     "ENCODER N", "malformed N", NULL);
        print_diag(output, &m);
        goto done;
      }
      rc = qm_cmd_encoder(&m, n);
    } else if (strcmp(cmd, "REQUEST") == 0) {
      int64_t stream_id;
      uint64_t n;
      if (ntok != 3 || !parse_stream(tok[1], &stream_id) ||
          !parse_u64(tok[2], &n)) {
        rc = qm_fail(&m, QM_CATEGORY_SCRIPT, "REQUEST", "bad_arguments",
                     1, ntok >= 2 ? (int64_t)strtoll(tok[1], NULL, 10) : 0,
                     "REQUEST STREAM(>=0,mult4) N", "malformed arguments",
                     NULL);
        print_diag(output, &m);
        goto done;
      }
      rc = qm_cmd_request(&m, stream_id, n);
    } else if (strcmp(cmd, "FEEDBACK") == 0) {
      uint64_t n;
      if (ntok != 2 || !parse_u64(tok[1], &n)) {
        rc = qm_fail(&m, QM_CATEGORY_SCRIPT, "FEEDBACK", "bad_arguments", 0,
                     0, "FEEDBACK N", "malformed N", NULL);
        print_diag(output, &m);
        goto done;
      }
      rc = qm_cmd_feedback(&m, n);
    } else if (strcmp(cmd, "CANCEL") == 0) {
      int64_t stream_id;
      if (ntok != 2 || !parse_stream(tok[1], &stream_id)) {
        rc = qm_fail(&m, QM_CATEGORY_SCRIPT, "CANCEL", "bad_arguments", 1,
                     ntok >= 2 ? (int64_t)strtoll(tok[1], NULL, 10) : 0,
                     "CANCEL STREAM(>=0,mult4)", "malformed stream", NULL);
        print_diag(output, &m);
        goto done;
      }
      rc = qm_cmd_cancel(&m, stream_id);
    } else if (strcmp(cmd, "END") == 0) {
      if (ntok != 1) {
        rc = qm_fail(&m, QM_CATEGORY_SCRIPT, "END", "bad_arguments", 0, 0,
                     "END", "extra tokens", NULL);
        print_diag(output, &m);
        goto done;
      }
      rc = qm_cmd_end(&m);
      if (rc) {
        print_diag(output, &m);
        goto done;
      }
      ended = 1;
      fprintf(output,
              "COMPLETE %u CANCELED %u\nCONSISTENT\n",
              m.completed_sections, m.canceled_streams);
    } else {
      rc = qm_fail(&m, QM_CATEGORY_SCRIPT, cmd, "unknown_command", 0, 0,
                   "INIT/SECTION/ENCODER/REQUEST/FEEDBACK/CANCEL/END",
                   cmd, NULL);
    }

    if (rc) {
      print_diag(output, &m);
      goto done;
    }
  }

  if (!started) {
    report_raw(output, line_no + 1, "INPUT", "empty_script",
               "expected INIT ... END");
    rc = 1;
  } else if (!ended) {
    m.line = line_no + 1;
    rc = qm_fail(&m, QM_CATEGORY_SCRIPT, "EOF", "missing_end", 0, 0,
                 "END before EOF", "no END command", NULL);
    print_diag(output, &m);
  }

done:
  free(line);
  if (started) qm_model_destroy(&m);
  fflush(output);
  return rc ? 1 : 0;
}

/* ------------------------- deterministic generator ---------------------- */

typedef struct qm_rng {
  uint64_t state;
} qm_rng;

static uint64_t qm_rng_next(qm_rng *r) {
  uint64_t z = (r->state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

static unsigned qm_rng_below(qm_rng *r, unsigned n) {
  if (n == 0) return 0;
  return (unsigned)(qm_rng_next(r) % n);
}

typedef struct qm_gen_stream {
  int used;
  int canceled;
  int safe_cancel;
} qm_gen_stream;

static const char *qm_hex_name(unsigned i) {
  static const char *names[] = {
    "61",       /* a */
    "62",       /* b */
    "6c696e6b", /* link */
    "782d6b",   /* x-k */
    "636174",   /* cat */
    "7a"        /* z */
  };
  return names[i % (sizeof(names) / sizeof(names[0]))];
}

static const char *qm_hex_value(unsigned i) {
  static const char *values[] = {
    "78", "79", "31", "7a", "666f6f", "626172"
  };
  return values[i % (sizeof(values) / sizeof(values[0]))];
}

int qpack_model_generate(uint64_t seed, unsigned int steps, FILE *out) {
  qm_rng r;
  qm_gen_stream streams[24];
  unsigned i;
  unsigned active = 0;
  uint64_t capacities[] = {64, 128, 256, 512, 1024, 4096};
  uint64_t capacity;
  unsigned blocked;

  if (!out) return 1;
  r.state = seed ? seed : 1;
  memset(streams, 0, sizeof(streams));

  capacity = capacities[qm_rng_below(&r, sizeof(capacities) /
                                          sizeof(capacities[0]))];
  blocked = 1 + qm_rng_below(&r, 4);
  fprintf(out, "INIT %" PRIu64 " %u\n", capacity, blocked);

  for (i = 0; i < steps; ++i) {
    unsigned action = qm_rng_below(&r, 100);
    int idx = -1;
    unsigned j;

    if (action < 42) {
      /* Submit a section.  Prefer an existing live stream to create
         same-stream FIFO ordering, but often share dynamic items across
         different streams. */
      if (active > 0 && qm_rng_below(&r, 100) < 55) {
        unsigned pick = qm_rng_below(&r, 24);
        for (j = 0; j < 24; ++j) {
          unsigned k = (pick + j) % 24;
          if (streams[k].used && !streams[k].canceled) {
            idx = (int)k;
            break;
          }
        }
      }
      if (idx < 0) {
        for (j = 0; j < 24; ++j) {
          if (!streams[j].used) {
            idx = (int)j;
            streams[j].used = 1;
            ++active;
            break;
          }
        }
      }
      if (idx < 0) {
        fprintf(out, "ENCODER 4096\n");
        continue;
      }
      streams[idx].safe_cancel = 1;
      fprintf(out, "SECTION %u %s %s\n", 4U * idx,
              qm_hex_name(qm_rng_below(&r, 6)),
              qm_hex_value(qm_rng_below(&r, 6)));
    } else if (action < 58) {
      static const unsigned ns[] = {0, 1, 2, 3, 7, 64, 4096};
      fprintf(out, "ENCODER %u\n", ns[qm_rng_below(&r, 7)]);
    } else if (action < 76) {
      unsigned pick = qm_rng_below(&r, 24);
      for (j = 0; j < 24; ++j) {
        unsigned k = (pick + j) % 24;
        if (streams[k].used && !streams[k].canceled) {
          static const unsigned ns[] = {0, 1, 2, 3, 4096};
          fprintf(out, "REQUEST %u %u\n", 4U * k,
                  ns[qm_rng_below(&r, 5)]);
          streams[k].safe_cancel = 0;
          break;
        }
      }
      if (j == 24) fprintf(out, "ENCODER 1\n");
    } else if (action < 91) {
      static const unsigned ns[] = {0, 1, 2, 3, 4096};
      fprintf(out, "FEEDBACK %u\n", ns[qm_rng_below(&r, 5)]);
    } else {
      unsigned pick = qm_rng_below(&r, 24);
      for (j = 0; j < 24; ++j) {
        unsigned k = (pick + j) % 24;
        if (streams[k].used && !streams[k].canceled &&
            streams[k].safe_cancel) {
          fprintf(out, "CANCEL %u\n", 4U * k);
          streams[k].canceled = 1;
          --active;
          break;
        }
      }
      if (j == 24) {
        fprintf(out, "FEEDBACK 1\n");
      }
    }
  }

  fputs("END\n", out);
  return 0;
}

/* ------------------------------ shrinking ------------------------------- */

static int replay_candidate(const char *text, size_t len, char *errbuf,
                            size_t errbufsz) {
  FILE *in = tmpfile();
  FILE *out = tmpfile();
  int rc;
  char buf[512];
  size_t n;

  if (!in || !out) {
    if (in) fclose(in);
    if (out) fclose(out);
    return -1;
  }
  fwrite(text, 1, len, in);
  rewind(in);
  rc = qpack_model_replay(in, out);
  rewind(out);
  errbuf[0] = '\0';
  while ((n = fread(buf, 1, sizeof(buf), out)) > 0) {
    size_t copy = n < errbufsz - 1 ? n : errbufsz - 1;
    memcpy(errbuf, buf, copy);
    errbuf[copy] = '\0';
    if (copy < n) break;
  }
  fclose(in);
  fclose(out);
  return rc;
}

int qpack_model_shrink_text(const char *script, FILE *report) {
  char **lines = NULL;
  size_t nlines = 0, cap = 0, i;
  const char *p;
  int changed = 1;

  if (!script || !report) return 1;

  /* Split into operation lines.  Greedily delete whole operations while the
     replay still reports a model/runtime failure rather than a script error.
     This prevents shrinkage from replacing a semantic failure with an
     invalid command sequence. */
  p = script;
  while (*p) {
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char *dup;
    if (nlines == cap) {
      cap = cap ? cap * 2 : 16;
      lines = realloc(lines, cap * sizeof(*lines));
      if (!lines) return 1;
    }
    dup = malloc(len + 1);
    if (!dup) return 1;
    memcpy(dup, p, len);
    dup[len] = '\0';
    lines[nlines++] = dup;
    if (!e) break;
    p = e + 1;
  }

  while (changed) {
    changed = 0;
    for (i = 0; i < nlines; ++i) {
      char *candidate;
      size_t total = 1, j, pos = 0;
      int rc;
      char err[1024];
      int is_model_failure;

      if (!lines[i]) continue;
      /* Never remove INIT or END. */
      if (!strncmp(lines[i], "INIT ", 5) || !strcmp(lines[i], "END")) continue;

      for (j = 0; j < nlines; ++j) {
        if (j != i && lines[j]) total += strlen(lines[j]) + 1;
      }
      candidate = malloc(total);
      if (!candidate) break;
      candidate[0] = '\0';
      for (j = 0; j < nlines; ++j) {
        if (j == i || !lines[j]) continue;
        pos += (size_t)snprintf(candidate + pos, total - pos, "%s\n",
                                lines[j]);
      }

      rc = replay_candidate(candidate, pos, err, sizeof(err));
      is_model_failure = rc != 0 && (strstr(err, "category=model") != NULL ||
                                     strstr(err, "category=runtime") != NULL);
      if (is_model_failure) {
        fprintf(report, "shrunk removing line: %s\n", lines[i]);
        free(lines[i]);
        lines[i] = NULL;
        changed = 1;
      }
      free(candidate);
    }
  }

  fputs("SHRUNK_SCRIPT_BEGIN\n", report);
  for (i = 0; i < nlines; ++i) {
    if (lines[i]) fprintf(report, "%s\n", lines[i]);
  }
  fputs("SHRUNK_SCRIPT_END\n", report);

  for (i = 0; i < nlines; ++i) free(lines[i]);
  free(lines);
  return 0;
}
