/*
 * Content assertions for the isolated replay-batch entry point.
 *
 * Groups: isolation, cancel, artifacts.  Every case runs real (benign,
 * finite) DeepState tests through `DeepState_ReplayBatch` and verifies saved
 * bytes and the published manifest, not only counters.  Cross-process stages
 * are synchronized through pipes/acks, never with sleeps.
 */

#include <deepstate/Replay.h>

#include "../task_support/check.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- small helpers ------------------------------------------------------ */

static int fd_write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *) buf;
  size_t off = 0;
  while (off < len) {
    ssize_t k = write(fd, p + off, len - off);
    if (k < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (k == 0) {
      return -1;
    }
    off += (size_t) k;
  }
  return 0;
}

static int fd_read_one(int fd) {
  char c;
  for (;;) {
    ssize_t k = read(fd, &c, 1);
    if (k == 1) {
      return (unsigned char) c;
    }
    if (k < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
}

static int mkroot(char *root, size_t cap) {
  const char *base = getenv("TMPDIR");
  if (base == NULL || base[0] == '\0') {
    base = "/tmp";
  }
  int n = snprintf(root, cap, "%s/deepstate-replay-check-XXXXXX", base);
  if (n < 0 || (size_t) n >= cap) {
    return -1;
  }
  return mkdtemp(root) != NULL ? 0 : -1;
}

static int join_path(char *out, size_t cap, const char *dir, const char *name) {
  int n = snprintf(out, cap, "%s/%s", dir, name);
  return (n < 0 || (size_t) n >= cap) ? -1 : 0;
}

static int path_exists(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0;
}

static void rm_rf(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    return;
  }
  if (S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    if (dir != NULL) {
      struct dirent *dp;
      while ((dp = readdir(dir)) != NULL) {
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0) {
          continue;
        }
        char child[PATH_MAX];
        if (join_path(child, sizeof(child), path, dp->d_name) == 0) {
          rm_rf(child);
        }
      }
      closedir(dir);
    }
    rmdir(path);
  } else {
    unlink(path);
  }
}

static ssize_t read_file_buf(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  size_t off = 0;
  for (;;) {
    if (off == cap) {
      break;
    }
    ssize_t k = read(fd, buf + off, cap - off);
    if (k < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(fd);
      return -1;
    }
    if (k == 0) {
      break;
    }
    off += (size_t) k;
  }
  close(fd);
  return (ssize_t) off;
}

static int file_equals(const char *path, const void *expect, size_t len) {
  char buf[8192];
  if (len > sizeof(buf)) {
    return 0;
  }
  ssize_t got = read_file_buf(path, buf, sizeof(buf));
  return got >= 0 && (size_t) got == len && memcmp(buf, expect, len) == 0;
}

static int file_contains(const char *path, const char *needle) {
  char buf[65536];
  ssize_t got = read_file_buf(path, buf, sizeof(buf) - 1);
  if (got < 0) {
    return 0;
  }
  buf[got] = '\0';
  return strstr(buf, needle) != NULL;
}

static int file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return -1;
  }
  return (int) st.st_size;
}

/* Number of entries directly under `dir` whose name looks like our staging
 * directories.  Used to assert nothing is leaked. */
static int count_staging(const char *dir) {
  DIR *d = opendir(dir);
  if (d == NULL) {
    return -1;
  }
  int n = 0;
  struct dirent *dp;
  while ((dp = readdir(d)) != NULL) {
    if (strstr(dp->d_name, ".replay-staging-") != NULL) {
      ++n;
    }
  }
  closedir(d);
  return n;
}

static int count_open_fds(void) {
  int n = 0;
  for (int fd = 0; fd < 256; ++fd) {
    if (fcntl(fd, F_GETFD) >= 0) {
      ++n;
    }
  }
  return n;
}

/* Silence the framework's own Trace/Info logs in children so byte-level log
 * assertions are exact.  The caller restores the previous level. */
static int g_saved_minlog;
static void quiet_logs(void) {
  g_saved_minlog = FLAGS_min_log_level;
  FLAGS_min_log_level = 6; /* Fatal */
}
static void restore_logs(void) {
  FLAGS_min_log_level = g_saved_minlog;
}

static void set_case(struct DeepState_ReplayCase *c, uint64_t id,
                     struct DeepState_TestInfo *test, const void *data,
                     size_t size) {
  memset(c, 0, sizeof(*c));
  c->id = id;
  c->test = test;
  c->data = (const uint8_t *) data;
  c->size = size;
}

static void set_cfg(struct DeepState_ReplayConfig *cfg, uint64_t run,
                    unsigned jobs, size_t log_limit, int cancel_fd,
                    const char *output_dir) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->run_id = run;
  cfg->jobs = jobs;
  cfg->log_limit = log_limit;
  cfg->cancel_fd = cancel_fd;
  cfg->output_dir = output_dir;
}

/* ---- benign test functions --------------------------------------------- */

static void t_pass1(void) {
  if (DeepState_UChar() != 0x11) {
    DeepState_Fail();
  }
  DeepState_Pass();
}

static void t_pass2(void) {
  if (DeepState_UChar() != 0x22) {
    DeepState_Fail();
  }
  if (DeepState_UChar() != 0x33) {
    DeepState_Fail();
  }
  DeepState_Pass();
}

static void t_pass3(void) {
  if (DeepState_UChar() != 0x44 || DeepState_UChar() != 0x55 ||
      DeepState_UChar() != 0x66) {
    DeepState_Fail();
  }
  DeepState_Pass();
}

static void t_pass0(void) {
  /* Consumes no input at all. */
  DeepState_Pass();
}

static void t_abandon1(void) {
  (void) DeepState_UChar();
  DeepState_Abandon("normal unsatisfied assumption");
}

static void t_abandon2(void) {
  (void) DeepState_UChar();
  (void) DeepState_UChar();
  DeepState_Abandon("normal abandon after two bytes");
}

static void t_fail1(void) {
  (void) DeepState_UChar();
  DeepState_Fail();
}

/* Streams a token into the Information-level stream, flushes it, then passes.
 * If inherited partial stream content were not reset, the parent's token
 * would appear in front of ours. */
static void t_stream_token(void) {
  DeepState_StreamCStr(DeepState_LogInfo, "CHILD_STREAM_TOKEN");
  DeepState_LogStream(DeepState_LogInfo);
  (void) DeepState_UChar();
  DeepState_Pass();
}

/* Writes exactly 200000 raw bytes to stdout (the child's merged log pipe). */
static void t_biglog(void) {
  uint8_t buf[8192];
  for (size_t i = 0; i < sizeof(buf); ++i) {
    buf[i] = (uint8_t) (i * 7u + 3u);
  }
  size_t remaining = 200000;
  size_t off = 0;
  while (remaining > 0) {
    size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
    if (fd_write_all(STDOUT_FILENO, buf + (off % sizeof(buf)), chunk) != 0) {
      DeepState_Fail();
    }
    off += chunk;
    remaining -= chunk;
  }
  (void) DeepState_UChar();
  DeepState_Pass();
}

/* Writes exactly 10 known raw bytes. */
static void t_smalllog(void) {
  static const char s[] = "0123456789";
  if (fd_write_all(STDOUT_FILENO, s, 10) != 0) {
    DeepState_Fail();
  }
  (void) DeepState_UChar();
  DeepState_Pass();
}

/* Gated runner: announces it started, then blocks until released. */
static int g_ack_w = -1;
static int g_release_r = -1;

static void t_gated(void) {
  char tag = 'A';
  if (g_ack_w >= 0) {
    (void) fd_write_all(g_ack_w, &tag, 1);
  }
  char c;
  if (g_release_r >= 0) {
    while (read(g_release_r, &c, 1) < 0 && errno == EINTR) {
    }
  }
  (void) DeepState_UChar();
  DeepState_Pass();
}

/* Fast runner that records that it started; must never run when queued and
 * canceled. */
static void t_marked(void) {
  static const char s[] = "GATED_MARKED_STARTED\n";
  (void) fd_write_all(STDOUT_FILENO, s, sizeof(s) - 1);
  (void) DeepState_UChar();
  DeepState_Pass();
}

/* ---- controller harness ------------------------------------------------- */

struct harness {
  int ack_w;
  int ack_r;
  int rel_w;
  int rel_r;
  int ctl_w;
  int ctl_r;
  pid_t controller;
};

typedef void (*controller_fn)(int ack_r, int rel_w, int ctl_w);

static uint64_t g_ctrl_run;
static uint64_t g_ctrl_cancel_id;
static uint64_t g_ctrl_normal_id;

static int harness_open(struct harness *h) {
  int ack[2], rel[2], ctl[2];
  memset(h, 0, sizeof(*h));
  h->ack_w = h->ack_r = h->rel_w = h->rel_r = h->ctl_w = h->ctl_r = -1;
  if (pipe(ack) != 0 || pipe(rel) != 0 || pipe(ctl) != 0) {
    return -1;
  }
  h->ack_r = ack[0];
  h->ack_w = ack[1];
  h->rel_r = rel[0];
  h->rel_w = rel[1];
  h->ctl_r = ctl[0];
  h->ctl_w = ctl[1];
  g_ack_w = h->ack_w;
  g_release_r = h->rel_r;
  return 0;
}

static int harness_fork(struct harness *h, controller_fn fn) {
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    close(h->ack_w);
    close(h->rel_r);
    close(h->ctl_r);
    fn(h->ack_r, h->rel_w, h->ctl_w);
    _exit(0);
  }
  close(h->ack_r);
  h->ack_r = -1;
  close(h->rel_w);
  h->rel_w = -1;
  close(h->ctl_w);
  h->ctl_w = -1;
  h->controller = pid;
  return 0;
}

static int harness_close(struct harness *h) {
  g_ack_w = -1;
  g_release_r = -1;
  if (h->ack_w >= 0) {
    close(h->ack_w);
    h->ack_w = -1;
  }
  if (h->rel_r >= 0) {
    close(h->rel_r);
    h->rel_r = -1;
  }
  if (h->ctl_r >= 0) {
    close(h->ctl_r);
    h->ctl_r = -1;
  }
  int rc = 0;
  if (h->controller > 0) {
    int status = 0;
    while (waitpid(h->controller, &status, 0) < 0) {
      if (errno != EINTR) {
        return 1;
      }
    }
    h->controller = 0;
    rc = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
  }
  return rc;
}

static void write_pair(int fd, uint64_t a, uint64_t b) {
  uint64_t rec[2];
  rec[0] = a;
  rec[1] = b;
  (void) fd_write_all(fd, rec, sizeof(rec));
}

static void ctrl_running(int ack_r, int rel_w, int ctl_w) {
  (void) rel_w;
  (void) fd_read_one(ack_r);
  write_pair(ctl_w, g_ctrl_run, g_ctrl_cancel_id);
}

static void ctrl_queued(int ack_r, int rel_w, int ctl_w) {
  (void) fd_read_one(ack_r);
  write_pair(ctl_w, g_ctrl_run, g_ctrl_cancel_id);
  char g = 'g';
  (void) fd_write_all(rel_w, &g, 1);
}

static void ctrl_completion(int ack_r, int rel_w, int ctl_w) {
  (void) fd_read_one(ack_r);
  write_pair(ctl_w, g_ctrl_run, g_ctrl_cancel_id);
  char g = 'g';
  (void) fd_write_all(rel_w, &g, 1);
}

static void ctrl_split_stale(int ack_r, int rel_w, int ctl_w) {
  (void) rel_w;
  (void) fd_read_one(ack_r);

  uint64_t stale = g_ctrl_run + 12345;
  uint64_t rec[2];

  /* A stale-run cancel for the queued normal case, split mid-record. */
  rec[0] = stale;
  rec[1] = g_ctrl_normal_id;
  (void) fd_write_all(ctl_w, rec, 8);
  sched_yield();
  (void) fd_write_all(ctl_w, (const uint8_t *) rec + 8, 8);

  /* A complete stale-run cancel for the running gated case. */
  write_pair(ctl_w, stale, g_ctrl_cancel_id);

  /* A command for an unknown case id. */
  write_pair(ctl_w, g_ctrl_run, 0xDEADBEEFu);

  /* The only command that must take effect. */
  write_pair(ctl_w, g_ctrl_run, g_ctrl_cancel_id);
}

/* Release the single gated child once it has announced itself. */
static void ctrl_release_one(int ack_r, int rel_w, int ctl_w) {
  (void) ctl_w;
  (void) fd_read_one(ack_r);
  char g = 'g';
  (void) fd_write_all(rel_w, &g, 1);
}

/* Wait for both parallel gated children to announce themselves, then release
 * both, so neither sees a closed ack pipe (SIGPIPE) nor an early EOF. */
static void ctrl_release_two(int ack_r, int rel_w, int ctl_w) {
  (void) ctl_w;
  (void) fd_read_one(ack_r);
  (void) fd_read_one(ack_r);
  char g[2] = {'g', 'g'};
  (void) fd_write_all(rel_w, g, 2);
}

/* ---- shared fixtures ---------------------------------------------------- */

static struct DeepState_TestInfo test_pass1 = {
    NULL, t_pass1, "Pass1", __FILE__, 1};
static struct DeepState_TestInfo test_pass2 = {
    NULL, t_pass2, "Pass2", __FILE__, 2};
static struct DeepState_TestInfo test_pass3 = {
    NULL, t_pass3, "Pass3", __FILE__, 3};
static struct DeepState_TestInfo test_pass0 = {
    NULL, t_pass0, "Pass0", __FILE__, 4};
static struct DeepState_TestInfo test_abandon1 = {
    NULL, t_abandon1, "Abandon1", __FILE__, 5};
static struct DeepState_TestInfo test_abandon2 = {
    NULL, t_abandon2, "Abandon2", __FILE__, 6};
static struct DeepState_TestInfo test_fail1 = {
    NULL, t_fail1, "Fail1", __FILE__, 7};
static struct DeepState_TestInfo test_stream = {
    NULL, t_stream_token, "StreamToken", __FILE__, 8};
static struct DeepState_TestInfo test_biglog = {
    NULL, t_biglog, "BigLog", __FILE__, 9};
static struct DeepState_TestInfo test_smalllog = {
    NULL, t_smalllog, "SmallLog", __FILE__, 10};
static struct DeepState_TestInfo test_gated = {
    NULL, t_gated, "Gated", __FILE__, 11};
static struct DeepState_TestInfo test_marked = {
    NULL, t_marked, "Marked", __FILE__, 12};

static const uint8_t in1[] = {0x11};
static const uint8_t in2[] = {0x22, 0x33};
static const uint8_t in3[] = {0x44, 0x55, 0x66};

/* ======================================================================== */
/* isolation group                                                          */
/* ======================================================================== */

static int case_different_consumption(void) {
  char root[PATH_MAX], out[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  uint8_t snap1[1], snap2[2], snap3[3];
  memcpy(snap1, in1, sizeof(in1));
  memcpy(snap2, in2, sizeof(in2));
  memcpy(snap3, in3, sizeof(in3));

  struct DeepState_ReplayCase cases[3];
  set_case(&cases[0], 11, &test_pass1, in1, sizeof(in1));
  set_case(&cases[1], 22, &test_pass2, in2, sizeof(in2));
  set_case(&cases[2], 33, &test_pass3, in3, sizeof(in3));

  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 501, 2, 4096, -1, out);
  struct DeepState_ReplayResult results[3];
  memset(results, 0x5a, sizeof(results));

  int rc = DeepState_ReplayBatch(cases, 3, &cfg, results);
  TASK_CHECK(rc == 0);

  TASK_CHECK(results[0].run_id == 501 && results[0].id == 11);
  TASK_CHECK(results[1].id == 22 && results[2].id == 33);
  TASK_CHECK(results[0].consumed == 1);
  TASK_CHECK(results[1].consumed == 2);
  TASK_CHECK(results[2].consumed == 3);
  TASK_CHECK(results[0].state == DeepState_ReplayComplete);
  TASK_CHECK(results[0].result == DeepState_TestRunPass);

  char path[PATH_MAX];
  TASK_CHECK(join_path(path, sizeof(path), out, "11.pass") == 0);
  TASK_CHECK(file_equals(path, in1, 1));
  TASK_CHECK(join_path(path, sizeof(path), out, "22.pass") == 0);
  TASK_CHECK(file_equals(path, in2, 2));
  TASK_CHECK(join_path(path, sizeof(path), out, "33.pass") == 0);
  TASK_CHECK(file_equals(path, in3, 3));

  /* Inputs must be untouched. */
  TASK_CHECK(memcmp(in1, snap1, 1) == 0);
  TASK_CHECK(memcmp(in2, snap2, 2) == 0);
  TASK_CHECK(memcmp(in3, snap3, 3) == 0);

  rm_rf(root);
  return 0;
}

static int case_normal_abandon(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  struct DeepState_ReplayCase cases[2];
  set_case(&cases[0], 7, &test_abandon2, in2, sizeof(in2));
  set_case(&cases[1], 8, &test_abandon1, in1, sizeof(in1));

  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 502, 2, 4096, -1, out);
  struct DeepState_ReplayResult results[2];
  int rc = DeepState_ReplayBatch(cases, 2, &cfg, results);
  TASK_CHECK(rc == 0); /* abandonment is not a failure */

  TASK_CHECK(results[0].state == DeepState_ReplayComplete);
  TASK_CHECK(results[0].result == DeepState_TestRunAbandon);
  TASK_CHECK(results[0].consumed == 2);
  TASK_CHECK(results[1].result == DeepState_TestRunAbandon);
  TASK_CHECK(results[1].consumed == 1);

  TASK_CHECK(join_path(path, sizeof(path), out, "7.pass") == 0);
  TASK_CHECK(!path_exists(path));
  TASK_CHECK(join_path(path, sizeof(path), out, "7.log") == 0);
  TASK_CHECK(path_exists(path));

  rm_rf(root);
  return 0;
}

static int case_inherited_streams(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  /* Leave pending content in the parent's Information stream. */
  DeepState_StreamCStr(DeepState_LogInfo, "INHERITED_STREAM_TOKEN");

  size_t saved_index = DeepState_InputIndex;
  struct DeepState_TestRunInfo *saved_run = DeepState_CurrentTestRun;
  const char *saved_out = FLAGS_output_test_dir;
  int saved_has = HAS_FLAG_output_test_dir;

  struct DeepState_ReplayCase cases[1];
  set_case(&cases[0], 44, &test_stream, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 503, 1, 4096, -1, out);
  struct DeepState_ReplayResult results[1];

  int rc = DeepState_ReplayBatch(cases, 1, &cfg, results);
  TASK_CHECK(rc == 0);

  /* The parent's stream content was never flushed nor changed by the batch. */
  DeepState_ClearStream(DeepState_LogInfo);
  TASK_CHECK(DeepState_InputIndex == saved_index);
  TASK_CHECK(DeepState_CurrentTestRun == saved_run);
  TASK_CHECK(FLAGS_output_test_dir == saved_out);
  TASK_CHECK(HAS_FLAG_output_test_dir == saved_has);

  TASK_CHECK(join_path(path, sizeof(path), out, "44.log") == 0);
  TASK_CHECK(file_contains(path, "CHILD_STREAM_TOKEN"));
  TASK_CHECK(!file_contains(path, "INHERITED_STREAM_TOKEN"));

  rm_rf(root);
  return 0;
}

static int case_log_caps(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  quiet_logs();
  struct DeepState_ReplayCase cases[2];
  set_case(&cases[0], 1, &test_biglog, in1, sizeof(in1));
  set_case(&cases[1], 2, &test_biglog, in2, sizeof(in2));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 504, 2, 1000, -1, out);
  struct DeepState_ReplayResult results[2];
  int rc = DeepState_ReplayBatch(cases, 2, &cfg, results);
  restore_logs();
  TASK_CHECK(rc == 0);

  TASK_CHECK(results[0].log_bytes == 200000);
  TASK_CHECK(results[0].log_truncated);
  TASK_CHECK(results[1].log_bytes == 200000);
  TASK_CHECK(results[1].log_truncated);

  /* The retained log is the first 1000 raw bytes. */
  uint8_t expect[1000];
  for (size_t i = 0; i < sizeof(expect); ++i) {
    expect[i] = (uint8_t) (i * 7u + 3u);
  }
  TASK_CHECK(join_path(path, sizeof(path), out, "1.log") == 0);
  TASK_CHECK(file_equals(path, expect, sizeof(expect)));
  TASK_CHECK(join_path(path, sizeof(path), out, "2.log") == 0);
  TASK_CHECK(file_equals(path, expect, sizeof(expect)));

  rm_rf(root);
  return 0;
}

static int case_zero_byte_input(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  struct DeepState_ReplayCase cases[1];
  set_case(&cases[0], 5, &test_pass0, NULL, 0);
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 505, 1, 0, -1, out);
  struct DeepState_ReplayResult results[1];
  int rc = DeepState_ReplayBatch(cases, 1, &cfg, results);
  TASK_CHECK(rc == 0);

  TASK_CHECK(results[0].result == DeepState_TestRunPass);
  TASK_CHECK(results[0].consumed == 0);
  TASK_CHECK(join_path(path, sizeof(path), out, "5.pass") == 0);
  TASK_CHECK(path_exists(path));
  TASK_CHECK(file_size(path) == 0);

  rm_rf(root);
  return 0;
}

/* A zero-byte log cap must still drain a very chatty normal test; otherwise
 * the child would block on a full pipe forever. */
static int case_zero_cap_drain(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  quiet_logs();
  struct DeepState_ReplayCase cases[1];
  set_case(&cases[0], 6, &test_biglog, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 506, 1, 0, -1, out);
  struct DeepState_ReplayResult results[1];
  int rc = DeepState_ReplayBatch(cases, 1, &cfg, results);
  restore_logs();
  TASK_CHECK(rc == 0);

  TASK_CHECK(results[0].result == DeepState_TestRunPass);
  TASK_CHECK(results[0].log_bytes == 200000);
  TASK_CHECK(results[0].log_truncated);
  TASK_CHECK(join_path(path, sizeof(path), out, "6.log") == 0);
  TASK_CHECK(path_exists(path) && file_size(path) == 0);

  rm_rf(root);
  return 0;
}

/* With jobs=2, tasks 1 and 2 run before 3 and 4, but tasks 1 and 2 are only
 * released after both have started; both then finish before 3/4 launch, so
 * results never reorder by completion. */
static int case_bounded_parallel(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  struct harness h;
  TASK_CHECK(harness_open(&h) == 0);
  g_ctrl_run = 609;
  TASK_CHECK(harness_fork(&h, ctrl_release_two) == 0);

  struct DeepState_ReplayCase cases[4];
  set_case(&cases[0], 1, &test_gated, in1, sizeof(in1));
  set_case(&cases[1], 2, &test_gated, in2, sizeof(in2));
  set_case(&cases[2], 3, &test_pass1, in1, sizeof(in1));
  set_case(&cases[3], 4, &test_pass2, in2, sizeof(in2));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 609, 2, 4096, h.ctl_r, out);
  struct DeepState_ReplayResult results[4];
  int rc = DeepState_ReplayBatch(cases, 4, &cfg, results);
  TASK_CHECK(rc == 0);

  TASK_CHECK(results[0].id == 1 && results[0].consumed == 1);
  TASK_CHECK(results[1].id == 2 && results[1].consumed == 1);
  TASK_CHECK(results[2].id == 3 && results[2].consumed == 1);
  TASK_CHECK(results[3].id == 4 && results[3].consumed == 2);
  for (size_t i = 0; i < 4; ++i) {
    TASK_CHECK(results[i].result == DeepState_TestRunPass);
  }
  TASK_CHECK(join_path(path, sizeof(path), out, "1.pass") == 0);
  TASK_CHECK(file_equals(path, in1, 1));
  TASK_CHECK(join_path(path, sizeof(path), out, "4.pass") == 0);
  TASK_CHECK(file_equals(path, in2, 2));
  TASK_CHECK(harness_close(&h) == 0);

  rm_rf(root);
  return 0;
}

/* ======================================================================== */
/* cancel group                                                             */
/* ======================================================================== */

static int case_eof_boundary(void) {
  char root[PATH_MAX], out[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  int ctl[2];
  TASK_CHECK(pipe(ctl) == 0);
  close(ctl[1]); /* EOF at a record boundary */

  struct DeepState_ReplayCase cases[1];
  set_case(&cases[0], 9, &test_pass1, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 601, 1, 4096, ctl[0], out);
  struct DeepState_ReplayResult results[1];
  int rc = DeepState_ReplayBatch(cases, 1, &cfg, results);
  TASK_CHECK(rc == 0);
  TASK_CHECK(results[0].result == DeepState_TestRunPass);
  close(ctl[0]);

  rm_rf(root);
  return 0;
}

static int case_partial_record_error(void) {
  char root[PATH_MAX], out[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  int ctl[2];
  TASK_CHECK(pipe(ctl) == 0);
  uint64_t half[2] = {601, 9};
  TASK_CHECK(fd_write_all(ctl[1], half, 8) == 0); /* half a record */
  close(ctl[1]);

  struct DeepState_ReplayCase cases[1];
  set_case(&cases[0], 9, &test_pass1, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 601, 1, 4096, ctl[0], out);
  struct DeepState_ReplayResult results[1];
  memset(results, 0x5a, sizeof(results));
  int rc = DeepState_ReplayBatch(cases, 1, &cfg, results);
  TASK_CHECK(rc == 2);
  TASK_CHECK(!path_exists(out));
  TASK_CHECK(count_staging(root) == 0);
  for (size_t i = 0; i < sizeof(results); ++i) {
    TASK_CHECK(((const unsigned char *) results)[i] == 0x5a);
  }
  close(ctl[0]);

  rm_rf(root);
  return 0;
}

static int case_queued_cancel(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  struct harness h;
  TASK_CHECK(harness_open(&h) == 0);
  g_ctrl_run = 602;
  g_ctrl_cancel_id = 31; /* queued, must never start */
  g_ctrl_normal_id = 30;
  TASK_CHECK(harness_fork(&h, ctrl_queued) == 0);

  struct DeepState_ReplayCase cases[2];
  set_case(&cases[0], 30, &test_gated, in1, sizeof(in1));
  set_case(&cases[1], 31, &test_marked, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 602, 1, 4096, h.ctl_r, out);
  struct DeepState_ReplayResult results[2];
  int rc = DeepState_ReplayBatch(cases, 2, &cfg, results);
  TASK_CHECK(rc == 0);

  TASK_CHECK(results[0].state == DeepState_ReplayComplete);
  TASK_CHECK(results[0].result == DeepState_TestRunPass);
  TASK_CHECK(results[0].consumed == 1);
  TASK_CHECK(results[1].state == DeepState_ReplayCanceled);
  TASK_CHECK(results[1].result == DeepState_TestRunAbandon);
  TASK_CHECK(results[1].consumed == 0);

  /* The queued case must never have started. */
  TASK_CHECK(join_path(path, sizeof(path), out, "31.log") == 0);
  TASK_CHECK(!file_contains(path, "GATED_MARKED_STARTED"));
  TASK_CHECK(join_path(path, sizeof(path), out, "31.pass") == 0);
  TASK_CHECK(!path_exists(path));
  TASK_CHECK(join_path(path, sizeof(path), out, "30.pass") == 0);
  TASK_CHECK(file_equals(path, in1, 1));

  TASK_CHECK(harness_close(&h) == 0);
  rm_rf(root);
  return 0;
}

static int case_running_cancel(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  struct harness h;
  TASK_CHECK(harness_open(&h) == 0);
  g_ctrl_run = 603;
  g_ctrl_cancel_id = 41;
  TASK_CHECK(harness_fork(&h, ctrl_running) == 0);

  struct DeepState_ReplayCase cases[1];
  set_case(&cases[0], 41, &test_gated, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 603, 1, 4096, h.ctl_r, out);
  struct DeepState_ReplayResult results[1];
  int rc = DeepState_ReplayBatch(cases, 1, &cfg, results);
  TASK_CHECK(rc == 0); /* cancellation is not a failure */

  TASK_CHECK(results[0].state == DeepState_ReplayCanceled);
  TASK_CHECK(results[0].result == DeepState_TestRunAbandon);
  TASK_CHECK(results[0].consumed == 0);
  TASK_CHECK(join_path(path, sizeof(path), out, "41.pass") == 0);
  TASK_CHECK(!path_exists(path));
  TASK_CHECK(join_path(path, sizeof(path), out, "41.log") == 0);
  TASK_CHECK(path_exists(path));

  TASK_CHECK(harness_close(&h) == 0);
  rm_rf(root);
  return 0;
}

static int case_split_and_stale(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  struct harness h;
  TASK_CHECK(harness_open(&h) == 0);
  g_ctrl_run = 604;
  g_ctrl_cancel_id = 51; /* running, validly canceled */
  g_ctrl_normal_id = 52; /* queued; only stale commands target it */
  TASK_CHECK(harness_fork(&h, ctrl_split_stale) == 0);

  struct DeepState_ReplayCase cases[2];
  set_case(&cases[0], 51, &test_gated, in1, sizeof(in1));
  set_case(&cases[1], 52, &test_pass1, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 604, 1, 4096, h.ctl_r, out);
  struct DeepState_ReplayResult results[2];
  int rc = DeepState_ReplayBatch(cases, 2, &cfg, results);
  TASK_CHECK(rc == 0);

  TASK_CHECK(results[0].state == DeepState_ReplayCanceled);
  /* The stale cross-run cancel for the queued case must be ignored. */
  TASK_CHECK(results[1].state == DeepState_ReplayComplete);
  TASK_CHECK(results[1].result == DeepState_TestRunPass);
  TASK_CHECK(results[1].consumed == 1);
  TASK_CHECK(join_path(path, sizeof(path), out, "52.pass") == 0);
  TASK_CHECK(file_equals(path, in1, 1));

  TASK_CHECK(harness_close(&h) == 0);
  rm_rf(root);
  return 0;
}

static int case_completion_boundary(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  struct harness h;
  TASK_CHECK(harness_open(&h) == 0);
  g_ctrl_run = 605;
  g_ctrl_cancel_id = 61;
  TASK_CHECK(harness_fork(&h, ctrl_completion) == 0);

  /* The child is released right after the cancel is written, so it may have
   * completed its pass by the time the parent is able to reap it; a cancel
   * parsed before the reap must still win. */
  struct DeepState_ReplayCase cases[1];
  set_case(&cases[0], 61, &test_gated, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 605, 1, 4096, h.ctl_r, out);
  struct DeepState_ReplayResult results[1];
  int rc = DeepState_ReplayBatch(cases, 1, &cfg, results);
  TASK_CHECK(rc == 0);

  TASK_CHECK(results[0].state == DeepState_ReplayCanceled);
  TASK_CHECK(results[0].result == DeepState_TestRunAbandon);
  TASK_CHECK(results[0].consumed == 0);
  TASK_CHECK(join_path(path, sizeof(path), out, "61.pass") == 0);
  TASK_CHECK(!path_exists(path));

  TASK_CHECK(harness_close(&h) == 0);
  rm_rf(root);
  return 0;
}

static int case_second_run_after_cancel(void) {
  char root[PATH_MAX], out1[PATH_MAX], out2[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out1, sizeof(out1), root, "out1") == 0);
  TASK_CHECK(join_path(out2, sizeof(out2), root, "out2") == 0);

  /* First batch: cancel case 71. */
  struct harness h;
  TASK_CHECK(harness_open(&h) == 0);
  g_ctrl_run = 606;
  g_ctrl_cancel_id = 71;
  TASK_CHECK(harness_fork(&h, ctrl_running) == 0);
  struct DeepState_ReplayCase cases[1];
  set_case(&cases[0], 71, &test_gated, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 606, 1, 4096, h.ctl_r, out1);
  struct DeepState_ReplayResult results[1];
  TASK_CHECK(DeepState_ReplayBatch(cases, 1, &cfg, results) == 0);
  TASK_CHECK(results[0].state == DeepState_ReplayCanceled);
  TASK_CHECK(harness_close(&h) == 0);

  /* Second batch: same case id, different run id, must run normally. */
  struct DeepState_ReplayCase cases2[1];
  set_case(&cases2[0], 71, &test_pass1, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg2;
  set_cfg(&cfg2, 607, 1, 4096, -1, out2);
  struct DeepState_ReplayResult results2[1];
  TASK_CHECK(DeepState_ReplayBatch(cases2, 1, &cfg2, results2) == 0);
  TASK_CHECK(results2[0].state == DeepState_ReplayComplete);
  TASK_CHECK(results2[0].result == DeepState_TestRunPass);
  TASK_CHECK(results2[0].consumed == 1);
  TASK_CHECK(results2[0].run_id == 607);
  TASK_CHECK(join_path(path, sizeof(path), out2, "71.pass") == 0);
  TASK_CHECK(file_equals(path, in1, 1));
  /* The earlier output is untouched. */
  TASK_CHECK(join_path(path, sizeof(path), out1, "71.pass") == 0);
  TASK_CHECK(!path_exists(path));

  rm_rf(root);
  return 0;
}

static int case_fd_and_child_cleanup(void) {
  char root[PATH_MAX], out[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  int fds_before = count_open_fds();

  struct harness h;
  TASK_CHECK(harness_open(&h) == 0);
  g_ctrl_run = 608;
  g_ctrl_cancel_id = 81;
  TASK_CHECK(harness_fork(&h, ctrl_running) == 0);

  int flags_before = fcntl(h.ctl_r, F_GETFL);
  TASK_CHECK(flags_before >= 0);

  struct DeepState_ReplayCase cases[1];
  set_case(&cases[0], 81, &test_gated, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 608, 1, 4096, h.ctl_r, out);
  struct DeepState_ReplayResult results[1];
  TASK_CHECK(DeepState_ReplayBatch(cases, 1, &cfg, results) == 0);

  /* The caller's control fd is still open with unchanged status flags. */
  TASK_CHECK(fcntl(h.ctl_r, F_GETFD) >= 0);
  TASK_CHECK(fcntl(h.ctl_r, F_GETFL) == flags_before);

  TASK_CHECK(harness_close(&h) == 0);

  TASK_CHECK(count_open_fds() == fds_before);
  TASK_CHECK(count_staging(root) == 0);

  int status = 0;
  errno = 0;
  pid_t leftover = waitpid(-1, &status, WNOHANG);
  TASK_CHECK(leftover == -1 && errno == ECHILD);

  rm_rf(root);
  return 0;
}

/* The framework's automatic output_test_dir saving is suppressed inside the
 * replay children: no saved .pass/.fail files may appear in a parent-configured
 * directory even for a passing test. */
static int case_no_framework_saving(void) {
  char root[PATH_MAX], out[PATH_MAX], savedir[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);
  TASK_CHECK(join_path(savedir, sizeof(savedir), root, "framework-save") == 0);
  TASK_CHECK(mkdir(savedir, 0755) == 0);

  const char *saved_dir = FLAGS_output_test_dir;
  int saved_has = HAS_FLAG_output_test_dir;
  FLAGS_output_test_dir = savedir;
  HAS_FLAG_output_test_dir = 1;

  struct DeepState_ReplayCase cases[1];
  set_case(&cases[0], 3, &test_pass2, in2, sizeof(in2));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 610, 1, 4096, -1, out);
  struct DeepState_ReplayResult results[1];
  int rc = DeepState_ReplayBatch(cases, 1, &cfg, results);

  TASK_CHECK(rc == 0);
  TASK_CHECK(results[0].result == DeepState_TestRunPass);

  /* The parent's option is untouched and the directory stayed empty. */
  TASK_CHECK(FLAGS_output_test_dir == savedir);
  TASK_CHECK(HAS_FLAG_output_test_dir == 1);
  TASK_CHECK(count_staging(savedir) == 0);
  DIR *d = opendir(savedir);
  TASK_CHECK(d != NULL);
  int entries = 0;
  struct dirent *dp;
  while ((dp = readdir(d)) != NULL) {
    if (strcmp(dp->d_name, ".") && strcmp(dp->d_name, "..")) {
      ++entries;
    }
  }
  closedir(d);
  TASK_CHECK(entries == 0);
  TASK_CHECK(join_path(path, sizeof(path), out, "3.pass") == 0);
  TASK_CHECK(file_equals(path, in2, 2));

  FLAGS_output_test_dir = saved_dir;
  HAS_FLAG_output_test_dir = saved_has;
  rm_rf(root);
  return 0;
}

/* ======================================================================== */
/* artifacts group                                                          */
/* ======================================================================== */

static int case_manifest_and_bytes(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  quiet_logs();
  struct DeepState_ReplayCase cases[4];
  set_case(&cases[0], 10, &test_pass2, in2, sizeof(in2));    /* pass, 2 */
  set_case(&cases[1], 20, &test_pass0, NULL, 0);             /* pass, 0 */
  set_case(&cases[2], 30, &test_abandon1, in1, sizeof(in1)); /* abandon, 1 */
  set_case(&cases[3], 40, &test_fail1, in1, sizeof(in1));    /* fail, 1 */
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 701, 4, 4096, -1, out);
  struct DeepState_ReplayResult results[4];
  int rc = DeepState_ReplayBatch(cases, 4, &cfg, results);
  restore_logs();
  TASK_CHECK(rc == 1); /* a Fail result is reported through the return code */

  TASK_CHECK(results[0].result == DeepState_TestRunPass &&
             results[0].consumed == 2);
  TASK_CHECK(results[1].result == DeepState_TestRunPass &&
             results[1].consumed == 0);
  TASK_CHECK(results[2].result == DeepState_TestRunAbandon &&
             results[2].consumed == 1);
  TASK_CHECK(results[3].result == DeepState_TestRunFail &&
             results[3].consumed == 1);

  const char *expect =
      "deepstate-replay-v1\t701\n"
      "10\tcomplete\t0\t2\t0\t0\n"
      "20\tcomplete\t0\t0\t0\t0\n"
      "30\tcomplete\t3\t1\t0\t0\n"
      "40\tcomplete\t1\t1\t0\t0\n";
  TASK_CHECK(join_path(path, sizeof(path), out, "manifest.tsv") == 0);
  TASK_CHECK(file_equals(path, expect, strlen(expect)));

  /* Passing prefixes are published exactly; no other case gets a .pass. */
  TASK_CHECK(join_path(path, sizeof(path), out, "10.pass") == 0);
  TASK_CHECK(file_equals(path, in2, 2));
  TASK_CHECK(join_path(path, sizeof(path), out, "20.pass") == 0);
  TASK_CHECK(path_exists(path) && file_size(path) == 0);
  TASK_CHECK(join_path(path, sizeof(path), out, "30.pass") == 0);
  TASK_CHECK(!path_exists(path));
  TASK_CHECK(join_path(path, sizeof(path), out, "40.pass") == 0);
  TASK_CHECK(!path_exists(path));

  /* Every case has a (bounded) log, empty under quiet logging here. */
  for (uint64_t id = 10; id <= 40; id += 10) {
    char name[32];
    snprintf(name, sizeof(name), "%llu.log", (unsigned long long) id);
    TASK_CHECK(join_path(path, sizeof(path), out, name) == 0);
    TASK_CHECK(path_exists(path) && file_size(path) == 0);
  }

  rm_rf(root);
  return 0;
}

static int case_log_bounds_manifest(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  quiet_logs();
  struct DeepState_ReplayCase cases[3];
  set_case(&cases[0], 1, &test_biglog, in1, sizeof(in1));
  set_case(&cases[1], 2, &test_smalllog, in1, sizeof(in1));
  set_case(&cases[2], 3, &test_pass1, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 702, 3, 1000, -1, out);
  struct DeepState_ReplayResult results[3];
  int rc = DeepState_ReplayBatch(cases, 3, &cfg, results);
  restore_logs();
  TASK_CHECK(rc == 0);

  TASK_CHECK(results[0].log_bytes == 200000 && results[0].log_truncated);
  TASK_CHECK(results[1].log_bytes == 10 && !results[1].log_truncated);
  TASK_CHECK(results[2].log_bytes == 0 && !results[2].log_truncated);

  const char *expect =
      "deepstate-replay-v1\t702\n"
      "1\tcomplete\t0\t1\t200000\t1\n"
      "2\tcomplete\t0\t1\t10\t0\n"
      "3\tcomplete\t0\t1\t0\t0\n";
  TASK_CHECK(join_path(path, sizeof(path), out, "manifest.tsv") == 0);
  TASK_CHECK(file_equals(path, expect, strlen(expect)));

  TASK_CHECK(join_path(path, sizeof(path), out, "1.log") == 0);
  TASK_CHECK(file_size(path) == 1000);
  TASK_CHECK(join_path(path, sizeof(path), out, "2.log") == 0);
  TASK_CHECK(file_equals(path, "0123456789", 10));
  TASK_CHECK(join_path(path, sizeof(path), out, "3.log") == 0);
  TASK_CHECK(path_exists(path) && file_size(path) == 0);

  rm_rf(root);
  return 0;
}

static int case_no_publish_invalid(void) {
  char root[PATH_MAX], out[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);

  struct DeepState_TestInfo t1 = {NULL, t_pass1, "T1", __FILE__, 1};
  struct DeepState_TestInfo t2 = {NULL, t_pass2, "T2", __FILE__, 2};
  struct DeepState_ReplayCase dup[2];
  set_case(&dup[0], 5, &t1, in1, sizeof(in1));
  set_case(&dup[1], 5, &t2, in2, sizeof(in2)); /* duplicate id */
  struct DeepState_ReplayConfig cfg;
  struct DeepState_ReplayResult results[2];

  set_cfg(&cfg, 703, 1, 4096, -1, out);
  memset(results, 0x5a, sizeof(results));
  TASK_CHECK(DeepState_ReplayBatch(dup, 2, &cfg, results) == 2);
  for (size_t i = 0; i < sizeof(results); ++i) {
    TASK_CHECK(((const unsigned char *) results)[i] == 0x5a);
  }
  TASK_CHECK(!path_exists(out));

  /* Null pointers, zero run id, bad jobs, oversized cap, null data. */
  TASK_CHECK(DeepState_ReplayBatch(NULL, 1, &cfg, results) == 2);
  TASK_CHECK(DeepState_ReplayBatch(dup, 1, NULL, results) == 2);
  TASK_CHECK(DeepState_ReplayBatch(dup, 1, &cfg, NULL) == 2);

  struct DeepState_ReplayCase one[1];
  set_case(&one[0], 6, &t1, in1, sizeof(in1));
  struct DeepState_ReplayConfig bad;
  set_cfg(&bad, 0, 1, 4096, -1, out);
  TASK_CHECK(DeepState_ReplayBatch(one, 1, &bad, results) == 2);
  set_cfg(&bad, 704, 0, 4096, -1, out);
  TASK_CHECK(DeepState_ReplayBatch(one, 1, &bad, results) == 2);
  set_cfg(&bad, 704, 33, 4096, -1, out);
  TASK_CHECK(DeepState_ReplayBatch(one, 1, &bad, results) == 2);
  set_cfg(&bad, 704, 1, (size_t) 1024 * 1024 + 1, -1, out);
  TASK_CHECK(DeepState_ReplayBatch(one, 1, &bad, results) == 2);

  struct DeepState_ReplayCase null_data[1];
  set_case(&null_data[0], 7, &t1, NULL, 4);
  set_cfg(&bad, 704, 1, 4096, -1, out);
  TASK_CHECK(DeepState_ReplayBatch(null_data, 1, &bad, results) == 2);

  TASK_CHECK(!path_exists(out));
  TASK_CHECK(count_staging(root) == 0);

  rm_rf(root);
  return 0;
}

static int case_no_overwrite_existing(void) {
  char root[PATH_MAX], out[PATH_MAX], path[PATH_MAX];
  TASK_CHECK(mkroot(root, sizeof(root)) == 0);
  TASK_CHECK(join_path(out, sizeof(out), root, "out") == 0);
  TASK_CHECK(mkdir(out, 0755) == 0);
  TASK_CHECK(join_path(path, sizeof(path), out, "keep") == 0);
  int keep_fd = open(path, O_CREAT | O_WRONLY, 0644);
  TASK_CHECK(keep_fd >= 0);
  TASK_CHECK(fd_write_all(keep_fd, "keep", 4) == 0);
  TASK_CHECK(close(keep_fd) == 0);

  struct DeepState_ReplayCase cases[1];
  set_case(&cases[0], 1, &test_pass1, in1, sizeof(in1));
  struct DeepState_ReplayConfig cfg;
  set_cfg(&cfg, 705, 1, 4096, -1, out);
  struct DeepState_ReplayResult results[1];
  memset(results, 0x5a, sizeof(results));
  TASK_CHECK(DeepState_ReplayBatch(cases, 1, &cfg, results) == 2);
  for (size_t i = 0; i < sizeof(results); ++i) {
    TASK_CHECK(((const unsigned char *) results)[i] == 0x5a);
  }

  /* The pre-existing directory and its contents are untouched. */
  TASK_CHECK(file_equals(path, "keep", 4));
  TASK_CHECK(count_staging(root) == 0);

  rm_rf(root);
  return 0;
}

/* ---- registration ------------------------------------------------------- */

const struct task_case task_cases[] = {
    {"isolation", "different_consumption", case_different_consumption},
    {"isolation", "normal_abandon", case_normal_abandon},
    {"isolation", "inherited_streams", case_inherited_streams},
    {"isolation", "log_caps", case_log_caps},
    {"isolation", "zero_byte_input", case_zero_byte_input},
    {"isolation", "zero_cap_drain", case_zero_cap_drain},
    {"isolation", "bounded_parallel", case_bounded_parallel},

    {"cancel", "eof_boundary", case_eof_boundary},
    {"cancel", "partial_record_error", case_partial_record_error},
    {"cancel", "queued_cancel", case_queued_cancel},
    {"cancel", "running_cancel", case_running_cancel},
    {"cancel", "split_and_stale", case_split_and_stale},
    {"cancel", "completion_boundary", case_completion_boundary},
    {"cancel", "second_run_after_cancel", case_second_run_after_cancel},
    {"cancel", "fd_and_child_cleanup", case_fd_and_child_cleanup},
    {"cancel", "no_framework_saving", case_no_framework_saving},

    {"artifacts", "manifest_and_bytes", case_manifest_and_bytes},
    {"artifacts", "log_bounds_manifest", case_log_bounds_manifest},
    {"artifacts", "no_publish_invalid", case_no_publish_invalid},
    {"artifacts", "no_overwrite_existing", case_no_overwrite_existing},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
