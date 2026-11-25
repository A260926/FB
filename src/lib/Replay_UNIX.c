/*
 * Copyright (c) 2019 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Native Unix isolated replay batches.  See `deepstate/Replay.h` and
 * `task_support/replay_contract.md` for the public contract.
 *
 * Design notes:
 *  - Each case runs in its own forked child.  The child owns private
 *    current-run state and input cursor; the parent's DeepState globals,
 *    options, current-run pointer and streams are never touched.
 *  - Each child's stdout+stderr go to that child's own private pipe, so log
 *    bytes are always attributed to the correct case and preserved in the
 *    pipe's byte order.
 *  - A second, per-child pipe carries a fixed-width result report; no
 *    pointers or test metadata cross the process boundary.
 *  - The parent polls all of those pipes plus the optional caller control
 *    pipe, draining logs continuously so a chatty normal test never blocks.
 */

#if !defined(__unix)
# error "Replay_UNIX.c is only for Unix-like platforms"
#endif

#include <deepstate/Replay.h>
#include <deepstate/Compiler.h>
#include <deepstate/DeepState.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "DeepState.h"

DEEPSTATE_BEGIN_EXTERN_C

enum {
  kReplayMaxCases = 64,
  kReplayMaxJobs = 32,
  kReplayMaxLogLimit = 1024 * 1024,
  kReplayIoBuffer = 64 * 1024,
  /* Finite cleanup deadline for pipes still held by stray descendants. */
  kReplayCleanupMs = 2000
};

struct ReplayChildReport {
  uint32_t magic;
  uint32_t result;
  uint64_t consumed;
};

enum { kReplayChildMagic = 0x52504c59u /* "RPLY" */ };

struct ReplayCaseState {
  const struct DeepState_ReplayCase *input;

  int started;
  int done;
  pid_t pid;
  int pid_valid;
  int pgid_ok;
  int cancel_requested;
  int killed;

  int log_fd; /* read end of this child's merged stdout/stderr pipe */
  int res_fd; /* read end of this child's report pipe */

  enum DeepState_ReplayState state;
  enum DeepState_TestRunResult result;
  size_t consumed;

  size_t log_bytes;   /* every drained byte, for reporting/manifest */
  bool log_truncated;
  uint8_t *log;       /* bounded retained prefix, <= log_limit bytes */
  size_t log_len;
  size_t log_cap;
};

struct ReplayControl {
  uint8_t buf[16];
  size_t have;
  int eof;    /* EOF at a record boundary: control disabled. */
  int broken; /* Partial record at EOF: infrastructure error. */
};

static void ReplayInitState(struct ReplayCaseState *c,
                            const struct DeepState_ReplayCase *input) {
  memset(c, 0, sizeof(*c));
  c->input = input;
  c->log_fd = -1;
  c->res_fd = -1;
  c->result = DeepState_TestRunCrash;
  c->state = DeepState_ReplayComplete;
}

static void ReplayFreeCaseState(struct ReplayCaseState *c) {
  free(c->log);
  c->log = NULL;
  c->log_len = c->log_cap = 0;
  if (c->log_fd >= 0) {
    close(c->log_fd);
    c->log_fd = -1;
  }
  if (c->res_fd >= 0) {
    close(c->res_fd);
    c->res_fd = -1;
  }
}

static int ReplaySetNonBlock(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---- log retention ------------------------------------------------------ */

/* Append up to `limit` bytes total, discarding excess without failing. */
static int ReplayRetainLog(struct ReplayCaseState *c, size_t limit,
                           const uint8_t *bytes, size_t n) {
  if (n == 0 || c->log_len >= limit) {
    return 0;
  }
  size_t room = limit - c->log_len;
  size_t take = n < room ? n : room;
  size_t need = c->log_len + take;
  if (need > c->log_cap) {
    size_t new_cap = c->log_cap ? c->log_cap : 256;
    while (new_cap < need) {
      new_cap *= 2;
    }
    if (new_cap > limit) {
      new_cap = limit;
    }
    uint8_t *grown = (uint8_t *) realloc(c->log, new_cap);
    if (grown == NULL) {
      return -1;
    }
    c->log = grown;
    c->log_cap = new_cap;
  }
  memcpy(c->log + c->log_len, bytes, take);
  c->log_len += take;
  return 0;
}

static int ReplayRecordLog(struct ReplayCaseState *c, size_t limit,
                           const uint8_t *bytes, size_t n) {
  if (n == 0) {
    return 0;
  }
  if (c->log_bytes > SIZE_MAX - n) {
    c->log_bytes = SIZE_MAX;
  } else {
    c->log_bytes += n;
  }
  if (c->log_bytes > limit) {
    c->log_truncated = true;
  }
  return ReplayRetainLog(c, limit, bytes, n);
}

/* ---- control records ---------------------------------------------------- */

/* Consume every complete command currently in `ctl->buf`.  Commands for a
 * different run, an unknown case, or an already-terminal case are ignored. */
static void ReplayApplyControl(struct ReplayControl *ctl, uint64_t run_id,
                               struct ReplayCaseState *cases, size_t count) {
  size_t off = 0;
  while (ctl->have - off >= 16) {
    uint64_t rid;
    uint64_t cid;
    memcpy(&rid, ctl->buf + off, sizeof(rid));
    memcpy(&cid, ctl->buf + off + sizeof(rid), sizeof(cid));
    off += 16;
    if (rid != run_id) {
      continue;
    }
    for (size_t i = 0; i < count; ++i) {
      struct ReplayCaseState *c = &cases[i];
      if (c->input->id != cid) {
        continue;
      }
      if (!c->done) {
        c->cancel_requested = 1;
      }
      break;
    }
  }
  if (off != 0) {
    ctl->have -= off;
    memmove(ctl->buf, ctl->buf + off, ctl->have);
  }
}

/* Drain all currently readable control bytes without blocking and without
 * changing the caller's fd.  We only read after poll reports readiness, so the
 * read cannot block, and a zero return at readiness means EOF.  A clean EOF
 * at a record boundary disables control; a partial record at EOF is an
 * infrastructure error.  Returns 0 or -1. */
static int ReplayDrainControl(struct ReplayControl *ctl, int cancel_fd,
                              uint64_t run_id,
                              struct ReplayCaseState *cases, size_t count) {
  if (cancel_fd < 0 || ctl->eof || ctl->broken) {
    return ctl->broken ? -1 : 0;
  }

  for (;;) {
    ReplayApplyControl(ctl, run_id, cases, count);

    struct pollfd pfd;
    pfd.fd = cancel_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr;
    do {
      pr = poll(&pfd, 1, 0);
    } while (pr < 0 && errno == EINTR);
    if (pr < 0) {
      return -1;
    }
    if (pr == 0) {
      break; /* nothing currently readable */
    }

    int avail = 0;
    if (ioctl(cancel_fd, FIONREAD, &avail) != 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (avail > 0) {
      size_t space = sizeof(ctl->buf) - ctl->have;
      size_t want = (size_t) avail < space ? (size_t) avail : space;
      if (want == 0) {
        return -1;
      }
      ssize_t k = read(cancel_fd, ctl->buf + ctl->have, want);
      if (k < 0) {
        if (errno == EINTR) {
          continue;
        }
        return -1;
      }
      ctl->have += (size_t) k;
      continue;
    }

    /* Readable with no buffered bytes: EOF, or an empty readable socket. */
    uint8_t probe;
    ssize_t k = read(cancel_fd, &probe, 1);
    if (k == 0) {
      if (ctl->have != 0) {
        ctl->broken = 1;
        return -1;
      }
      ctl->eof = 1;
      break;
    }
    if (k < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      return -1;
    }
    if (ctl->have == sizeof(ctl->buf)) {
      return -1;
    }
    ctl->buf[ctl->have++] = probe;
  }

  ReplayApplyControl(ctl, run_id, cases, count);
  return ctl->broken ? -1 : 0;
}

/* ---- small IO helpers --------------------------------------------------- */

static int ReplayWriteAll(int fd, const void *buf, size_t len) {
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

static int ReplayWriteFile(const char *path, const void *buf, size_t len) {
  int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
  if (fd < 0) {
    return -1;
  }
  int rc = ReplayWriteAll(fd, buf, len);
  if (close(fd) != 0) {
    rc = -1;
  }
  return rc;
}

/* ---- path helpers ------------------------------------------------------- */

static int ReplayPathExists(const char *path) {
  struct stat st;
  if (lstat(path, &st) == 0) {
    return 1;
  }
  return errno == ENOENT ? 0 : -1;
}

static void ReplayRemoveTree(const char *path) {
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
        int n = snprintf(child, sizeof(child), "%s/%s", path, dp->d_name);
        if (n < 0 || (size_t) n >= sizeof(child)) {
          continue;
        }
        ReplayRemoveTree(child);
      }
      closedir(dir);
    }
    rmdir(path);
  } else {
    unlink(path);
  }
}

/* Split `path` into its parent directory.  Returns 0 on success. */
static int ReplayParentDir(const char *path, char *out, size_t out_size) {
  const char *slash = strrchr(path, '/');
  if (slash == NULL) {
    return snprintf(out, out_size, ".") < (int) out_size ? 0 : -1;
  }
  if (slash == path) {
    return snprintf(out, out_size, "/") < (int) out_size ? 0 : -1;
  }
  size_t len = (size_t) (slash - path);
  if (len + 1 > out_size) {
    return -1;
  }
  memcpy(out, path, len);
  out[len] = '\0';
  return 0;
}

/* Create a private staging directory beside `output_dir`.  Returns 0 with
 * `out` filled, or -1. */
static int ReplayMakeStagingDir(char *out, size_t out_size,
                                const char *output_dir, uint64_t run_id) {
  char parent[PATH_MAX];
  if (ReplayParentDir(output_dir, parent, sizeof(parent)) != 0) {
    return -1;
  }
  const char *slash = strrchr(output_dir, '/');
  const char *base = slash ? slash + 1 : output_dir;
  if (base[0] == '\0') {
    return -1;
  }
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    int n = snprintf(out, out_size, "%s/.%s.replay-staging-%llu-%ld-%u",
                     parent, base, (unsigned long long) run_id,
                     (long) getpid(), attempt);
    if (n < 0 || (size_t) n >= out_size) {
      return -1;
    }
    int exists = ReplayPathExists(out);
    if (exists < 0) {
      return -1;
    }
    if (exists == 0) {
      if (mkdir(out, 0700) == 0) {
        return 0;
      }
      if (errno != EEXIST) {
        return -1;
      }
    }
  }
  return -1;
}

/* ---- validation --------------------------------------------------------- */

static int ReplayValidate(const struct DeepState_ReplayCase *cases,
                          size_t count,
                          const struct DeepState_ReplayConfig *config,
                          const struct DeepState_ReplayResult *results) {
  if (cases == NULL || config == NULL || results == NULL) {
    return 2;
  }
  if (count < 1 || count > kReplayMaxCases) {
    return 2;
  }
  if (config->run_id == 0) {
    return 2;
  }
  if (config->jobs < 1 || config->jobs > kReplayMaxJobs) {
    return 2;
  }
  if (config->log_limit > kReplayMaxLogLimit) {
    return 2;
  }
  if (config->output_dir == NULL || config->output_dir[0] == '\0') {
    return 2;
  }

  if (config->cancel_fd != -1) {
    if (config->cancel_fd < 0) {
      return 2;
    }
    struct stat st;
    if (fstat(config->cancel_fd, &st) != 0 || !S_ISFIFO(st.st_mode)) {
      return 2;
    }
    int flags = fcntl(config->cancel_fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) == O_WRONLY) {
      return 2;
    }
  }

  for (size_t i = 0; i < count; ++i) {
    const struct DeepState_ReplayCase *c = &cases[i];
    if (c->id == 0 || c->test == NULL || c->test->test_func == NULL) {
      return 2;
    }
    if (c->size > (size_t) DeepState_InputSize) {
      return 2;
    }
    if (c->size > 0 && c->data == NULL) {
      return 2;
    }
    for (size_t j = 0; j < i; ++j) {
      if (cases[j].id == c->id) {
        return 2;
      }
    }
  }

  if (ReplayPathExists(config->output_dir) != 0) {
    return 2;
  }
  char parent[PATH_MAX];
  struct stat pst;
  if (ReplayParentDir(config->output_dir, parent, sizeof(parent)) != 0 ||
      stat(parent, &pst) != 0 || !S_ISDIR(pst.st_mode)) {
    return 2;
  }
  return 0;
}

/* ---- child side --------------------------------------------------------- */

/* Runs in the freshly forked child; never returns. */
static void ReplayChildMain(const struct DeepState_ReplayCase *input,
                            int report_fd) {
  enum DeepState_TestRunResult result = DeepState_TestRunCrash;
  size_t consumed = 0;
  int rc = DeepState_RunReplayChild(input->test, input->data, input->size,
                                    &result, &consumed);
  struct ReplayChildReport report;
  report.magic = kReplayChildMagic;
  if (rc != 0) {
    report.result = 0xffffffffu; /* parent turns this into an infra error */
    report.consumed = 0;
  } else {
    report.result = (uint32_t) result;
    report.consumed = (uint64_t) consumed;
  }

  fflush(stdout);
  fflush(stderr);
  ReplayWriteAll(report_fd, &report, sizeof(report));
  close(report_fd);
  _exit(0);
}

/* ---- parent event-loop helpers ----------------------------------------- */

/* Drain whatever is readable on a log pipe now, nonblocking. */
static int ReplayDrainLogAvailable(struct ReplayCaseState *c, size_t log_limit,
                                   uint8_t *scratch) {
  for (;;) {
    ssize_t k = read(c->log_fd, scratch, kReplayIoBuffer);
    if (k == 0) {
      close(c->log_fd);
      c->log_fd = -1;
      return 0;
    }
    if (k < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      return -1;
    }
    if (ReplayRecordLog(c, log_limit, scratch, (size_t) k) < 0) {
      return -1;
    }
  }
}

static long long ReplayNowMs(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Drain a log pipe to EOF, waiting at most `deadline_ms` for stray writers
 * (e.g. descendants that retained the pipe).  Always closes the fd. */
static int ReplayDrainLogToEof(struct ReplayCaseState *c, size_t log_limit,
                               uint8_t *scratch, long long deadline_ms) {
  long long deadline = ReplayNowMs() + deadline_ms;
  while (c->log_fd >= 0) {
    ssize_t k = read(c->log_fd, scratch, kReplayIoBuffer);
    if (k == 0) {
      close(c->log_fd);
      c->log_fd = -1;
      return 0;
    }
    if (k < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        long long left = deadline - ReplayNowMs();
        if (left <= 0) {
          close(c->log_fd);
          c->log_fd = -1;
          return 0;
        }
        struct pollfd pfd = {c->log_fd, POLLIN, 0};
        int pr = poll(&pfd, 1, (int) left);
        if (pr < 0 && errno != EINTR) {
          close(c->log_fd);
          c->log_fd = -1;
          return -1;
        }
        continue;
      }
      close(c->log_fd);
      c->log_fd = -1;
      return -1;
    }
    if (ReplayRecordLog(c, log_limit, scratch, (size_t) k) < 0) {
      return -1;
    }
  }
  return 0;
}

/* Read the fixed-width result report after the child has been reaped. */
static int ReplayReadReport(struct ReplayCaseState *c,
                            struct ReplayChildReport *out) {
  uint8_t *p = (uint8_t *) out;
  size_t got = 0;
  long long deadline = ReplayNowMs() + kReplayCleanupMs;
  while (got < sizeof(*out) && c->res_fd >= 0) {
    ssize_t k = read(c->res_fd, p + got, sizeof(*out) - got);
    if (k > 0) {
      got += (size_t) k;
      continue;
    }
    if (k == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      long long left = deadline - ReplayNowMs();
      if (left <= 0) {
        break;
      }
      struct pollfd pfd = {c->res_fd, POLLIN, 0};
      int pr = poll(&pfd, 1, (int) left);
      if (pr < 0 && errno != EINTR) {
        break;
      }
      continue;
    }
    break;
  }
  close(c->res_fd);
  c->res_fd = -1;
  return got == sizeof(*out) ? 0 : -1;
}

static void ReplayMarkCanceled(struct ReplayCaseState *c) {
  c->state = DeepState_ReplayCanceled;
  c->result = DeepState_TestRunAbandon;
  c->consumed = 0;
  c->done = 1;
}

/* Start one queued case in its own child.  Returns 0 or -1 (infra error). */
static int ReplaySpawn(struct ReplayCaseState *cases, size_t count,
                       size_t index, int cancel_fd) {
  struct ReplayCaseState *c = &cases[index];
  int log_pipe[2] = {-1, -1};
  int res_pipe[2] = {-1, -1};

  if (pipe(log_pipe) != 0) {
    return -1;
  }
  if (pipe(res_pipe) != 0) {
    close(log_pipe[0]);
    close(log_pipe[1]);
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(log_pipe[0]);
    close(log_pipe[1]);
    close(res_pipe[0]);
    close(res_pipe[1]);
    return -1;
  }

  if (pid == 0) {
    close(log_pipe[0]);
    close(res_pipe[0]);
    if (cancel_fd >= 0) {
      close(cancel_fd);
    }
    /* Close every runner pipe end inherited from earlier cases. */
    for (size_t i = 0; i < count; ++i) {
      if (cases[i].log_fd >= 0) {
        close(cases[i].log_fd);
      }
      if (cases[i].res_fd >= 0) {
        close(cases[i].res_fd);
      }
    }
    setpgid(0, 0);
    if (dup2(log_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(log_pipe[1], STDERR_FILENO) < 0) {
      _exit(127);
    }
    if (log_pipe[1] != STDOUT_FILENO && log_pipe[1] != STDERR_FILENO) {
      close(log_pipe[1]);
    }
    ReplayChildMain(c->input, res_pipe[1]);
    _exit(127);
  }

  close(log_pipe[1]);
  close(res_pipe[1]);

  if (setpgid(pid, pid) == 0) {
    c->pgid_ok = 1;
  }
  c->pid = pid;
  c->pid_valid = 1;
  c->started = 1;
  c->state = DeepState_ReplayComplete;
  c->log_fd = log_pipe[0];
  c->res_fd = res_pipe[0];
  if (ReplaySetNonBlock(c->log_fd) != 0 ||
      ReplaySetNonBlock(c->res_fd) != 0) {
    return -1;
  }
  return 0;
}

static void ReplayKillGroup(struct ReplayCaseState *c) {
  if (!c->pid_valid || c->killed) {
    return;
  }
  c->killed = 1;
  if (c->pgid_ok) {
    kill(-c->pid, SIGKILL);
  } else {
    kill(c->pid, SIGKILL);
  }
}

/* ---- publication -------------------------------------------------------- */

static int ReplayCasePath(char *out, size_t out_size, const char *dir,
                          uint64_t id, const char *suffix) {
  int n = snprintf(out, out_size, "%s/%llu.%s", dir,
                   (unsigned long long) id, suffix);
  return (n < 0 || (size_t) n >= out_size) ? -1 : 0;
}

static size_t ReplayConsumed(const struct ReplayCaseState *c) {
  size_t consumed = c->consumed;
  return consumed <= c->input->size ? consumed : c->input->size;
}

static int ReplayWriteArtifacts(const char *staging,
                                struct ReplayCaseState *cases, size_t count,
                                uint64_t run_id) {
  char path[PATH_MAX];

  for (size_t i = 0; i < count; ++i) {
    struct ReplayCaseState *c = &cases[i];
    if (ReplayCasePath(path, sizeof(path), staging, c->input->id, "log") != 0) {
      return -1;
    }
    const void *log = c->log ? (const void *) c->log : (const void *) "";
    if (ReplayWriteFile(path, log, c->log_len) != 0) {
      return -1;
    }

    if (c->state == DeepState_ReplayComplete &&
        c->result == DeepState_TestRunPass) {
      size_t consumed = ReplayConsumed(c);
      if (ReplayCasePath(path, sizeof(path), staging, c->input->id, "pass") !=
          0) {
        return -1;
      }
      const void *src = consumed ? (const void *) c->input->data
                                 : (const void *) "";
      if (ReplayWriteFile(path, src, consumed) != 0) {
        return -1;
      }
    }
  }

  int n = snprintf(path, sizeof(path), "%s/manifest.tsv", staging);
  if (n < 0 || (size_t) n >= sizeof(path)) {
    return -1;
  }

  /* Build the manifest with snprintf and ordinary byte writes: DeepState
   * interposes printf/fprintf in production, so they must not be used for
   * artifacts. */
  char buf[256];
  int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
  if (fd < 0) {
    return -1;
  }
  int rc = 0;
  n = snprintf(buf, sizeof(buf), "deepstate-replay-v1\t%llu\n",
               (unsigned long long) run_id);
  if (n < 0 || (size_t) n >= sizeof(buf) || ReplayWriteAll(fd, buf, (size_t) n)) {
    rc = -1;
  }
  for (size_t i = 0; rc == 0 && i < count; ++i) {
    struct ReplayCaseState *c = &cases[i];
    const char *state =
        c->state == DeepState_ReplayCanceled ? "canceled" : "complete";
    n = snprintf(buf, sizeof(buf), "%llu\t%s\t%d\t%zu\t%zu\t%d\n",
                 (unsigned long long) c->input->id, state, (int) c->result,
                 ReplayConsumed(c), c->log_bytes, c->log_truncated ? 1 : 0);
    if (n < 0 || (size_t) n >= sizeof(buf) ||
        ReplayWriteAll(fd, buf, (size_t) n)) {
      rc = -1;
    }
  }
  if (close(fd) != 0) {
    rc = -1;
  }
  return rc;
}

/* Create staging, fill it, and rename it into place.  Never exposes a partial
 * final directory and never overwrites an existing one. */
static int ReplayPublish(struct ReplayCaseState *cases, size_t count,
                         uint64_t run_id, const char *output_dir,
                         int *staging_created, char *staging_out,
                         size_t staging_out_size) {
  if (ReplayMakeStagingDir(staging_out, staging_out_size, output_dir,
                           run_id) != 0) {
    return -1;
  }
  *staging_created = 1;

  if (ReplayWriteArtifacts(staging_out, cases, count, run_id) != 0) {
    return -1;
  }
  /* Re-check right before the atomic publish so we never clobber a run that
   * appeared while we were staging. */
  if (ReplayPathExists(output_dir) != 0) {
    return -1;
  }
  if (rename(staging_out, output_dir) != 0) {
    return -1;
  }
  *staging_created = 0; /* ownership transferred to output_dir */
  return 0;
}

/* ---- public entry point ------------------------------------------------- */

int DeepState_ReplayBatch(const struct DeepState_ReplayCase *cases, size_t count,
                          const struct DeepState_ReplayConfig *config,
                          struct DeepState_ReplayResult *results) {
  if (ReplayValidate(cases, count, config, results) != 0) {
    return 2;
  }

  const uint64_t run_id = config->run_id;
  const unsigned jobs = config->jobs;
  const size_t log_limit = config->log_limit;
  const int cancel_fd = config->cancel_fd;
  const char *output_dir = config->output_dir;

  struct ReplayCaseState *state =
      (struct ReplayCaseState *) calloc(count, sizeof(*state));
  uint8_t *scratch = (uint8_t *) malloc(kReplayIoBuffer);
  if (state == NULL || scratch == NULL) {
    free(state);
    free(scratch);
    return 2;
  }
  for (size_t i = 0; i < count; ++i) {
    ReplayInitState(&state[i], &cases[i]);
  }

  struct ReplayControl ctl;
  memset(&ctl, 0, sizeof(ctl));

  int rc = 0;
  size_t running = 0;
  size_t next = 0;

  for (;;) {
    /* 1. Parse control before doing anything else this iteration. */
    if (cancel_fd >= 0 && !ctl.eof) {
      if (ReplayDrainControl(&ctl, cancel_fd, run_id, state, count) != 0) {
        rc = 2;
        break;
      }
    }

    /* 2. Stop any running child whose cancel we have already parsed. */
    for (size_t i = 0; i < next; ++i) {
      struct ReplayCaseState *c = &state[i];
      if (c->started && !c->done && c->cancel_requested) {
        ReplayKillGroup(c);
      }
    }

    /* 3. Launch queued cases in input order, skipping canceled ones. */
    while (rc == 0 && running < jobs && next < count) {
      struct ReplayCaseState *c = &state[next];
      if (c->cancel_requested) {
        ReplayMarkCanceled(c);
        ++next;
        continue;
      }
      if (ReplaySpawn(state, count, next, cancel_fd) != 0) {
        rc = 2;
        break;
      }
      ++running;
      ++next;
    }
    if (rc != 0) {
      break;
    }

    if (running == 0 && next >= count) {
      break; /* every case is terminal */
    }

    /* 4. Wait for any progress. */
    struct pollfd pfds[1 + 2 * kReplayMaxCases];
    int log_slot_of[kReplayMaxCases];
    size_t npfds = 0;
    for (size_t i = 0; i < count; ++i) {
      log_slot_of[i] = -1;
    }

    if (cancel_fd >= 0 && !ctl.eof) {
      pfds[npfds].fd = cancel_fd;
      pfds[npfds].events = POLLIN;
      pfds[npfds].revents = 0;
      ++npfds;
    }
    for (size_t i = 0; i < next; ++i) {
      struct ReplayCaseState *c = &state[i];
      if (!c->started || c->done) {
        continue;
      }
      if (c->res_fd >= 0) {
        pfds[npfds].fd = c->res_fd;
        pfds[npfds].events = POLLIN;
        pfds[npfds].revents = 0;
        ++npfds;
      }
      if (c->log_fd >= 0) {
        log_slot_of[i] = (int) npfds;
        pfds[npfds].fd = c->log_fd;
        pfds[npfds].events = POLLIN;
        pfds[npfds].revents = 0;
        ++npfds;
      }
    }

    if (npfds == 0) {
      rc = 2; /* running work exists but nothing to wait on */
      break;
    }

    int pr;
    do {
      pr = poll(pfds, npfds, -1);
    } while (pr < 0 && errno == EINTR);
    if (pr < 0) {
      rc = 2;
      break;
    }

    /* 5. Parse control that arrived while polling, before harvesting. */
    if (cancel_fd >= 0 && !ctl.eof) {
      if (ReplayDrainControl(&ctl, cancel_fd, run_id, state, count) != 0) {
        rc = 2;
        break;
      }
    }

    /* 6. Cancel wins at the reap boundary: kill before harvesting. */
    for (size_t i = 0; i < next; ++i) {
      struct ReplayCaseState *c = &state[i];
      if (c->started && !c->done && c->cancel_requested) {
        ReplayKillGroup(c);
      }
    }

    /* 7. Drain logs continuously so chatty runners never block. */
    for (size_t i = 0; i < next; ++i) {
      struct ReplayCaseState *c = &state[i];
      int slot = log_slot_of[i];
      if (slot < 0) {
        continue;
      }
      int rev = pfds[slot].revents;
      if (rev & (POLLIN | POLLHUP | POLLERR)) {
        if (ReplayDrainLogAvailable(c, log_limit, scratch) != 0) {
          rc = 2;
          break;
        }
      }
    }
    if (rc != 0) {
      break;
    }

    /* 8. Harvest any children that have exited. */
    for (size_t i = 0; i < next; ++i) {
      struct ReplayCaseState *c = &state[i];
      if (!c->started || c->done) {
        continue;
      }
      int status = 0;
      pid_t got = waitpid(c->pid, &status, WNOHANG);
      if (got == 0) {
        continue;
      }
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        rc = 2;
        break;
      }
      c->pid_valid = 0;
      if (c->cancel_requested) {
        /* A cancel parsed before this reap always wins, even if the child
         * had just produced a passing result. */
        ReplayMarkCanceled(c);
      } else {
        struct ReplayChildReport report;
        if (ReplayReadReport(c, &report) != 0 ||
            report.magic != kReplayChildMagic || report.result > 3) {
          if (WIFSIGNALED(status)) {
            /* A genuine uncaught crash in the child. */
            c->state = DeepState_ReplayComplete;
            c->result = DeepState_TestRunCrash;
            c->consumed = 0;
            c->done = 1;
          } else {
            rc = 2;
            break;
          }
        } else {
          c->state = DeepState_ReplayComplete;
          c->result = (enum DeepState_TestRunResult) report.result;
          c->consumed = (size_t) report.consumed;
          c->done = 1;
        }
      }
      if (ReplayDrainLogToEof(c, log_limit, scratch, kReplayCleanupMs) != 0) {
        rc = 2;
        break;
      }
      --running;
    }
    if (rc != 0) {
      break;
    }
  }

  /* Settle every child and close every owned descriptor, even on error. */
  for (size_t i = 0; i < count; ++i) {
    struct ReplayCaseState *c = &state[i];
    if (c->started && !c->done) {
      ReplayKillGroup(c);
      if (c->pid_valid) {
        int status = 0;
        while (waitpid(c->pid, &status, 0) < 0) {
          if (errno != EINTR) {
            break;
          }
        }
        c->pid_valid = 0;
      }
      struct ReplayChildReport report;
      (void) ReplayReadReport(c, &report);
      (void) ReplayDrainLogToEof(c, log_limit, scratch, kReplayCleanupMs);
    }
    if (c->log_fd >= 0) {
      close(c->log_fd);
      c->log_fd = -1;
    }
    if (c->res_fd >= 0) {
      close(c->res_fd);
      c->res_fd = -1;
    }
  }

  int staging_created = 0;
  char staging[PATH_MAX];

  if (rc == 0) {
    if (ReplayPublish(state, count, run_id, output_dir, &staging_created,
                      staging, sizeof(staging)) != 0) {
      rc = 2;
    }
  }
  if (staging_created) {
    ReplayRemoveTree(staging);
  }

  if (rc == 0) {
    int any_failure = 0;
    for (size_t i = 0; i < count; ++i) {
      struct ReplayCaseState *c = &state[i];
      results[i].run_id = run_id;
      results[i].id = c->input->id;
      results[i].state = c->state;
      results[i].result = c->result;
      results[i].consumed = ReplayConsumed(c);
      results[i].log_bytes = c->log_bytes;
      results[i].log_truncated = c->log_truncated;
      if (c->state == DeepState_ReplayComplete &&
          (c->result == DeepState_TestRunFail ||
           c->result == DeepState_TestRunCrash)) {
        any_failure = 1;
      }
    }
    rc = any_failure ? 1 : 0;
  }

  for (size_t i = 0; i < count; ++i) {
    ReplayFreeCaseState(&state[i]);
  }
  free(state);
  free(scratch);
  return rc;
}

DEEPSTATE_END_EXTERN_C
