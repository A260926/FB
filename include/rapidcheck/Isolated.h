#pragma once

// Isolated property checking: run every test case callback in a freshly
// forked child process so that native crashes (SIGABRT, SIGSEGV, ...) are
// reported as structured results instead of taking down the test runner.
//
// This backend requires POSIX facilities (fork, waitpid, pipes) and is only
// available on Linux and macOS. The rest of RapidCheck is unaffected and the
// regular `rc::check` entry points keep their existing behavior and reproduce
// strings.
#if !defined(_WIN32)

// Expose POSIX declarations (pid_t, fork, waitpid, ...) even when this
// header is included first in a translation unit built in strict -std=c++11
// mode. Purely additive; does not hide anything.
#if !defined(__APPLE__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <sys/types.h> // pid_t

#include "rapidcheck/Gen.h"
#include "rapidcheck/Maybe.h"
#include "rapidcheck/Random.h"
#include "rapidcheck/detail/Results.h"

namespace rc {

/// Default upper bound for the size of a single result message (4 byte length
/// header plus payload) transferred from a child process to the parent. A
/// child whose result message would exceed this bound is reported as a run
/// error, never as a test outcome. The bound can be changed per check through
/// `IsolatedParams::maxMessageSize`.
constexpr std::size_t kIsolatedDefaultMaxMessageSize = 1024 * 1024; // 1 MiB

/// The category of a failure found by isolated checking. A native crash is a
/// different category from an ordinary property failure and the two are never
/// substituted for each other while shrinking.
enum class IsolatedFailureClass {
  /// The property reported failure through the normal RapidCheck channels:
  /// a `false` return, a failed assertion or a thrown C++ exception.
  Assertion,
  /// The child process running the callback was terminated by a native
  /// signal. The signal number is available separately.
  Crash,
};

/// Information about a single isolated attempt, reported to the observer (if
/// any) from the parent process after the attempt's child has been reaped.
struct IsolatedAttemptInfo {
  enum class Outcome { Success, Failure, Discard, Signalled, Error };

  /// The pid of the child process the callback ran in. Never the parent's
  /// own pid; the callback itself never runs in the parent process.
  pid_t childPid = -1;
  Outcome outcome = Outcome::Error;
  /// Valid when `outcome == Outcome::Signalled`.
  int signalNumber = 0;
  /// Whether this attempt happened while shrinking (as opposed to searching).
  bool shrinkPhase = false;
  /// For shrink phase attempts, whether the candidate was accepted.
  bool accepted = false;
};

/// Optional hook invoked in the parent process after every isolated attempt.
using IsolatedObserver = std::function<void(const IsolatedAttemptInfo &)>;

/// Parameters for isolated checking. Mirrors `detail::TestParams` and adds
/// isolation specific knobs.
struct IsolatedParams {
  /// The seed to use for generation.
  uint64_t seed = 0;
  /// The maximum number of successes before deciding a property passes.
  int maxSuccess = 100;
  /// The maximum size to generate.
  int maxSize = 100;
  /// The maximum allowed number of discarded tests per successful test.
  int maxDiscardRatio = 10;
  /// Whether shrinking should be disabled or not.
  bool disableShrinking = false;
  /// Maximum size in bytes of one child-to-parent result message, including
  /// its 4 byte length header. Larger messages are run errors.
  std::size_t maxMessageSize = kIsolatedDefaultMaxMessageSize;
  /// Whether to disable core dumps (RLIMIT_CORE=0) in the forked children.
  /// The crash is reported structurally either way; disabling core dumps
  /// keeps repeated crash testing fast and leaves no core files behind.
  bool disableCoreDumps = true;
  /// Optional observer invoked in the parent after every attempt.
  IsolatedObserver observer;
};

/// Everything needed to reproduce the final input of an isolated check in a
/// new child process. The record does not contain the input value itself (and
/// never a printed form of it); the input is regenerated from `random` and
/// `size` using the original generator and then located with `shrinkPath`.
/// This record type and its serialized form are versioned independently of
/// the regular `detail::Reproduce` machinery, which is left unchanged.
struct IsolatedReplay {
  /// The Random state the failing shrinkable was generated with.
  Random random;
  /// The size the failing shrinkable was generated with.
  int size = 0;
  /// The accepted shrink path leading to the final input.
  std::vector<std::size_t> shrinkPath;
  /// The failure category of the original failure. A replay only matches if
  /// the re-run produces the same category (and, for crashes, signal).
  IsolatedFailureClass failureClass = IsolatedFailureClass::Assertion;
  /// The signal of the original crash. Only meaningful for Crash.
  int signalNumber = 0;
};

bool operator==(const IsolatedReplay &lhs, const IsolatedReplay &rhs);
bool operator!=(const IsolatedReplay &lhs, const IsolatedReplay &rhs);

/// Serializes an `IsolatedReplay` to a copyable, printable ASCII string. The
/// format is versioned ("RCISO1/...") and independent of the regular
/// RapidCheck reproduce strings.
std::string serializeIsolatedReplay(const IsolatedReplay &replay);

/// Parses a string produced by `serializeIsolatedReplay`. Returns Nothing if
/// the string is not a valid record of a supported version.
Maybe<IsolatedReplay> deserializeIsolatedReplay(const std::string &text);

/// Structured description of a failure found by isolated checking.
template <typename T>
struct IsolatedFailureInfo {
  /// The category of the first failure found. Shrinking only accepted
  /// candidates of this same category (and signal, for crashes).
  IsolatedFailureClass failureClass = IsolatedFailureClass::Assertion;
  /// The terminating signal. Only valid when `failureClass` is Crash.
  int signalNumber = 0;
  /// A description of the failure: the assertion/exception message for
  /// Assertion (including anything logged through RC_LOG) or a description
  /// of the terminating signal for Crash. Always originates from the
  /// execution that was actually selected as the minimal one.
  std::string description;
  /// Tags reported by the selected execution (empty for crashes).
  std::vector<std::string> tags;
  /// The originally generated failing input.
  T originalInput;
  /// The final (minimized) failing input.
  T finalInput;
  /// The accepted shrink path from the original to the final input.
  std::vector<std::size_t> shrinkPath;
  /// A record that regenerates `finalInput` and re-runs it in a new child.
  IsolatedReplay replay;
  /// Number of shrink candidates that could not be evaluated because of a
  /// run error. Such candidates are rejected, never accepted.
  int numShrinkErrors = 0;
};

/// The structured result of an isolated check.
template <typename T>
struct IsolatedCheckResult {
  enum class Type {
    /// The property held for all generated cases.
    Success,
    /// A failing input was found (and, unless disabled, shrunk).
    Failure,
    /// Too many cases were discarded.
    GaveUp,
    /// The checking process itself failed (fork/pipe/read/write/wait
    /// failures, missing or malformed result messages, unexpected child
    /// exit codes). Never produced by an ordinary test outcome.
    Error,
  };

  Type type = Type::Success;
  int numSuccess = 0;
  int numDiscarded = 0;
  /// Distribution of tags of successful cases.
  detail::Distribution distribution;
  /// Present iff `type == Type::Failure`.
  Maybe<IsolatedFailureInfo<T>> failure;
  /// For GaveUp: the last discard description. For Error: what went wrong.
  std::string description;
};

/// The result of re-running an `IsolatedReplay` record.
template <typename T>
struct IsolatedReplayResult {
  enum class Type {
    /// The regenerated input reproduced the recorded failure category (and
    /// signal, for crashes).
    Matched,
    /// The record could not be replayed as recorded: the shrink path does
    /// not exist, the case now succeeds, is discarded, or fails with a
    /// different category or signal. The recorded failure is NOT reported
    /// as if it had reproduced; see `expectedDescription` and
    /// `actualDescription`.
    Mismatch,
    /// A run error occurred while replaying.
    Error,
  };

  Type type = Type::Mismatch;
  /// The regenerated final input, if the shrink path exists.
  Maybe<T> finalInput;
  /// Present iff `type == Type::Matched`. All logs and state come from the
  /// replay execution itself.
  Maybe<IsolatedFailureInfo<T>> failure;
  /// For Mismatch: what the record expected.
  std::string expectedDescription;
  /// For Mismatch: what actually happened.
  std::string actualDescription;
  /// For Error: what went wrong.
  std::string errorDescription;
};

/// Checks the given property callback in isolation. Every invocation of
/// `callable` happens in a freshly forked child process; generation and
/// shrink enumeration happen in the parent. `T` does not need to be
/// serializable: the generated value is inherited across `fork()`.
///
/// The callback receives the generated value as `const T &` and may return
/// `bool` or `void` and use the usual RapidCheck assertions, preconditions,
/// tags and logging; these keep their `PropertyAdapter` meaning. All random
/// generation must happen inside `gen`; the callback must not dereference
/// generators. The caller must enter this function single-threaded, and the
/// callback must terminate, must not create descendant processes and must
/// not depend on side effects outside its child process.
///
/// Exceptions thrown by generation or shrink enumeration in the parent
/// propagate to the caller.
template <typename T, typename Callable>
IsolatedCheckResult<T> checkIsolated(Gen<T> gen,
                                     Callable callable,
                                     IsolatedParams params = IsolatedParams());

/// Re-runs the final input of a previous isolated check in a new child
/// process. Only `params.maxMessageSize`, `params.disableCoreDumps` and
/// `params.observer` are used.
template <typename T, typename Callable>
IsolatedReplayResult<T>
replayIsolated(Gen<T> gen,
               Callable callable,
               const IsolatedReplay &replay,
               IsolatedParams params = IsolatedParams());

namespace detail {

/// The outcome of a single isolated attempt.
struct IsolatedAttempt {
  enum class Kind {
    /// The child exited normally with a complete, well-formed message.
    Completed,
    /// The child was terminated by a native signal.
    Signalled,
    /// A run error: fork/pipe/read/write/wait failure, missing, truncated
    /// or oversized message, or an unexpected child exit code.
    Error,
  };

  Kind kind = Kind::Error;
  /// The pid of the child that ran (or would have run) the callback.
  pid_t childPid = -1;
  /// Valid when `kind == Kind::Signalled`.
  int signalNumber = 0;
  /// Valid when `kind == Kind::Completed`.
  CaseResult caseResult;
  /// Valid when `kind == Kind::Completed`.
  Tags tags;
  /// Valid when `kind == Kind::Error`.
  std::string errorDescription;
};

/// Runs `childMessage` in a freshly forked child process and classifies the
/// outcome. `childMessage` is only ever invoked in the child; whatever string
/// it returns is sent to the parent as the result message. The parent reads
/// the message while the child is still running (so messages larger than the
/// pipe capacity work), then reaps the child. The parent's signal handlers
/// and file descriptors are left untouched and both pipe ends are closed
/// before returning. The parent's stdio/iostream buffers are flushed before
/// forking and the child exits with `_exit`, so pending parent output is
/// never flushed a second time by the child.
IsolatedAttempt runIsolatedAttempt(
    const std::function<std::string()> &childMessage,
    std::size_t maxMessageSize,
    bool disableCoreDumps);

/// Returns a stable symbolic name for common signals ("SIGABRT", ...),
/// falling back to "signal N".
std::string isolatedSignalName(int signalNumber);

/// Describes a child crash for reporting purposes.
std::string describeIsolatedCrash(int signalNumber);

/// Encodes a CaseResult plus tags as a result message payload.
std::string encodeIsolatedCaseMessage(const CaseResult &result,
                                      const Tags &tags);

/// Decodes a result message payload. Strict: returns false on any
/// structural inconsistency.
bool decodeIsolatedCaseMessage(const std::string &payload,
                               CaseResult &result,
                               Tags &tags);

/// Status of reading one framed message from a channel.
enum class IsolatedChannelStatus {
  Ok,
  /// EOF before a single byte: no message at all.
  NoMessage,
  /// Fewer than 4 header bytes, or the payload length did not match the
  /// declared length (including trailing garbage).
  Truncated,
  /// The message exceeded the maximum allowed size.
  TooLarge,
  /// An I/O error occurred while reading.
  IoError,
};

/// Reads one framed message (4 byte little-endian length + payload) from
/// `fd`, reading until EOF and retrying on EINTR. At most `maxMessageSize`
/// bytes (header included) are accepted; the channel is drained even beyond
/// that so a well-behaved writer is not blocked. On Ok, `messageOut`
/// receives the payload.
IsolatedChannelStatus readIsolatedChannel(int fd,
                                          std::size_t maxMessageSize,
                                          std::string &messageOut,
                                          std::string &errorOut);

/// Writes `payload` as one framed message to `fd`, retrying on EINTR and
/// handling partial writes. Returns false on I/O error.
bool writeIsolatedChannel(int fd,
                          const std::string &payload,
                          std::string &errorOut);

} // namespace detail
} // namespace rc

#include "Isolated.hpp"

#endif // !defined(_WIN32)
