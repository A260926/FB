/*
  Structured reduction regression tests.

  Three groups are registered with the shared task driver:

    dependency_closure - dependency cycles, cross-interval closure, identical
                         bytes with distinct identities, post-accept rescan,
                         and zero-budget semantics.
    cache_resume       - full-byte cache reuse, distinct-byte separation,
                         error-callback retry, and state save/load round trips.
    tmin_entry         - real afl-tmin processes: stdin and file replay, a
                         two-process checkpoint resume, option rejection, and
                         state identity/integrity failures.

  Only C and POSIX interfaces are used.  The CLI tests drive the real
  afl-tmin binary that links this repository's production run_target().
*/

#include "afl-struct.h"
#include "task_support/check.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Byte-string observation helpers                                    */
/* ------------------------------------------------------------------ */

#define MAX_CANDS 32

typedef struct {
  int calls;
  int max_calls;
  const char *expect[MAX_CANDS];
  int result[MAX_CANDS];
  int mismatch;
  char seen[MAX_CANDS][64];
  size_t seen_len[MAX_CANDS];
} script_ctx;

static int script_observe(const unsigned char *data, size_t len, void *opaque) {
  script_ctx *c = (script_ctx *)opaque;
  int i = c->calls;

  if (i < MAX_CANDS) {
    size_t copy = len < sizeof(c->seen[i]) - 1 ? len : sizeof(c->seen[i]) - 1;
    memcpy(c->seen[i], data, copy);
    c->seen[i][copy] = 0;
    c->seen_len[i] = len;
    if (c->expect[i] && (strlen(c->expect[i]) != len ||
                         memcmp(c->expect[i], data, len) != 0))
      c->mismatch = 1;
  }

  ++c->calls;

  if (i < c->max_calls) return c->result[i];
  return 0;
}

static int contains_byte(const unsigned char *data, size_t len, unsigned char b) {
  size_t i;
  for (i = 0; i < len; ++i)
    if (data[i] == b) return 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Group: dependency_closure                                          */
/* ------------------------------------------------------------------ */

/* Accept while 'f' is present; record the first candidate's bytes. */

typedef struct {
  int calls;
  char first[16];
  size_t first_len;
} marker_ctx;

static int marker_observe(const unsigned char *data, size_t len, void *opaque) {
  marker_ctx *c = (marker_ctx *)opaque;
  if (c->calls == 0) {
    size_t copy = len < sizeof(c->first) - 1 ? len : sizeof(c->first) - 1;
    memcpy(c->first, data, copy);
    c->first[copy] = 0;
    c->first_len = len;
  }
  ++c->calls;
  return contains_byte(data, len, 'f');
}

static int case_dep_cycle_closure_real(void) {
  static const unsigned char input[] = {'a', 'b', 'c', 'd', 'e', 'f'};
  const char *table =
      "1 0 1 : 2\n"
      "2 1 2 : 1\n"
      "3 2 3 : 2\n"
      "4 3 4 : -\n"
      "5 4 5 : 4\n"
      "6 5 6 : -\n";
  afl_struct_session *s;
  marker_ctx ctx;
  const unsigned char *out;
  size_t out_len = 0;
  int rc;

  memset(&ctx, 0, sizeof(ctx));

  s = afl_struct_create(input, sizeof(input), table);
  TASK_CHECK(s != NULL);

  if (!s) return 1;

  rc = afl_struct_step(s, (size_t)-1, marker_observe, &ctx);
  TASK_CHECK(rc == 0);

  /* First candidate removes {1,2,3}: "def". */
  TASK_CHECK(ctx.first_len == 3);
  TASK_CHECK(strcmp(ctx.first, "def") == 0);

  out = afl_struct_data(s, &out_len);
  TASK_CHECK(out_len == 1 && out && out[0] == 'f');

  afl_struct_destroy(s);
  return 0;
}

/* Two fragments with identical text must stay separate identities. */

static int case_same_bytes_distinct_identity(void) {
  static const unsigned char input[] = {'A', 'B', 'A', 'B'};
  const char *table =
      "1 0 2 : -\n"
      "2 2 4 : -\n";
  afl_struct_session *s;
  script_ctx ctx;
  const unsigned char *out;
  size_t out_len = 0;
  int rc;

  memset(&ctx, 0, sizeof(ctx));
  ctx.max_calls = 2;
  ctx.result[0] = 1; /* accept deleting fragment 1, leaving the second "AB" */
  ctx.result[1] = 0; /* reject deleting the last fragment                 */
  ctx.expect[0] = "AB";
  ctx.expect[1] = "";

  s = afl_struct_create(input, sizeof(input), table);
  TASK_CHECK(s != NULL);
  if (!s) return 1;

  rc = afl_struct_step(s, (size_t)-1, script_observe, &ctx);
  TASK_CHECK(rc == 0);
  TASK_CHECK(!ctx.mismatch);

  /* If identical bytes were merged into one ID, the first candidate would be
     empty rather than the surviving copy. */
  TASK_CHECK(ctx.calls == 2);
  TASK_CHECK(ctx.seen_len[0] == 2);
  TASK_CHECK(strcmp(ctx.seen[0], "AB") == 0);

  out = afl_struct_data(s, &out_len);
  TASK_CHECK(out_len == 2 && memcmp(out, "AB", 2) == 0);

  afl_struct_destroy(s);
  return 0;
}

/* After an acceptance the scan restarts from the smallest active ID. */

static int case_accept_rescan(void) {
  static const unsigned char input[] = {'w', 'x', 'y', 'z'};
  const char *table =
      "1 0 1 : -\n"
      "2 1 2 : -\n"
      "3 2 4 : -\n";
  afl_struct_session *s;
  script_ctx ctx;
  const unsigned char *out;
  size_t out_len = 0;
  int rc;

  memset(&ctx, 0, sizeof(ctx));
  ctx.max_calls = 4;
  ctx.result[0] = 0; /* delete 1 -> "xyz": reject            */
  ctx.result[1] = 1; /* delete 2 -> "wyz": accept            */
  ctx.result[2] = 1; /* rescan: delete 1 -> "yz": accept     */
  ctx.result[3] = 0; /* delete 3 -> "": reject               */
  ctx.expect[0] = "xyz";
  ctx.expect[1] = "wyz";
  ctx.expect[2] = "yz";
  ctx.expect[3] = "";

  s = afl_struct_create(input, sizeof(input), table);
  TASK_CHECK(s != NULL);
  if (!s) return 1;

  rc = afl_struct_step(s, (size_t)-1, script_observe, &ctx);
  TASK_CHECK(rc == 0);
  TASK_CHECK(!ctx.mismatch);

  /* The "yz" candidate must be attempted after the earlier IDs restart. */
  TASK_CHECK(ctx.calls == 4);
  TASK_CHECK(ctx.seen_len[2] == 2 && strcmp(ctx.seen[2], "yz") == 0);

  out = afl_struct_data(s, &out_len);
  TASK_CHECK(out_len == 2 && memcmp(out, "yz", 2) == 0);

  afl_struct_destroy(s);
  return 0;
}

/* Invalid tables are rejected by create; valid but empty input is accepted. */

static int case_table_validation(void) {
  static const unsigned char input[] = {'a', 'b', 'c', 'd'};
  afl_struct_session *s;

  /* Missing dependency reference. */
  s = afl_struct_create(input, 4, "1 0 4 : 9\n");
  TASK_CHECK(s == NULL);

  /* Interval gap: [0,2) and [3,4) leave byte 2 uncovered. */
  s = afl_struct_create(input, 4, "1 0 2 : -\n2 3 4 : -\n");
  TASK_CHECK(s == NULL);

  /* Interval overrun. */
  s = afl_struct_create(input, 4, "1 0 5 : -\n");
  TASK_CHECK(s == NULL);

  /* Duplicate ID. */
  s = afl_struct_create(input, 4, "1 0 2 : -\n1 2 4 : -\n");
  TASK_CHECK(s == NULL);

  /* Malformed line (missing colon). */
  s = afl_struct_create(input, 4, "1 0 2 3 4\n");
  TASK_CHECK(s == NULL);

  /* Negative-looking value. */
  s = afl_struct_create(input, 4, "1 -1 2 : -\n");
  TASK_CHECK(s == NULL);

  /* A dependency cycle over a correct partition is legal. */
  s = afl_struct_create(input, 4, "1 0 2 : 2\n2 2 4 : 1\n");
  TASK_CHECK(s != NULL);
  afl_struct_destroy(s);

  /* Empty data + empty table is valid. */
  s = afl_struct_create(NULL, 0, "");
  TASK_CHECK(s != NULL);
  afl_struct_destroy(s);

  /* Non-empty data with an empty table is not a partition. */
  s = afl_struct_create(input, 4, "");
  TASK_CHECK(s == NULL);

  return 0;
}

/* Zero budget distinguishes an empty graph from pending candidates. */
static int case_zero_budget(void) {
  static const unsigned char empty[] = {0};
  static const unsigned char input[] = {'A', 'B', 'A', 'B'};
  const char *table =
      "1 0 2 : -\n"
      "2 2 4 : -\n";
  afl_struct_session *empty_s;
  afl_struct_session *s;
  script_ctx ctx;
  const unsigned char *out;
  size_t out_len = 0;
  int rc;

  /* Empty data + empty table is a valid, already-complete session. */
  empty_s = afl_struct_create(empty, 0, "");
  TASK_CHECK(empty_s != NULL);
  if (!empty_s) return 1;
  out = afl_struct_data(empty_s, &out_len);
  TASK_CHECK(out_len == 0);
  memset(&ctx, 0, sizeof(ctx));
  rc = afl_struct_step(empty_s, 0, script_observe, &ctx);
  TASK_CHECK(rc == 0);
  TASK_CHECK(ctx.calls == 0);
  afl_struct_destroy(empty_s);

  /* A non-empty graph with zero budget must pause, not complete. */
  s = afl_struct_create(input, sizeof(input), table);
  TASK_CHECK(s != NULL);
  if (!s) return 1;

  memset(&ctx, 0, sizeof(ctx));
  rc = afl_struct_step(s, 0, script_observe, &ctx);
  TASK_CHECK(rc == 1);
  TASK_CHECK(ctx.calls == 0);

  out = afl_struct_data(s, &out_len);
  TASK_CHECK(out_len == 4 && memcmp(out, "ABAB", 4) == 0);

  afl_struct_destroy(s);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Group: cache_resume                                                */
/* ------------------------------------------------------------------ */

/* Different deletion sets that yield identical bytes reuse one decision. */

static int case_cache_identical_bytes(void) {
  static const unsigned char input[] = {'a', 'a', 'a', 'a'};
  const char *table =
      "1 0 1 : -\n"
      "2 1 2 : -\n"
      "3 2 3 : -\n"
      "4 3 4 : -\n";
  afl_struct_session *s;
  script_ctx ctx;
  int rc;

  memset(&ctx, 0, sizeof(ctx));
  ctx.max_calls = MAX_CANDS; /* all rejections */

  s = afl_struct_create(input, sizeof(input), table);
  TASK_CHECK(s != NULL);
  if (!s) return 1;

  rc = afl_struct_step(s, (size_t)-1, script_observe, &ctx);
  TASK_CHECK(rc == 0);

  /* Deleting any single fragment yields "aaa"; it must be executed once. */
  TASK_CHECK(ctx.calls == 1);
  TASK_CHECK(ctx.seen_len[0] == 3 && strcmp(ctx.seen[0], "aaa") == 0);

  afl_struct_destroy(s);
  return 0;
}

/* Equal-length but different bytes must not be conflated. */

static int case_cache_distinct_bytes(void) {
  static const unsigned char input[] = {'a', 'b', 'c', 'd'};
  const char *table =
      "1 0 1 : -\n"
      "2 1 2 : -\n"
      "3 2 3 : -\n"
      "4 3 4 : -\n";
  afl_struct_session *s;
  script_ctx ctx;
  int rc;

  memset(&ctx, 0, sizeof(ctx));
  ctx.max_calls = MAX_CANDS;

  s = afl_struct_create(input, sizeof(input), table);
  TASK_CHECK(s != NULL);
  if (!s) return 1;

  rc = afl_struct_step(s, (size_t)-1, script_observe, &ctx);
  TASK_CHECK(rc == 0);

  /* Four distinct three-byte candidates, all rejected. */
  TASK_CHECK(ctx.calls == 4);
  TASK_CHECK(ctx.seen_len[0] == 3 && strcmp(ctx.seen[0], "bcd") == 0);
  TASK_CHECK(ctx.seen_len[1] == 3 && strcmp(ctx.seen[1], "acd") == 0);
  TASK_CHECK(ctx.seen_len[2] == 3 && strcmp(ctx.seen[2], "abd") == 0);
  TASK_CHECK(ctx.seen_len[3] == 3 && strcmp(ctx.seen[3], "abc") == 0);

  afl_struct_destroy(s);
  return 0;
}

/* An execution error does not advance the cursor or populate the cache. */

static int case_callback_error_retry(void) {
  static const unsigned char input[] = {'w', 'x', 'y', 'z'};
  const char *table =
      "1 0 1 : -\n"
      "2 1 2 : -\n"
      "3 2 4 : -\n";
  afl_struct_session *s;
  script_ctx ctx;
  int rc;

  memset(&ctx, 0, sizeof(ctx));
  ctx.max_calls = 1;
  ctx.result[0] = -1; /* first attempt fails */

  s = afl_struct_create(input, sizeof(input), table);
  TASK_CHECK(s != NULL);
  if (!s) return 1;

  rc = afl_struct_step(s, 10, script_observe, &ctx);
  TASK_CHECK(rc == -1);
  TASK_CHECK(ctx.calls == 1);
  TASK_CHECK(ctx.seen_len[0] == 3 && strcmp(ctx.seen[0], "xyz") == 0);

  /* Retry from the same place; the failed candidate was not cached. */
  ctx.calls = 0;
  memset(ctx.seen, 0, sizeof(ctx.seen));
  memset(ctx.result, 0, sizeof(ctx.result));
  ctx.max_calls = MAX_CANDS;
  ctx.expect[0] = "xyz";
  ctx.expect[1] = "wyz";
  ctx.expect[2] = "wx";

  rc = afl_struct_step(s, 10, script_observe, &ctx);
  TASK_CHECK(rc == 0);
  TASK_CHECK(!ctx.mismatch);
  TASK_CHECK(ctx.calls == 3);
  TASK_CHECK(ctx.seen_len[0] == 3 && strcmp(ctx.seen[0], "xyz") == 0);

  afl_struct_destroy(s);
  return 0;
}

/* A saved session resumes to the same bytes with no repeated executions. */

static int case_state_roundtrip(void) {
  static const unsigned char input[] = {'A', 'A', 'B'};
  const char *table =
      "1 0 1 : -\n"
      "2 1 2 : -\n"
      "3 2 3 : -\n";
  const char *state = "task-state-roundtrip.tmp";
  afl_struct_session *s;
  afl_struct_session *loaded = NULL;
  script_ctx ctx;
  script_ctx full_ctx;
  const unsigned char *out;
  size_t out_len = 0;
  int rc;
  int calls_a, calls_full;
  char dir[4096];
  char path[4096];

  if (!getcwd(dir, sizeof(dir))) return 1;
  snprintf(path, sizeof(path), "%s/%s", dir, state);
  unlink(path);

  /* Single uninterrupted run for reference. */
  memset(&full_ctx, 0, sizeof(full_ctx));
  full_ctx.max_calls = MAX_CANDS;
  s = afl_struct_create(input, sizeof(input), table);
  TASK_CHECK(s != NULL);
  if (!s) return 1;
  rc = afl_struct_step(s, (size_t)-1, script_observe, &full_ctx);
  TASK_CHECK(rc == 0);
  calls_full = full_ctx.calls;
  afl_struct_destroy(s);

  /* Partial run with a one-execution budget, then checkpoint. */
  memset(&ctx, 0, sizeof(ctx));
  ctx.max_calls = MAX_CANDS;
  s = afl_struct_create(input, sizeof(input), table);
  TASK_CHECK(s != NULL);
  if (!s) return 1;

  rc = afl_struct_step(s, 1, script_observe, &ctx);
  TASK_CHECK(rc == 1);

  calls_a = ctx.calls;
  TASK_CHECK(afl_struct_state_save(s, path, (const unsigned char *)"m", 1) == 0);
  afl_struct_destroy(s);

  /* Resume in a fresh session. */
  {
    unsigned char meta[16];
    size_t meta_len = 0;
    rc = afl_struct_state_load(path, input, sizeof(input), table,
                               meta, sizeof(meta), &meta_len, &loaded);
    TASK_CHECK(rc == AFL_STRUCT_STATE_OK);
    TASK_CHECK(loaded != NULL);
    TASK_CHECK(meta_len == 1 && meta[0] == 'm');
  }

  if (!loaded) { unlink(path); return 1; }

  memset(&ctx, 0, sizeof(ctx));
  ctx.max_calls = MAX_CANDS;
  rc = afl_struct_step(loaded, (size_t)-1, script_observe, &ctx);
  TASK_CHECK(rc == 0);

  out = afl_struct_data(loaded, &out_len);
  TASK_CHECK(out_len == 3 && memcmp(out, "AAB", 3) == 0);

  /* No candidate was executed twice across the two processes. */
  TASK_CHECK(calls_a + ctx.calls == calls_full);

  afl_struct_destroy(loaded);
  unlink(path);
  return 0;
}

/* A saved state matches a reordered equivalent table (canonical identity). */

static int case_table_order_identity(void) {
  static const unsigned char input[] = {'a', 'b', 'c', 'd'};
  const char *table_a =
      "1 0 1 : -\n"
      "2 1 3 : 4,1\n"
      "3 3 4 : -\n"
      "4 2 2 : -\n";
  /* Same graph: line order changed, NEEDS order changed. */
  const char *table_b =
      "4 2 2 : -\n"
      "3 3 4 : -\n"
      "2 1 3 : 1,4\n"
      "1 0 1 : -\n";
  const char *state = "task-state-order.tmp";
  afl_struct_session *s;
  afl_struct_session *loaded = NULL;
  script_ctx ctx;
  size_t out_len = 0;
  char dir[4096], path[4096];
  int rc;

  if (!getcwd(dir, sizeof(dir))) return 1;
  snprintf(path, sizeof(path), "%s/%s", dir, state);
  unlink(path);

  memset(&ctx, 0, sizeof(ctx));
  ctx.max_calls = MAX_CANDS;

  s = afl_struct_create(input, sizeof(input), table_a);
  TASK_CHECK(s != NULL);
  if (!s) { return 1; }

  rc = afl_struct_step(s, 1, script_observe, &ctx);
  TASK_CHECK(rc == 1);
  TASK_CHECK(afl_struct_state_save(s, path, (const unsigned char *)"x", 1) == 0);
  afl_struct_destroy(s);

  {
    unsigned char meta[8];
    size_t meta_len = 0;
    rc = afl_struct_state_load(path, input, sizeof(input), table_b,
                               meta, sizeof(meta), &meta_len, &loaded);
    TASK_CHECK(rc == AFL_STRUCT_STATE_OK);
    TASK_CHECK(loaded != NULL);
  }

  if (loaded) {
    const unsigned char *out;
    afl_struct_destroy(loaded);
    (void)out;
  }

  unlink(path);

  /* A truly different graph must not match. */
  {
    const char *table_c =
        "1 0 1 : -\n"
        "2 1 3 : 4\n"
        "3 3 4 : -\n"
        "4 2 2 : -\n";
    memset(&ctx, 0, sizeof(ctx));
    ctx.max_calls = MAX_CANDS;
    s = afl_struct_create(input, sizeof(input), table_a);
    TASK_CHECK(s != NULL);
    if (!s) return 1;
    rc = afl_struct_step(s, 1, script_observe, &ctx);
    TASK_CHECK(rc == 1);
    TASK_CHECK(afl_struct_state_save(s, path, (const unsigned char *)"x", 1) == 0);
    afl_struct_destroy(s);

    loaded = NULL;
    rc = afl_struct_state_load(path, input, sizeof(input), table_c, NULL, 0,
                               NULL, &loaded);
    TASK_CHECK(rc == AFL_STRUCT_STATE_MISMATCH);
    TASK_CHECK(loaded == NULL);
    unlink(path);
  }

  (void)out_len;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Group: tmin_entry - real afl-tmin processes                        */
/* ------------------------------------------------------------------ */

/* CLI cases use the same registered check macro as everything else. */

#define CLI_CHECK(x) TASK_CHECK(x)

static char g_root[4096];

static int make_tmpdir(char *out, size_t cap) {
  const char *base = getenv("TMPDIR");
  char tmpl[4096];
  if (!base || !*base) base = "/tmp";
  snprintf(tmpl, sizeof(tmpl), "%s/afl-struct-test-XXXXXX", base);
  if (!mkdtemp(tmpl)) return -1;
  snprintf(out, cap, "%s", tmpl);
  return 0;
}

static int write_bytes(const char *path, const void *data, size_t len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  size_t off = 0;
  if (fd < 0) return -1;
  while (off < len) {
    ssize_t n = write(fd, (const char *)data + off, len - off);
    if (n < 0) { if (errno == EINTR) continue; close(fd); return -1; }
    if (n == 0) break;
    off += (size_t)n;
  }
  close(fd);
  return off == len ? 0 : -1;
}

static int read_all(const char *path, char *buf, size_t cap, size_t *out_len) {
  int fd = open(path, O_RDONLY);
  size_t off = 0;
  if (fd < 0) return -1;
  for (;;) {
    ssize_t n = read(fd, buf + off, cap - off - 1);
    if (n < 0) { if (errno == EINTR) continue; close(fd); return -1; }
    if (n == 0) break;
    off += (size_t)n;
    if (off + 1 >= cap) break;
  }
  close(fd);
  buf[off] = 0;
  if (out_len) *out_len = off;
  return 0;
}

static int run_process(char *const argv[], const char *logpath) {
  int fd;
  pid_t pid;
  int status = 0;

  fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;

  pid = fork();
  if (pid < 0) { close(fd); return -1; }

  if (pid == 0) {
    if (dup2(fd, 1) < 0 || dup2(fd, 2) < 0) _exit(127);
    close(fd);
    execv(argv[0], argv);
    _exit(127);
  }

  close(fd);
  if (waitpid(pid, &status, 0) < 0) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 128 + WTERMSIG(status);
}

static int log_contains(const char *path, const char *needle) {
  char buf[65536];
  size_t len;
  if (read_all(path, buf, sizeof(buf), &len)) return 0;
  return strstr(buf, needle) != NULL;
}

static void setup_root(void) {
  if (!g_root[0]) {
    if (!getcwd(g_root, sizeof(g_root))) g_root[0] = 0;
  }
}

static int write_table(const char *dir) {
  char path[4096];
  const char *table =
      "1 0 1 : -\n"
      "2 1 5 : -\n"
      "3 5 6 : -\n"
      "4 6 7 : -\n";
  snprintf(path, sizeof(path), "%s/table.txt", dir);
  return write_bytes(path, table, strlen(table));
}

static int stdin_replay_impl(int use_file_mode) {
  char dir[4096], inpath[4096], outpath[4096], logpath[4096];
  char tool[4096], target[4096], prog_in[4096];
  char outbuf[64];
  size_t outlen = 0;
  const char *input = "AxxxxB\n";
  int rc;

  setup_root();
  CLI_CHECK(g_root[0] != 0);
  CLI_CHECK(make_tmpdir(dir, sizeof(dir)) == 0);

  snprintf(inpath, sizeof(inpath), "%s/in.bin", dir);
  snprintf(outpath, sizeof(outpath), "%s/out.bin", dir);
  snprintf(logpath, sizeof(logpath), "%s/log.txt", dir);
  snprintf(prog_in, sizeof(prog_in), "%s/prog.in", dir);
  snprintf(tool, sizeof(tool), "%s/.task-build/afl-tmin", g_root);
  snprintf(target, sizeof(target), "%s/.task-build/benign-target", g_root);

  CLI_CHECK(write_bytes(inpath, input, strlen(input)) == 0);
  CLI_CHECK(write_table(dir) == 0);

  if (use_file_mode) {
    char *argv[16];
    char table_abs[4096];
    int n = 0;
    snprintf(table_abs, sizeof(table_abs), "%s/table.txt", dir);
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table_abs;
    argv[n++] = "-f"; argv[n++] = prog_in;
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = outpath;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = "-t"; argv[n++] = (char *)"1000";
    argv[n++] = (char *)"--";
    argv[n++] = target;
    argv[n++] = (char *)"@@";
    argv[n] = NULL;
    rc = run_process(argv, logpath);
  } else {
    char *argv[16];
    int n = 0;
    char table_abs[4096];
    snprintf(table_abs, sizeof(table_abs), "%s/table.txt", dir);
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table_abs;
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = outpath;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = "-t"; argv[n++] = (char *)"1000";
    argv[n++] = (char *)"--";
    argv[n++] = target;
    argv[n] = NULL;
    rc = run_process(argv, logpath);
  }

  CLI_CHECK(rc == 0);
  CLI_CHECK(log_contains(logpath, "STRUCT complete"));
  CLI_CHECK(read_all(outpath, outbuf, sizeof(outbuf), &outlen) == 0);
  CLI_CHECK(outlen == 2);
  CLI_CHECK(outbuf[0] == 'A' && outbuf[1] == 'B');

  return 0;
}

static int case_tmin_stdin_replay(void) { return stdin_replay_impl(0); }
static int case_tmin_file_replay(void) { return stdin_replay_impl(1); }

static int case_tmin_checkpoint_resume(void) {
  char dir[4096], inpath[4096], state[4096], out1[4096], out2[4096];
  char out3[4096], log1[4096], log2[4096], log3[4096];
  char tool[4096], target[4096], table[4096];
  char b1[64], b2[64], b3[64];
  size_t l1 = 0, l2 = 0, l3 = 0;
  const char *input = "AxxxxB\n";
  int rc;

  setup_root();
  CLI_CHECK(g_root[0] != 0);
  CLI_CHECK(make_tmpdir(dir, sizeof(dir)) == 0);

  snprintf(inpath, sizeof(inpath), "%s/in.bin", dir);
  snprintf(state, sizeof(state), "%s/session.state", dir);
  snprintf(out1, sizeof(out1), "%s/out1.bin", dir);
  snprintf(out2, sizeof(out2), "%s/out2.bin", dir);
  snprintf(out3, sizeof(out3), "%s/out3.bin", dir);
  snprintf(log1, sizeof(log1), "%s/log1.txt", dir);
  snprintf(log2, sizeof(log2), "%s/log2.txt", dir);
  snprintf(log3, sizeof(log3), "%s/log3.txt", dir);
  snprintf(tool, sizeof(tool), "%s/.task-build/afl-tmin", g_root);
  snprintf(target, sizeof(target), "%s/.task-build/benign-target", g_root);
  snprintf(table, sizeof(table), "%s/table.txt", dir);

  CLI_CHECK(write_bytes(inpath, input, strlen(input)) == 0);
  CLI_CHECK(write_table(dir) == 0);

  /* First process: one execution, then pause and checkpoint. */
  {
    char *argv[18];
    int n = 0;
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table;
    argv[n++] = "-N"; argv[n++] = (char *)"1";
    argv[n++] = "-C"; argv[n++] = state;
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = out1;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = "-t"; argv[n++] = (char *)"1000";
    argv[n++] = (char *)"--"; argv[n++] = target;
    argv[n] = NULL;
    rc = run_process(argv, log1);
  }

  CLI_CHECK(rc == 0);
  CLI_CHECK(log_contains(log1, "STRUCT paused"));
  CLI_CHECK(read_all(out1, b1, sizeof(b1), &l1) == 0);
  /* Nothing accepted yet: the paused output is the original input. */
  CLI_CHECK(l1 == strlen(input) && memcmp(b1, input, l1) == 0);

  /* Second process: resume without a budget and finish. */
  {
    char *argv[18];
    int n = 0;
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table;
    argv[n++] = "-C"; argv[n++] = state;
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = out2;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = "-t"; argv[n++] = (char *)"1000";
    argv[n++] = (char *)"--"; argv[n++] = target;
    argv[n] = NULL;
    rc = run_process(argv, log2);
  }

  CLI_CHECK(rc == 0);
  CLI_CHECK(log_contains(log2, "STRUCT complete"));
  CLI_CHECK(read_all(out2, b2, sizeof(b2), &l2) == 0);

  /* Single uninterrupted run for comparison. */
  {
    char *argv[18];
    int n = 0;
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table;
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = out3;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = "-t"; argv[n++] = (char *)"1000";
    argv[n++] = (char *)"--"; argv[n++] = target;
    argv[n] = NULL;
    rc = run_process(argv, log3);
  }

  CLI_CHECK(rc == 0);
  CLI_CHECK(read_all(out3, b3, sizeof(b3), &l3) == 0);

  CLI_CHECK(l2 == l3 && memcmp(b2, b3, l2) == 0);
  CLI_CHECK(l2 == 2 && b2[0] == 'A' && b2[1] == 'B');

  return 0;
}

static int case_tmin_reject_options(void) {
  char dir[4096], inpath[4096], outpath[4096], logpath[4096];
  char tool[4096], target[4096], table[4096];
  int rc;

  setup_root();
  CLI_CHECK(g_root[0] != 0);
  CLI_CHECK(make_tmpdir(dir, sizeof(dir)) == 0);

  snprintf(inpath, sizeof(inpath), "%s/in.bin", dir);
  snprintf(outpath, sizeof(outpath), "%s/out.bin", dir);
  snprintf(logpath, sizeof(logpath), "%s/log.txt", dir);
  snprintf(tool, sizeof(tool), "%s/.task-build/afl-tmin", g_root);
  snprintf(target, sizeof(target), "%s/.task-build/benign-target", g_root);
  snprintf(table, sizeof(table), "%s/table.txt", dir);

  CLI_CHECK(write_bytes(inpath, "AxxxxB\n", 7) == 0);
  CLI_CHECK(write_table(dir) == 0);

  /* -x is rejected in structural mode. */
  {
    char *argv[18];
    int n = 0;
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table;
    argv[n++] = "-x";
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = outpath;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = (char *)"--"; argv[n++] = target;
    argv[n] = NULL;
    rc = run_process(argv, logpath);
  }
  CLI_CHECK(rc != 0);
  CLI_CHECK(log_contains(logpath, "-Q and -x"));

  /* -Q is rejected in structural mode (before any QEMU lookup). */
  {
    char *argv[18];
    int n = 0;
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table;
    argv[n++] = "-Q";
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = outpath;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = (char *)"--"; argv[n++] = target;
    argv[n] = NULL;
    rc = run_process(argv, logpath);
  }
  CLI_CHECK(rc != 0);
  CLI_CHECK(log_contains(logpath, "-Q and -x"));

  return 0;
}

static int case_tmin_reject_abnormal_start(void) {
  char dir[4096], inpath[4096], outpath[4096], logpath[4096];
  char tool[4096], script[4096], table[4096];
  int rc;

  setup_root();
  CLI_CHECK(g_root[0] != 0);
  CLI_CHECK(make_tmpdir(dir, sizeof(dir)) == 0);

  snprintf(inpath, sizeof(inpath), "%s/in.bin", dir);
  snprintf(outpath, sizeof(outpath), "%s/out.bin", dir);
  snprintf(logpath, sizeof(logpath), "%s/log.txt", dir);
  snprintf(tool, sizeof(tool), "%s/.task-build/afl-tmin", g_root);
  snprintf(script, sizeof(script), "%s/abnormal-target", dir);
  snprintf(table, sizeof(table), "%s/table.txt", dir);

  CLI_CHECK(write_bytes(inpath, "AxxxxB\n", 7) == 0);
  CLI_CHECK(write_table(dir) == 0);
  CLI_CHECK(write_bytes(script, "#!/bin/sh\nexit 3\n", 18) == 0);
  CLI_CHECK(chmod(script, 0755) == 0);

  {
    char *argv[16];
    int n = 0;
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table;
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = outpath;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = "-t"; argv[n++] = (char *)"1000";
    argv[n++] = (char *)"--"; argv[n++] = script;
    argv[n] = NULL;
    rc = run_process(argv, logpath);
  }

  CLI_CHECK(rc != 0);
  CLI_CHECK(log_contains(logpath, "terminates normally"));

  return 0;
}

static int case_tmin_state_identity_mismatch(void) {
  char dir[4096], inpath[4096], out1[4096], out2[4096], state[4096];
  char log1[4096], log2[4096], tool[4096], target[4096], table[4096];
  int rc;

  setup_root();
  CLI_CHECK(g_root[0] != 0);
  CLI_CHECK(make_tmpdir(dir, sizeof(dir)) == 0);

  snprintf(inpath, sizeof(inpath), "%s/in.bin", dir);
  snprintf(out1, sizeof(out1), "%s/out1.bin", dir);
  snprintf(out2, sizeof(out2), "%s/out2.bin", dir);
  snprintf(state, sizeof(state), "%s/session.state", dir);
  snprintf(log1, sizeof(log1), "%s/log1.txt", dir);
  snprintf(log2, sizeof(log2), "%s/log2.txt", dir);
  snprintf(tool, sizeof(tool), "%s/.task-build/afl-tmin", g_root);
  snprintf(target, sizeof(target), "%s/.task-build/benign-target", g_root);
  snprintf(table, sizeof(table), "%s/table.txt", dir);

  CLI_CHECK(write_bytes(inpath, "AxxxxB\n", 7) == 0);
  CLI_CHECK(write_table(dir) == 0);

  {
    char *argv[18];
    int n = 0;
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table;
    argv[n++] = "-N"; argv[n++] = (char *)"1";
    argv[n++] = "-C"; argv[n++] = state;
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = out1;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = "-t"; argv[n++] = (char *)"1000";
    argv[n++] = (char *)"--"; argv[n++] = target;
    argv[n] = NULL;
    rc = run_process(argv, log1);
  }
  CLI_CHECK(rc == 0);

  /* Resume with a different timeout: the stored identity no longer matches. */
  {
    char *argv[18];
    int n = 0;
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table;
    argv[n++] = "-C"; argv[n++] = state;
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = out2;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = "-t"; argv[n++] = (char *)"2000";
    argv[n++] = (char *)"--"; argv[n++] = target;
    argv[n] = NULL;
    rc = run_process(argv, log2);
  }
  CLI_CHECK(rc != 0);
  CLI_CHECK(log_contains(log2, "different target"));
  CLI_CHECK(access(out2, F_OK) != 0); /* -o must not be created */

  return 0;
}

static int case_tmin_state_corrupt(void) {
  char dir[4096], inpath[4096], out1[4096], out2[4096], state[4096];
  char log1[4096], log2[4096], tool[4096], target[4096], table[4096];
  char before[8192], after[8192];
  size_t before_len = 0, after_len = 0;
  int rc;

  setup_root();
  CLI_CHECK(g_root[0] != 0);
  CLI_CHECK(make_tmpdir(dir, sizeof(dir)) == 0);

  snprintf(inpath, sizeof(inpath), "%s/in.bin", dir);
  snprintf(out1, sizeof(out1), "%s/out1.bin", dir);
  snprintf(out2, sizeof(out2), "%s/out2.bin", dir);
  snprintf(state, sizeof(state), "%s/session.state", dir);
  snprintf(log1, sizeof(log1), "%s/log1.txt", dir);
  snprintf(log2, sizeof(log2), "%s/log2.txt", dir);
  snprintf(tool, sizeof(tool), "%s/.task-build/afl-tmin", g_root);
  snprintf(target, sizeof(target), "%s/.task-build/benign-target", g_root);
  snprintf(table, sizeof(table), "%s/table.txt", dir);

  CLI_CHECK(write_bytes(inpath, "AxxxxB\n", 7) == 0);
  CLI_CHECK(write_table(dir) == 0);

  {
    char *argv[18];
    int n = 0;
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table;
    argv[n++] = "-N"; argv[n++] = (char *)"1";
    argv[n++] = "-C"; argv[n++] = state;
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = out1;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = "-t"; argv[n++] = (char *)"1000";
    argv[n++] = (char *)"--"; argv[n++] = target;
    argv[n] = NULL;
    rc = run_process(argv, log1);
  }
  CLI_CHECK(rc == 0);

  /* Corrupt one hex digit of the data record, leaving the checksum stale. */
  CLI_CHECK(read_all(state, before, sizeof(before), &before_len) == 0);
  {
    char *pos = strchr(before, '\n');
    CLI_CHECK(pos != NULL);
    ++pos; /* start of the data line */
    pos = strstr(pos, "data 7 ");
    CLI_CHECK(pos != NULL);
    pos += 7;
    CLI_CHECK(*pos != 0);
    *pos = (*pos == '0') ? '1' : '0';
  }
  CLI_CHECK(write_bytes(state, before, before_len) == 0);

  {
    char *argv[18];
    int n = 0;
    argv[n++] = tool;
    argv[n++] = "-S"; argv[n++] = table;
    argv[n++] = "-C"; argv[n++] = state;
    argv[n++] = "-i"; argv[n++] = inpath;
    argv[n++] = "-o"; argv[n++] = out2;
    argv[n++] = "-m"; argv[n++] = (char *)"none";
    argv[n++] = "-t"; argv[n++] = (char *)"1000";
    argv[n++] = (char *)"--"; argv[n++] = target;
    argv[n] = NULL;
    rc = run_process(argv, log2);
  }

  CLI_CHECK(rc != 0);
  CLI_CHECK(log_contains(log2, "corrupt"));
  CLI_CHECK(access(out2, F_OK) != 0);

  /* The failed run must not overwrite the old state file. */
  CLI_CHECK(read_all(state, after, sizeof(after), &after_len) == 0);
  CLI_CHECK(after_len == before_len && memcmp(before, after, before_len) == 0);

  return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"dependency_closure", "cycle_cross_interval", case_dep_cycle_closure_real},
    {"dependency_closure", "same_bytes_distinct_identity",
     case_same_bytes_distinct_identity},
    {"dependency_closure", "accept_rescan", case_accept_rescan},
    {"dependency_closure", "table_validation", case_table_validation},
    {"dependency_closure", "zero_budget", case_zero_budget},

    {"cache_resume", "identical_bytes_reuse", case_cache_identical_bytes},
    {"cache_resume", "distinct_bytes_separate", case_cache_distinct_bytes},
    {"cache_resume", "error_callback_retry", case_callback_error_retry},
    {"cache_resume", "state_roundtrip", case_state_roundtrip},
    {"cache_resume", "table_order_identity", case_table_order_identity},

    {"tmin_entry", "stdin_replay", case_tmin_stdin_replay},
    {"tmin_entry", "file_replay", case_tmin_file_replay},
    {"tmin_entry", "checkpoint_resume", case_tmin_checkpoint_resume},
    {"tmin_entry", "reject_options", case_tmin_reject_options},
    {"tmin_entry", "reject_abnormal_start", case_tmin_reject_abnormal_start},
    {"tmin_entry", "state_identity_mismatch", case_tmin_state_identity_mismatch},
    {"tmin_entry", "state_corrupt", case_tmin_state_corrupt},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
