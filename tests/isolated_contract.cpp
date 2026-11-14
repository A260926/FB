// Contract tests for the isolated checking entry point (checkIsolated /
// replayIsolated). Everything goes through the public entry points and real
// forked child processes; nothing here inspects the implementation.
//
// Sections print ISOLATED_RESULTS_OK, ISOLATED_SHRINK_OK, ISOLATED_REPLAY_OK
// and TLV_BOUNDARY_OK when (and only when) their checks pass.

// Expose POSIX declarations (usleep, setitimer, sigaction, ...) in strict
// -std=c++11 mode on glibc. Must come before any system header.
#if !defined(__APPLE__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <rapidcheck.h>
#include <rapidcheck/Isolated.h>

#include "tlv_parser.h"

namespace {

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__,   \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

#define CHECK_CONTAINS(haystack, needle)                                     \
  do {                                                                       \
    if ((haystack).find(needle) == std::string::npos) {                      \
      std::fprintf(stderr,                                                   \
                   "CHECK failed %s:%d: expected [%s] to contain [%s]\n",    \
                   __FILE__, __LINE__, (haystack).c_str(), (needle));        \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using rc::Gen;
using rc::IsolatedAttemptInfo;
using rc::IsolatedCheckResult;
using rc::IsolatedFailureClass;
using rc::IsolatedParams;
using rc::IsolatedReplayResult;

template <typename T>
using ResultType = typename rc::IsolatedCheckResult<T>::Type;

/// Parent-side sentinel: callbacks try to clobber it, but since they only
/// ever run in forked children, the parent's value must never change.
int g_sentinel = 0x5E17;

/// If two callbacks ever ran in the same process, the second one would see
/// this marker set and misbehave; the tests detect that.
int g_childMarker = 0;

/// A generator of `root` that shrinks one step at a time down to 0.
Gen<int> chainGen(int root) {
  return Gen<int>([root](const rc::Random &, int) {
    return rc::shrinkable::shrinkRecur(root, [](int x) {
      return x > 0 ? rc::seq::just(x - 1) : rc::Seq<int>();
    });
  });
}

/// A generator of a constant value with no shrinks.
Gen<int> constGen(int value) {
  return Gen<int>(
      [value](const rc::Random &, int) { return rc::shrinkable::just(value); });
}

int countOpenFds() {
  DIR *dir = opendir("/dev/fd");
  if (dir == nullptr) {
    return -1;
  }
  int count = 0;
  while (readdir(dir) != nullptr) {
    count++;
  }
  closedir(dir);
  // Includes ".", ".." and the directory fd itself; consistent across calls.
  return count;
}

int waitFor(pid_t pid) {
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while ((waited < 0) && (errno == EINTR));
  CHECK(waited == pid);
  return status;
}

std::string frameOf(const std::string &payload) {
  std::string frame(4, '\0');
  for (int i = 0; i < 4; i++) {
    frame[i] = static_cast<char>((payload.size() >> (i * 8)) & 0xFF);
  }
  return frame + payload;
}

// ---------------------------------------------------------------------------
// Controlled communication for the framing tests
// ---------------------------------------------------------------------------

/// Child side: writes all of `data` to `fd` in chunks of `chunk` bytes, then
/// closes and exits. Uses raw write() and _exit only.
void childWriteChunked(int fd, const std::string &data, std::size_t chunk) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t n = std::min(chunk, data.size() - offset);
    const ssize_t written = write(fd, data.data() + offset, n);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      _exit(2);
    }
    offset += static_cast<std::size_t>(written);
  }
  close(fd);
  _exit(0);
}

/// Child side: reads one frame from `fd` and verifies it byte for byte
/// against the (i * 31 + 7) & 0xFF pattern, reading slowly in small chunks
/// so the writer is forced into partial writes.
void childReadAndVerify(int fd, std::size_t expectedPayload) {
  unsigned char header[4];
  std::size_t got = 0;
  while (got < 4) {
    const ssize_t r = read(fd, header + got, 4 - got);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      _exit(2);
    }
    if (r == 0) {
      _exit(3);
    }
    got += static_cast<std::size_t>(r);
  }
  const std::size_t declared = static_cast<std::size_t>(header[0]) |
      (static_cast<std::size_t>(header[1]) << 8) |
      (static_cast<std::size_t>(header[2]) << 16) |
      (static_cast<std::size_t>(header[3]) << 24);
  if (declared != expectedPayload) {
    _exit(4);
  }

  char buffer[512];
  std::size_t pos = 0;
  while (pos < declared) {
    const ssize_t r = read(fd, buffer, sizeof(buffer));
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      _exit(2);
    }
    if (r == 0) {
      _exit(5);
    }
    for (ssize_t i = 0; i < r; i++, pos++) {
      const auto expected =
          static_cast<unsigned char>((pos * 31 + 7) & 0xFF);
      if (static_cast<unsigned char>(buffer[i]) != expected) {
        _exit(6);
      }
    }
    usleep(50); // stay slow so the writer's pipe really fills up
  }
  _exit(0);
}

// ---------------------------------------------------------------------------
// Framing channel: segmented, truncated, missing, oversized, partial writes
// ---------------------------------------------------------------------------

void testChannelSegmented() {
  std::string payload;
  for (int i = 0; i < 2000; i++) {
    payload.push_back(static_cast<char>('A' + (i % 26)));
  }

  int fds[2];
  CHECK(pipe(fds) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(fds[0]);
    childWriteChunked(fds[1], frameOf(payload), 1); // one byte per write
  }
  close(fds[1]);

  std::string message, error;
  const auto status =
      rc::detail::readIsolatedChannel(fds[0], 1 << 20, message, error);
  close(fds[0]);
  const int wstatus = waitFor(pid);
  CHECK(WIFEXITED(wstatus) && (WEXITSTATUS(wstatus) == 0));
  CHECK(status == rc::detail::IsolatedChannelStatus::Ok);
  CHECK(message == payload);
}

void testChannelTruncated() {
  std::string frame(4, '\0');
  frame[0] = 100; // declares 100 payload bytes ...
  frame += std::string(50, 'x'); // ... but only 50 follow

  int fds[2];
  CHECK(pipe(fds) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(fds[0]);
    childWriteChunked(fds[1], frame, 7);
  }
  close(fds[1]);

  std::string message, error;
  const auto status =
      rc::detail::readIsolatedChannel(fds[0], 1 << 20, message, error);
  close(fds[0]);
  const int wstatus = waitFor(pid);
  CHECK(WIFEXITED(wstatus) && (WEXITSTATUS(wstatus) == 0));
  CHECK(status == rc::detail::IsolatedChannelStatus::Truncated);
}

void testChannelNoMessage() {
  int fds[2];
  CHECK(pipe(fds) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(fds[0]);
    close(fds[1]); // exit without writing anything
    _exit(0);
  }
  close(fds[1]);

  std::string message, error;
  const auto status =
      rc::detail::readIsolatedChannel(fds[0], 1 << 20, message, error);
  close(fds[0]);
  const int wstatus = waitFor(pid);
  CHECK(WIFEXITED(wstatus) && (WEXITSTATUS(wstatus) == 0));
  CHECK(status == rc::detail::IsolatedChannelStatus::NoMessage);
}

void testChannelTooLarge() {
  const std::string payload(64, 'y');
  int fds[2];
  CHECK(pipe(fds) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(fds[0]);
    childWriteChunked(fds[1], frameOf(payload), 3);
  }
  close(fds[1]);

  std::string message, error;
  const auto status =
      rc::detail::readIsolatedChannel(fds[0], 32, message, error);
  close(fds[0]);
  const int wstatus = waitFor(pid);
  // The parent drains the pipe, so the writer finishes normally.
  CHECK(WIFEXITED(wstatus) && (WEXITSTATUS(wstatus) == 0));
  CHECK(status == rc::detail::IsolatedChannelStatus::TooLarge);
}

void testChannelPartialWrites() {
  const std::size_t total = 200 * 1024; // larger than any pipe capacity
  std::string payload(total, '\0');
  for (std::size_t i = 0; i < total; i++) {
    payload[i] = static_cast<char>((i * 31 + 7) & 0xFF);
  }

  int fds[2];
  CHECK(pipe(fds) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(fds[1]);
    childReadAndVerify(fds[0], total);
  }
  close(fds[0]);

  std::string error;
  CHECK(rc::detail::writeIsolatedChannel(fds[1], payload, error));
  close(fds[1]);
  const int wstatus = waitFor(pid);
  CHECK(WIFEXITED(wstatus) && (WEXITSTATUS(wstatus) == 0));
}

// ---------------------------------------------------------------------------
// Interrupted syscalls: SIGALRM without SA_RESTART during a real check
// ---------------------------------------------------------------------------

volatile sig_atomic_t g_alarmCount = 0;

void onAlarm(int) { g_alarmCount++; }

void testInterruptedCheck() {
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = &onAlarm;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; // no SA_RESTART: syscalls must surface EINTR
  CHECK(sigaction(SIGALRM, &sa, nullptr) == 0);

  struct itimerval timer;
  std::memset(&timer, 0, sizeof(timer));
  timer.it_interval.tv_usec = 5000; // 5 ms
  timer.it_value = timer.it_interval;
  CHECK(setitimer(ITIMER_REAL, &timer, nullptr) == 0);

  IsolatedParams params;
  params.maxSuccess = 5;
  const auto result = rc::checkIsolated(
      constGen(1),
      [](const int &) {
        usleep(20000); // 20 ms per case: many EINTR windows for the parent
        g_sentinel = -1;
        return true;
      },
      params);

  struct itimerval disarm;
  std::memset(&disarm, 0, sizeof(disarm));
  CHECK(setitimer(ITIMER_REAL, &disarm, nullptr) == 0);
  struct sigaction dfl;
  std::memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  CHECK(sigaction(SIGALRM, &dfl, nullptr) == 0);

  CHECK(g_alarmCount > 0); // signals really fired during the check
  CHECK(result.type == ResultType<int>::Success);
  CHECK(result.numSuccess == 5);
}

// ---------------------------------------------------------------------------
// Basic result categories
// ---------------------------------------------------------------------------

void testSuccessAndProcessIdentity() {
  std::vector<IsolatedAttemptInfo> attempts;
  IsolatedParams params;
  params.maxSuccess = 10;
  params.observer = [&](const IsolatedAttemptInfo &info) {
    attempts.push_back(info);
  };

  const auto result = rc::checkIsolated(
      constGen(1),
      [](const int &) {
        g_sentinel = -1; // clobbers only the child's copy
        return true;
      },
      params);

  CHECK(result.type == ResultType<int>::Success);
  CHECK(result.numSuccess == 10);
  CHECK(attempts.size() == 10);
  const pid_t parent = getpid();
  for (const auto &attempt : attempts) {
    CHECK(attempt.outcome == IsolatedAttemptInfo::Outcome::Success);
    CHECK(attempt.childPid > 0);
    CHECK(attempt.childPid != parent); // callback never runs in the parent
    CHECK(!attempt.shrinkPhase);
  }
}

void testAssertionFailureIdentityAndTags() {
  std::vector<IsolatedAttemptInfo> attempts;
  IsolatedParams params;
  params.observer = [&](const IsolatedAttemptInfo &info) {
    attempts.push_back(info);
  };

  const auto result = rc::checkIsolated(
      chainGen(8),
      [](const int &x) {
        RC_TAG("alpha");
        RC_CLASSIFY(x > 5, "big");
        RC_LOG() << "exec_pid=" << ::getpid();
        RC_ASSERT(x < 3);
      },
      params);

  CHECK(result.type == ResultType<int>::Failure);
  CHECK(result.failure);
  const auto &failure = *result.failure;
  CHECK(failure.failureClass == IsolatedFailureClass::Assertion);
  CHECK(failure.originalInput == 8);
  CHECK(failure.finalInput == 3);
  CHECK(failure.shrinkPath ==
        (std::vector<std::size_t>{0, 0, 0, 0, 0}));
  CHECK(failure.signalNumber == 0);
  CHECK_CONTAINS(failure.description, "RC_ASSERT(x < 3)");
  // The log comes from the execution that was actually selected.
  CHECK_CONTAINS(failure.description, "exec_pid=");
  // Tags of the selected (minimal) execution: x == 3 is not "big".
  // (RapidCheck stringifies string tags with quotes, see detail::showValue.)
  CHECK(failure.tags == std::vector<std::string>{"\"alpha\""});

  // The pid logged inside the child must match the pid the parent
  // recorded for the last accepted shrink attempt.
  const std::string key = "exec_pid=";
  const auto pos = failure.description.find(key);
  CHECK(pos != std::string::npos);
  const int loggedPid = std::stoi(failure.description.substr(pos + key.size()));
  CHECK(loggedPid != getpid());
  const auto lastAccepted = std::find_if(
      attempts.rbegin(), attempts.rend(), [](const IsolatedAttemptInfo &i) {
        return i.shrinkPhase && i.accepted;
      });
  CHECK(lastAccepted != attempts.rend());
  CHECK(loggedPid == lastAccepted->childPid);
}

void testDiscardGaveUp() {
  std::vector<IsolatedAttemptInfo> attempts;
  IsolatedParams params;
  params.maxSuccess = 5;
  params.maxDiscardRatio = 2; // gave up after 10 discards
  params.observer = [&](const IsolatedAttemptInfo &info) {
    attempts.push_back(info);
  };

  const auto result = rc::checkIsolated(
      constGen(1),
      [](const int &x) {
        RC_PRE(x > 1000); // never holds
        return true;
      },
      params);

  CHECK(result.type == ResultType<int>::GaveUp);
  CHECK(result.numDiscarded == 11);
  CHECK_CONTAINS(result.description, "RC_PRE");
  CHECK(attempts.size() == 11);
  for (const auto &attempt : attempts) {
    CHECK(attempt.outcome == IsolatedAttemptInfo::Outcome::Discard);
  }
}

void testExceptionsAndVoid() {
  {
    const auto result = rc::checkIsolated(
        constGen(0),
        [](const int &) -> bool {
          throw std::runtime_error("boom-exception");
        },
        IsolatedParams());
    CHECK(result.type == ResultType<int>::Failure);
    CHECK(result.failure->failureClass == IsolatedFailureClass::Assertion);
    CHECK_CONTAINS(result.failure->description, "boom-exception");
  }
  {
    const auto result = rc::checkIsolated(
        constGen(0),
        [](const int &) -> bool {
          throw 42; // unknown object
        },
        IsolatedParams());
    CHECK(result.type == ResultType<int>::Failure);
    CHECK_CONTAINS(result.failure->description, "Unknown object thrown");
  }
  {
    // void-returning callback: success means "no exceptions thrown"
    IsolatedParams params;
    params.maxSuccess = 3;
    const auto result = rc::checkIsolated(
        constGen(0), [](const int &) { g_sentinel = -1; }, params);
    CHECK(result.type == ResultType<int>::Success);
    CHECK(result.numSuccess == 3);
  }
}

void testTagDistribution() {
  IsolatedParams params;
  params.maxSuccess = 7;
  const auto result = rc::checkIsolated(
      constGen(1),
      [](const int &) {
        RC_CLASSIFY(true, "always");
        return true;
      },
      params);
  CHECK(result.type == ResultType<int>::Success);
  const auto it =
      result.distribution.find(std::vector<std::string>{"\"always\""});
  CHECK(it != result.distribution.end());
  CHECK(it->second == 7);
}

void testLargeLog() {
  std::string bigLog = "BIGLOG_START";
  bigLog += std::string(256 * 1024, 'x'); // far beyond any pipe capacity
  bigLog += "BIGLOG_END";

  const auto result = rc::checkIsolated(
      constGen(0),
      [&](const int &) {
        RC_LOG() << bigLog;
        RC_ASSERT(false);
      },
      IsolatedParams());

  CHECK(result.type == ResultType<int>::Failure);
  const auto &failure = *result.failure;
  CHECK(failure.description.size() > 256 * 1024);
  CHECK_CONTAINS(failure.description, "BIGLOG_START");
  CHECK_CONTAINS(failure.description, "BIGLOG_END");
}

void testRunErrors() {
  {
    // Child exits with code 0 but never sends a result message.
    const auto result = rc::checkIsolated(
        constGen(0), [](const int &) { _exit(0); }, IsolatedParams());
    CHECK(result.type == ResultType<int>::Error);
    CHECK_CONTAINS(result.description, "without sending a result message");
  }
  {
    // Child exits with a non-zero code.
    const auto result = rc::checkIsolated(
        constGen(0), [](const int &) { _exit(3); }, IsolatedParams());
    CHECK(result.type == ResultType<int>::Error);
    CHECK_CONTAINS(result.description, "code 3");
  }
  {
    // Result message exceeds the configured maximum size.
    IsolatedParams params;
    params.maxMessageSize = 4096;
    const auto result = rc::checkIsolated(
        constGen(0),
        [](const int &) {
          RC_LOG() << std::string(8192, 'm');
          RC_ASSERT(false);
        },
        params);
    CHECK(result.type == ResultType<int>::Error);
    CHECK_CONTAINS(result.description, "exceeds the maximum message size");
  }
}

/// Runs `fn` in a sacrificial subprocess and requires it to report success
/// by exiting with code 0. Used to exercise resource-exhaustion run errors
/// without endangering the main test process.
void expectInSubprocess(const char *name, int (*fn)()) {
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    _exit(fn() == 0 ? 0 : 1);
  }
  const int status = waitFor(pid);
  if (!(WIFEXITED(status) && (WEXITSTATUS(status) == 0))) {
    std::fprintf(stderr, "subprocess check failed: %s\n", name);
    std::exit(1);
  }
}

int subprocessPipeFailure() {
  // Exhaust the file descriptor limit so that pipe() fails.
  struct rlimit noFds;
  noFds.rlim_cur = 3; // stdin/stdout/stderr only
  noFds.rlim_max = 3;
  if (setrlimit(RLIMIT_NOFILE, &noFds) != 0) {
    return 1;
  }
  const auto attempt = rc::detail::runIsolatedAttempt(
      [] { return std::string(); }, 1024, true);
  return (attempt.kind == rc::detail::IsolatedAttempt::Kind::Error) &&
          (attempt.errorDescription.find("pipe") != std::string::npos)
      ? 0
      : 2;
}

int subprocessForkFailure() {
  // Disallow new processes so that fork() fails.
  struct rlimit noProcs;
  noProcs.rlim_cur = 0;
  noProcs.rlim_max = 0;
  if (setrlimit(RLIMIT_NPROC, &noProcs) != 0) {
    return 1;
  }
  const auto attempt = rc::detail::runIsolatedAttempt(
      [] { return std::string(); }, 1024, true);
  return (attempt.kind == rc::detail::IsolatedAttempt::Kind::Error) &&
          (attempt.errorDescription.find("fork") != std::string::npos)
      ? 0
      : 2;
}

void testRunErrorsResourceExhaustion() {
  expectInSubprocess("pipe failure", &subprocessPipeFailure);
  expectInSubprocess("fork failure", &subprocessForkFailure);
}

void testNoDuplicateFlush() {
  // A crashing child (abort() may flush inherited stdio streams) must not
  // re-flush the parent's pending output buffers. Redirect stdout to a
  // pipe, leave a buffered line pending, run a crashing check, and count.
  int fds[2];
  CHECK(pipe(fds) == 0);
  const int savedStdout = dup(STDOUT_FILENO);
  CHECK(savedStdout >= 0);
  CHECK(dup2(fds[1], STDOUT_FILENO) == STDOUT_FILENO);
  close(fds[1]);

  std::cout << "FLUSH_SENTINEL\n"; // stays buffered: stdout is now a pipe
  const auto result = rc::checkIsolated(
      constGen(0),
      [](const int &) {
        std::abort(); // abort may flush inherited stdio buffers
      },
      IsolatedParams());
  std::cout.flush();
  std::fflush(stdout);
  CHECK(dup2(savedStdout, STDOUT_FILENO) == STDOUT_FILENO);
  close(savedStdout);

  CHECK(result.type == ResultType<int>::Failure);
  CHECK(result.failure->failureClass == IsolatedFailureClass::Crash);
  CHECK(result.failure->signalNumber == SIGABRT);

  std::string captured;
  char buffer[4096];
  for (;;) {
    const ssize_t n = read(fds[0], buffer, sizeof(buffer));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (n == 0) {
      break;
    }
    captured.append(buffer, static_cast<std::size_t>(n));
  }
  close(fds[0]);

  int occurrences = 0;
  const std::string sentinel = "FLUSH_SENTINEL";
  for (std::size_t pos = captured.find(sentinel); pos != std::string::npos;
       pos = captured.find(sentinel, pos + 1)) {
    occurrences++;
  }
  CHECK(occurrences == 1); // flushed exactly once, by the parent
}

void testResourceCleanup() {
  const int fdsBefore = countOpenFds();
  CHECK(fdsBefore > 0);

  for (int i = 0; i < 4; i++) {
    IsolatedParams params;
    params.maxSuccess = 3;
    const auto ok = rc::checkIsolated(
        constGen(1), [](const int &) { return true; }, params);
    CHECK(ok.type == ResultType<int>::Success);

    const auto crash = rc::checkIsolated(
        constGen(0),
        [](const int &) {
          std::raise(SIGABRT);
          _exit(97);
        },
        params);
    CHECK(crash.type == ResultType<int>::Failure);
    CHECK(crash.failure->failureClass == IsolatedFailureClass::Crash);
    CHECK(crash.failure->signalNumber == SIGABRT);

    const auto error = rc::checkIsolated(
        constGen(0), [](const int &) { _exit(0); }, params);
    CHECK(error.type == ResultType<int>::Error);
  }

  const int fdsAfter = countOpenFds();
  CHECK(fdsBefore == fdsAfter); // no leaked pipe ends

  // No unreaped children left behind.
  errno = 0;
  const pid_t zombie = waitpid(-1, nullptr, WNOHANG);
  CHECK((zombie == -1) && (errno == ECHILD));
}

// ---------------------------------------------------------------------------
// Shrink categories: crashes keep their signal, nothing else is accepted
// ---------------------------------------------------------------------------

/// Generator of 8 whose shrink tree contains every smaller non-negative
/// value as an immediate shrink.
Gen<int> signalTreeGen() {
  return Gen<int>([](const rc::Random &, int) {
    return rc::shrinkable::shrinkRecur(8, [](int x) {
      std::vector<int> shrinks;
      for (int i = x - 1; i >= 0; i--) {
        shrinks.push_back(i);
      }
      return rc::seq::fromContainer(std::move(shrinks));
    });
  });
}

/// Behavior by value: 8..5 -> SIGABRT, 4 -> SIGSEGV, 3 -> false,
/// 2..1 -> SIGABRT, 0 -> success.
bool signalTreeProperty(const int &x) {
  if (x == 4) {
    std::raise(SIGSEGV);
    _exit(97);
  }
  if (x == 3) {
    return false;
  }
  if ((x >= 5) || (x == 1) || (x == 2)) {
    std::raise(SIGABRT);
    _exit(97);
  }
  return true;
}

void testShrinkKeepsFailureClass() {
  std::vector<IsolatedAttemptInfo> attempts;
  IsolatedParams params;
  params.observer = [&](const IsolatedAttemptInfo &info) {
    attempts.push_back(info);
  };

  const auto result =
      rc::checkIsolated(signalTreeGen(), &signalTreeProperty, params);

  CHECK(result.type == ResultType<int>::Failure);
  const auto &failure = *result.failure;
  CHECK(failure.failureClass == IsolatedFailureClass::Crash);
  CHECK(failure.signalNumber == SIGABRT);
  CHECK(failure.originalInput == 8);
  // The walk: 8->7->6->5 accepted, 4 (SIGSEGV) rejected, 3 (false)
  // rejected, 2 accepted, 1 accepted, 0 (success) rejected.
  CHECK(failure.finalInput == 1);
  CHECK(failure.shrinkPath == (std::vector<std::size_t>{0, 0, 0, 2, 0}));
  CHECK_CONTAINS(failure.description, "SIGABRT");
  CHECK(failure.replay.failureClass == IsolatedFailureClass::Crash);
  CHECK(failure.replay.signalNumber == SIGABRT);

  int aborts = 0, segvs = 0, falses = 0, successes = 0, segvAccepted = 0;
  for (const auto &attempt : attempts) {
    switch (attempt.outcome) {
    case IsolatedAttemptInfo::Outcome::Signalled:
      if (attempt.signalNumber == SIGABRT) {
        aborts++;
      } else if (attempt.signalNumber == SIGSEGV) {
        segvs++;
        if (attempt.accepted) {
          segvAccepted++;
        }
      }
      break;
    case IsolatedAttemptInfo::Outcome::Failure:
      falses++;
      break;
    case IsolatedAttemptInfo::Outcome::Success:
      successes++;
      break;
    default:
      break;
    }
  }
  CHECK(aborts == 6);      // 8, 7, 6, 5, 2, 1
  CHECK(segvs == 1);       // 4
  CHECK(segvAccepted == 0); // a different signal never replaces the crash
  CHECK(falses == 1);      // 3
  CHECK(successes == 1);   // 0
}

void testShrinkDiscardDoesNotReplaceCrash() {
  // Tree: 2 -> {1, 0}. Property: 2 crashes, 1 is a precondition discard,
  // 0 crashes with the same signal. The discard must not be accepted, and
  // the walk must continue past it to the smaller same-signal crash.
  Gen<int> gen = Gen<int>([](const rc::Random &, int) {
    return rc::shrinkable::shrinkRecur(2, [](int x) {
      std::vector<int> shrinks;
      for (int i = x - 1; i >= 0; i--) {
        shrinks.push_back(i);
      }
      return rc::seq::fromContainer(std::move(shrinks));
    });
  });

  std::vector<IsolatedAttemptInfo> attempts;
  IsolatedParams params;
  params.observer = [&](const IsolatedAttemptInfo &info) {
    attempts.push_back(info);
  };

  const auto result = rc::checkIsolated(
      gen,
      [](const int &x) {
        if (x == 1) {
          RC_PRE(false); // discarded candidate: never a crash replacement
        }
        std::raise(SIGABRT);
        _exit(97);
      },
      params);

  CHECK(result.type == ResultType<int>::Failure);
  const auto &failure = *result.failure;
  CHECK(failure.failureClass == IsolatedFailureClass::Crash);
  CHECK(failure.signalNumber == SIGABRT);
  CHECK(failure.originalInput == 2);
  CHECK(failure.finalInput == 0);
  CHECK(failure.shrinkPath == (std::vector<std::size_t>{1}));

  int discards = 0;
  for (const auto &attempt : attempts) {
    if (attempt.outcome == IsolatedAttemptInfo::Outcome::Discard) {
      discards++;
      CHECK(!attempt.accepted);
    }
  }
  CHECK(discards == 1);
}

void testShrinkFreshProcessesAndSentinel() {
  std::vector<IsolatedAttemptInfo> attempts;
  IsolatedParams params;
  params.observer = [&](const IsolatedAttemptInfo &info) {
    attempts.push_back(info);
  };

  g_childMarker = 0;
  const auto result = rc::checkIsolated(
      chainGen(8),
      [](const int &x) {
        if (g_childMarker != 0) {
          // A reused process would see the previous callback's marker and
          // start failing cases that should succeed, corrupting the shrink.
          return false;
        }
        g_childMarker = 1;
        g_sentinel = -1; // clobbers only the child's copy
        return x < 3;
      },
      params);

  CHECK(result.type == ResultType<int>::Failure);
  // Minimum is 3, not 0: every attempt ran in a fresh process image.
  CHECK(result.failure->finalInput == 3);
  CHECK(result.failure->shrinkPath ==
        (std::vector<std::size_t>{0, 0, 0, 0, 0}));
  CHECK(g_childMarker == 0); // parent state untouched by callbacks
  const pid_t parent = getpid();
  for (const auto &attempt : attempts) {
    CHECK(attempt.childPid != parent);
  }
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

rc::IsolatedReplay makeAssertionRecord(std::vector<IsolatedAttemptInfo> *out) {
  IsolatedParams params;
  params.observer = [&](const IsolatedAttemptInfo &info) {
    out->push_back(info);
  };
  const auto result = rc::checkIsolated(
      chainGen(8),
      [](const int &x) {
        RC_LOG() << "exec_pid=" << ::getpid();
        RC_ASSERT(x < 3);
      },
      params);
  CHECK(result.type == ResultType<int>::Failure);
  return result.failure->replay;
}

void testReplayAssertion() {
  std::vector<IsolatedAttemptInfo> attempts;
  const auto record = makeAssertionRecord(&attempts);
  CHECK(record.failureClass == IsolatedFailureClass::Assertion);
  CHECK(record.shrinkPath == (std::vector<std::size_t>{0, 0, 0, 0, 0}));

  // The record is a copyable, printable string.
  const std::string text = rc::serializeIsolatedReplay(record);
  CHECK(text.compare(0, 7, "RCISO1/") == 0);
  const auto parsed = rc::deserializeIsolatedReplay(text);
  CHECK(parsed);
  CHECK(*parsed == record);

  const auto replay = rc::replayIsolated(
      chainGen(8),
      [](const int &x) {
        RC_LOG() << "exec_pid=" << ::getpid();
        RC_ASSERT(x < 3);
      },
      *parsed);

  CHECK(replay.type == IsolatedReplayResult<int>::Type::Matched);
  const auto &failure = *replay.failure;
  CHECK(failure.failureClass == IsolatedFailureClass::Assertion);
  CHECK(failure.finalInput == 3);
  CHECK(failure.originalInput == 3);
  CHECK_CONTAINS(failure.description, "RC_ASSERT(x < 3)");
  // Logs belong to the replay execution itself: a fresh child pid.
  const std::string key = "exec_pid=";
  const auto pos = failure.description.find(key);
  CHECK(pos != std::string::npos);
  const int loggedPid = std::stoi(failure.description.substr(pos + key.size()));
  CHECK(loggedPid > 0);
  CHECK(loggedPid != getpid());
}

void testReplayMismatchSuccess() {
  std::vector<IsolatedAttemptInfo> attempts;
  const auto record = makeAssertionRecord(&attempts);

  // The recorded input (3) now satisfies the property.
  const auto replay = rc::replayIsolated(
      chainGen(8), [](const int &x) { return x < 5; }, record);
  CHECK(replay.type == IsolatedReplayResult<int>::Type::Mismatch);
  CHECK(!replay.failure); // the old failure is not reused
  CHECK(replay.finalInput);
  CHECK(*replay.finalInput == 3);
  CHECK_CONTAINS(replay.actualDescription, "succeeded");
  CHECK(!replay.expectedDescription.empty());
}

void testReplayMismatchPath() {
  std::vector<IsolatedAttemptInfo> attempts;
  auto record = makeAssertionRecord(&attempts);
  record.shrinkPath.push_back(99); // no such shrink

  const auto replay = rc::replayIsolated(
      chainGen(8), [](const int &x) { return x < 3; }, record);
  CHECK(replay.type == IsolatedReplayResult<int>::Type::Mismatch);
  CHECK(!replay.failure);
  CHECK(!replay.finalInput);
  CHECK_CONTAINS(replay.actualDescription, "path");
}

void testReplayCrashAndMismatches() {
  // Produce a crash record with the signal tree.
  const auto crashCheck =
      rc::checkIsolated(signalTreeGen(), &signalTreeProperty, IsolatedParams());
  CHECK(crashCheck.type == ResultType<int>::Failure);
  const auto record = crashCheck.failure->replay;
  CHECK(record.failureClass == IsolatedFailureClass::Crash);
  CHECK(record.signalNumber == SIGABRT);

  // Same property: the crash reproduces with the same signal.
  const auto matched =
      rc::replayIsolated(signalTreeGen(), &signalTreeProperty, record);
  CHECK(matched.type == IsolatedReplayResult<int>::Type::Matched);
  CHECK(matched.failure->failureClass == IsolatedFailureClass::Crash);
  CHECK(matched.failure->signalNumber == SIGABRT);
  CHECK(matched.failure->finalInput == 1);

  // A different signal is a different failure: mismatch, and the recorded
  // crash must not be reported as reproduced.
  const auto otherSignal = rc::replayIsolated(
      signalTreeGen(),
      [](const int &) -> bool {
        std::raise(SIGSEGV);
        _exit(97);
      },
      record);
  CHECK(otherSignal.type == IsolatedReplayResult<int>::Type::Mismatch);
  CHECK(!otherSignal.failure);
  CHECK_CONTAINS(otherSignal.actualDescription, "SIGSEGV");
  CHECK_CONTAINS(otherSignal.expectedDescription, "SIGABRT");

  // A plain false is a different category: mismatch as well.
  const auto plainFalse = rc::replayIsolated(
      signalTreeGen(), [](const int &) { return false; }, record);
  CHECK(plainFalse.type == IsolatedReplayResult<int>::Type::Mismatch);
  CHECK(!plainFalse.failure);

  // And the assertion record does not match a property that crashes on the
  // recorded input either (assertion vs crash are different categories).
  std::vector<IsolatedAttemptInfo> attempts;
  const auto assertionRecord = makeAssertionRecord(&attempts);
  const auto wrongCategory = rc::replayIsolated(
      chainGen(8),
      [](const int &x) -> bool {
        if (x == 3) {
          std::raise(SIGABRT);
          _exit(97);
        }
        return x < 3;
      },
      assertionRecord);
  CHECK(wrongCategory.type == IsolatedReplayResult<int>::Type::Mismatch);
  CHECK(!wrongCategory.failure);
}

void testReplayRecordParsing() {
  CHECK(!rc::deserializeIsolatedReplay(""));
  CHECK(!rc::deserializeIsolatedReplay("garbage"));
  CHECK(!rc::deserializeIsolatedReplay("RCISO1/"));
  CHECK(!rc::deserializeIsolatedReplay("RCISO1/zz"));
  CHECK(!rc::deserializeIsolatedReplay("RCISO1/010"));

  std::vector<IsolatedAttemptInfo> attempts;
  const auto record = makeAssertionRecord(&attempts);
  const std::string text = rc::serializeIsolatedReplay(record);

  // Truncated records are rejected.
  CHECK(!rc::deserializeIsolatedReplay(text.substr(0, text.size() - 2))
             );
  // Unsupported version is rejected (version is the first little-endian
  // uint32 after the prefix).
  std::string otherVersion = text;
  otherVersion.replace(7, 8, "02000000");
  CHECK(!rc::deserializeIsolatedReplay(otherVersion));
  // Trailing garbage is rejected.
  CHECK(!rc::deserializeIsolatedReplay(text + "00"));
}

// ---------------------------------------------------------------------------
// TLV parser material: bounded-length parser boundary defect
// ---------------------------------------------------------------------------

struct TlvNode {
  std::uint8_t type = 0; // container if the high bit is set
  bool lengthOverride = false;
  std::uint16_t overrideLength = 0;
  std::vector<std::uint8_t> payload; // leaf nodes
  std::vector<TlvNode> children;     // container nodes
};

struct TlvDoc {
  std::vector<TlvNode> nodes; // top-level forest
};

void serializeNode(const TlvNode &node, std::vector<std::uint8_t> &out);

void serializeNodeBody(const TlvNode &node, std::vector<std::uint8_t> &out) {
  if (tlv::isContainer(node.type)) {
    for (const auto &child : node.children) {
      serializeNode(child, out);
    }
  } else {
    out.insert(out.end(), node.payload.begin(), node.payload.end());
  }
}

std::size_t bodySize(const TlvNode &node) {
  std::vector<std::uint8_t> body;
  serializeNodeBody(node, body);
  return body.size();
}

void serializeNode(const TlvNode &node, std::vector<std::uint8_t> &out) {
  std::vector<std::uint8_t> body;
  serializeNodeBody(node, body);
  // Length fields are recomputed from the actual tree at serialization
  // time, except for nodes carrying an explicit override.
  const std::uint16_t declared = node.lengthOverride
      ? node.overrideLength
      : static_cast<std::uint16_t>(body.size());
  out.push_back(node.type);
  out.push_back(static_cast<std::uint8_t>(declared & 0xFF));
  out.push_back(static_cast<std::uint8_t>((declared >> 8) & 0xFF));
  out.insert(out.end(), body.begin(), body.end());
}

std::vector<std::uint8_t> serializeDoc(const TlvDoc &doc) {
  std::vector<std::uint8_t> out;
  for (const auto &node : doc.nodes) {
    serializeNode(node, out);
  }
  return out;
}

std::uint64_t genBelow(rc::Random &r, std::uint64_t n) {
  return static_cast<std::uint64_t>(r.next()) % n;
}

TlvNode genTlvNode(rc::Random &r, int depth) {
  TlvNode node;
  const bool container = (depth < 3) && (genBelow(r, 2) == 0);
  node.type = container ? static_cast<std::uint8_t>(0x80 | genBelow(r, 4))
                        : static_cast<std::uint8_t>(genBelow(r, 0x80));
  if (container) {
    const auto count = genBelow(r, 4);
    for (std::uint64_t i = 0; i < count; i++) {
      node.children.push_back(genTlvNode(r, depth + 1));
    }
  } else {
    const auto count = genBelow(r, 7);
    for (std::uint64_t i = 0; i < count; i++) {
      node.payload.push_back(static_cast<std::uint8_t>(genBelow(r, 256)));
    }
  }
  if (genBelow(r, 4) == 0) {
    node.lengthOverride = true;
    node.overrideLength = static_cast<std::uint16_t>(genBelow(r, 65536));
  }
  return node;
}

TlvDoc genTlvDoc(rc::Random &r) {
  TlvDoc doc;
  const auto count = 1 + genBelow(r, 3);
  for (std::uint64_t i = 0; i < count; i++) {
    doc.nodes.push_back(genTlvNode(r, 0));
  }
  return doc;
}

std::vector<TlvNode> shrinkTlvNode(const TlvNode &node) {
  std::vector<TlvNode> out;
  if (node.lengthOverride) {
    const std::size_t computed = bodySize(node);
    if (node.overrideLength > computed) {
      // Shrink the declared length towards the recomputed correct length.
      TlvNode mid = node;
      mid.overrideLength = static_cast<std::uint16_t>(
          computed + (node.overrideLength - computed) / 2);
      out.push_back(mid);
      TlvNode exact = node;
      exact.overrideLength = static_cast<std::uint16_t>(computed);
      out.push_back(exact);
    }
  }
  if (tlv::isContainer(node.type)) {
    for (std::size_t j = 0; j < node.children.size(); j++) {
      TlvNode removed = node;
      removed.children.erase(removed.children.begin() +
                             static_cast<std::ptrdiff_t>(j));
      out.push_back(removed);
      for (const auto &sub : shrinkTlvNode(node.children[j])) {
        TlvNode replaced = node;
        replaced.children[j] = sub;
        out.push_back(replaced);
      }
    }
  } else if (!node.payload.empty()) {
    TlvNode empty = node;
    empty.payload.clear();
    out.push_back(empty);
    if (node.payload.size() > 1) {
      TlvNode half = node;
      half.payload.resize(node.payload.size() / 2);
      out.push_back(half);
    }
  }
  return out;
}

// Shrinking removes or simplifies subtrees. Since every non-override length
// field is recomputed from the tree at serialization time, the lengths of
// containers unrelated to the violation are recomputed automatically while
// the override node's declared length (the defect site) is preserved.
rc::Seq<TlvDoc> shrinkTlvDoc(const TlvDoc &doc) {
  std::vector<TlvDoc> out;
  for (std::size_t i = 0; i < doc.nodes.size(); i++) {
    TlvDoc removed = doc;
    removed.nodes.erase(removed.nodes.begin() +
                        static_cast<std::ptrdiff_t>(i));
    out.push_back(removed);
    for (const auto &sub : shrinkTlvNode(doc.nodes[i])) {
      TlvDoc replaced = doc;
      replaced.nodes[i] = sub;
      out.push_back(replaced);
    }
  }
  return rc::seq::fromContainer(std::move(out));
}

Gen<TlvDoc> tlvGen() {
  return Gen<TlvDoc>([](const rc::Random &random, int size) {
    (void)size;
    rc::Random r = random;
    TlvDoc doc = genTlvDoc(r);
    return rc::shrinkable::shrinkRecur(std::move(doc), &shrinkTlvDoc);
  });
}

/// The property under test: parse the serialized document with the legacy
/// parser version. Any return from parseV1 (clean parse or ordinary format
/// error) is a success; only the bounds-guard abort is a failure. Random
/// generation lives entirely in tlvGen; the callback dereferences no
/// generator.
bool tlvProperty(const TlvDoc &doc) {
  const std::vector<std::uint8_t> bytes = serializeDoc(doc);
  tlv::parseV1(bytes); // legacy version: aborts on boundary violation
  return true;
}

std::string hexOf(const std::vector<std::uint8_t> &bytes) {
  static const char *const digits = "0123456789abcdef";
  std::string out;
  for (const auto b : bytes) {
    out.push_back(digits[(b >> 4) & 0xF]);
    out.push_back(digits[b & 0xF]);
  }
  return out.empty() ? std::string("(empty)") : out;
}

std::vector<std::vector<std::uint8_t>> legalSamples() {
  std::vector<std::vector<std::uint8_t>> samples;
  samples.push_back({}); // empty document
  // two top-level leaves
  samples.push_back({0x01, 0x02, 0x00, 0xAA, 0xBB, 0x02, 0x00, 0x00});
  // nested: container{ leaf "ab", container{ leaf {} } }
  samples.push_back({0x80, 0x0B, 0x00, 0x01, 0x02, 0x00, 0x61, 0x62,
                     0x81, 0x03, 0x00, 0x02, 0x00, 0x00});
  return samples;
}

void testTlvLegalSamples() {
  // A nested document built through the model must serialize to the
  // hand-computed legal sample and be accepted by both parser versions.
  TlvNode innerLeaf;
  innerLeaf.type = 0x02;
  TlvNode inner;
  inner.type = 0x81;
  inner.children.push_back(innerLeaf);
  TlvNode leaf;
  leaf.type = 0x01;
  leaf.payload = {0x61, 0x62};
  TlvNode outer;
  outer.type = 0x80;
  outer.children.push_back(leaf);
  outer.children.push_back(inner);
  TlvDoc doc;
  doc.nodes.push_back(outer);

  const auto samples = legalSamples();
  CHECK(serializeDoc(doc) == samples[2]);

  for (const auto &sample : samples) {
    CHECK(tlv::parseV1(sample).status == tlv::ParseReport::Status::Ok);
    CHECK(tlv::parseV2(sample).status == tlv::ParseReport::Status::Ok);
    CHECK(tlv::firstBoundaryViolation(sample) == tlv::kNoViolation);
  }
  std::cout << "TLV_LEGAL_SAMPLES_OK=" << samples.size() << "\n";
}

void testTlv() {
  testTlvLegalSamples();

  IsolatedParams params;
  params.seed = 20261115;
  params.maxSuccess = 50;

  const auto result = rc::checkIsolated(tlvGen(), &tlvProperty, params);
  CHECK(result.type == ResultType<TlvDoc>::Failure);
  const auto &failure = *result.failure;
  CHECK(failure.failureClass == IsolatedFailureClass::Crash);
  CHECK(failure.signalNumber == SIGABRT);

  const auto originalBytes = serializeDoc(failure.originalInput);
  const auto finalBytes = serializeDoc(failure.finalInput);
  CHECK(finalBytes.size() <= originalBytes.size());

  // The independent checker pinpoints the first boundary crossing, and the
  // fixed version reports a format error at the very same position.
  const auto violation = tlv::firstBoundaryViolation(finalBytes);
  CHECK(violation != tlv::kNoViolation);
  const auto fixed = tlv::parseV2(finalBytes);
  CHECK(fixed.status == tlv::ParseReport::Status::FormatError);
  CHECK(fixed.offset == violation);
  CHECK_CONTAINS(fixed.message, "exceeds parent container");
  CHECK(tlv::firstBoundaryViolation(originalBytes) != tlv::kNoViolation);

  // The replay record regenerates the same final input and reproduces the
  // same SIGABRT at the same position in a new child process.
  const std::string recordText = rc::serializeIsolatedReplay(failure.replay);
  const auto record = rc::deserializeIsolatedReplay(recordText);
  CHECK(record);
  const auto replay = rc::replayIsolated(tlvGen(), &tlvProperty, *record);
  CHECK(replay.type == IsolatedReplayResult<TlvDoc>::Type::Matched);
  CHECK(replay.failure->failureClass == IsolatedFailureClass::Crash);
  CHECK(replay.failure->signalNumber == SIGABRT);
  CHECK(serializeDoc(replay.failure->finalInput) == finalBytes);
  CHECK(tlv::firstBoundaryViolation(
            serializeDoc(replay.failure->finalInput)) == violation);

  std::cout << "TLV_ORIGINAL_BYTES=" << hexOf(originalBytes) << "\n";
  std::cout << "TLV_FINAL_BYTES=" << hexOf(finalBytes) << "\n";
  std::cout << "TLV_BOUNDARY_OFFSET=" << violation << "\n";
  std::cout << "TLV_V2_ERROR_OFFSET=" << fixed.offset << "\n";
  std::cout << "TLV_REPLAY=matched signal=SIGABRT\n";
  std::cout << "TLV_FIXED_REJECTED=1\n";
}

} // namespace

int main() {
  // Framing channel under controlled communication.
  testChannelSegmented();
  testChannelTruncated();
  testChannelNoMessage();
  testChannelTooLarge();
  testChannelPartialWrites();
  testInterruptedCheck();

  // Basic result categories.
  testSuccessAndProcessIdentity();
  testAssertionFailureIdentityAndTags();
  testDiscardGaveUp();
  testExceptionsAndVoid();
  testTagDistribution();
  testLargeLog();
  testRunErrors();
  testRunErrorsResourceExhaustion();
  testNoDuplicateFlush();
  testResourceCleanup();
  std::cout << "ISOLATED_RESULTS_OK" << std::endl;

  // Shrink categories and process freshness.
  testShrinkKeepsFailureClass();
  testShrinkDiscardDoesNotReplaceCrash();
  testShrinkFreshProcessesAndSentinel();
  std::cout << "ISOLATED_SHRINK_OK" << std::endl;

  // Replay records.
  testReplayAssertion();
  testReplayMismatchSuccess();
  testReplayMismatchPath();
  testReplayCrashAndMismatches();
  testReplayRecordParsing();
  std::cout << "ISOLATED_REPLAY_OK" << std::endl;

  // Structured fuzzing of the TLV parser material.
  testTlv();
  std::cout << "TLV_BOUNDARY_OK" << std::endl;

  // Across the whole suite, no callback ever ran in this process.
  CHECK(g_sentinel == 0x5E17);
  return 0;
}
