#include "rapidcheck/Isolated.h"

#if !defined(_WIN32)

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rapidcheck/detail/Serialization.h"

namespace rc {
namespace {

/// Exit codes used by the framework child when it cannot deliver a proper
/// result message. All of them are reported as run errors by the parent.
enum ChildExitCode {
  /// The child action itself threw (outside of the PropertyAdapter catches).
  kChildExitActionThrew = 117,
  /// The result message would exceed the configured maximum size.
  kChildExitMessageTooLarge = 118,
  /// Writing the result message to the pipe failed.
  kChildExitWriteFailed = 119,
};

/// Version of the serialized IsolatedReplay format.
constexpr std::uint32_t kIsolatedReplayVersion = 1;
/// String prefix of serialized IsolatedReplay records.
const char *const kIsolatedReplayPrefix = "RCISO1/";

std::string errnoDescription(const char *what) {
  return std::string(what) + " failed: " + std::strerror(errno);
}

/// Resets all signal dispositions to their defaults in the child process so
/// that fatal signals actually terminate it with their native meaning,
/// regardless of handlers the caller may have installed in the parent. Only
/// the child's (throwaway) process image is affected; the caller's handlers
/// are never touched. SIGPIPE is then ignored so that a broken result pipe
/// surfaces as a write error instead of an accidental SIGPIPE death.
void resetChildSignalDispositions() {
#ifdef NSIG
  const int maxSignal = NSIG;
#else
  const int maxSignal = 64;
#endif
  for (int sig = 1; sig < maxSignal; sig++) {
    // SIGKILL/SIGSTOP cannot be reset; signal() simply fails for them.
    std::signal(sig, SIG_DFL);
  }
  std::signal(SIGPIPE, SIG_IGN);
}

void disableCoreDumpsInChild() {
  struct rlimit noCore;
  noCore.rlim_cur = 0;
  noCore.rlim_max = 0;
  // Best effort: the crash is reported structurally either way.
  setrlimit(RLIMIT_CORE, &noCore);
}

void appendLE32(std::uint32_t value, std::string &out) {
  for (int i = 0; i < 4; i++) {
    out.push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
  }
}

bool readLE32(const std::string &data,
              std::size_t offset,
              std::uint32_t &out) {
  if (data.size() - offset < 4) {
    return false;
  }
  std::uint32_t value = 0;
  for (int i = 0; i < 4; i++) {
    value |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(data[offset + i]))
        << (i * 8);
  }
  out = value;
  return true;
}

std::string hexEncode(const std::vector<std::uint8_t> &bytes) {
  static const char *const digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto b : bytes) {
    out.push_back(digits[(b >> 4) & 0xF]);
    out.push_back(digits[b & 0xF]);
  }
  return out;
}

bool hexDecode(const std::string &text, std::vector<std::uint8_t> &out) {
  if (text.size() % 2 != 0) {
    return false;
  }
  const auto nibble = [](char c) -> int {
    if ((c >= '0') && (c <= '9')) {
      return c - '0';
    }
    if ((c >= 'a') && (c <= 'f')) {
      return c - 'a' + 10;
    }
    if ((c >= 'A') && (c <= 'F')) {
      return c - 'A' + 10;
    }
    return -1;
  };
  out.clear();
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = nibble(text[i]);
    const int lo = nibble(text[i + 1]);
    if ((hi < 0) || (lo < 0)) {
      return false;
    }
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return true;
}

} // namespace

bool operator==(const IsolatedReplay &lhs, const IsolatedReplay &rhs) {
  return (lhs.random == rhs.random) && (lhs.size == rhs.size) &&
      (lhs.shrinkPath == rhs.shrinkPath) &&
      (lhs.failureClass == rhs.failureClass) &&
      (lhs.signalNumber == rhs.signalNumber);
}

bool operator!=(const IsolatedReplay &lhs, const IsolatedReplay &rhs) {
  return !(lhs == rhs);
}

std::string serializeIsolatedReplay(const IsolatedReplay &replay) {
  std::vector<std::uint8_t> bytes;
  auto output = std::back_inserter(bytes);
  output = detail::serialize(kIsolatedReplayVersion, output);
  output = detail::serialize(
      static_cast<std::uint8_t>(replay.failureClass), output);
  output = detail::serialize(static_cast<std::int32_t>(replay.signalNumber),
                             output);
  output = serialize(replay.random, output);
  output = detail::serialize(static_cast<std::uint32_t>(replay.size), output);
  output = detail::serializeCompact(
      begin(replay.shrinkPath), end(replay.shrinkPath), output);
  return std::string(kIsolatedReplayPrefix) + hexEncode(bytes);
}

Maybe<IsolatedReplay> deserializeIsolatedReplay(const std::string &text) {
  const std::string prefix(kIsolatedReplayPrefix);
  if (text.compare(0, prefix.size(), prefix) != 0) {
    return Nothing;
  }

  std::vector<std::uint8_t> bytes;
  if (!hexDecode(text.substr(prefix.size()), bytes)) {
    return Nothing;
  }

  IsolatedReplay replay;
  try {
    auto it = begin(bytes);
    std::uint32_t version = 0;
    it = detail::deserialize(it, end(bytes), version);
    if (version != kIsolatedReplayVersion) {
      return Nothing;
    }

    std::uint8_t failureClass = 0;
    it = detail::deserialize(it, end(bytes), failureClass);
    if (failureClass >
        static_cast<std::uint8_t>(IsolatedFailureClass::Crash)) {
      return Nothing;
    }
    replay.failureClass = static_cast<IsolatedFailureClass>(failureClass);

    std::int32_t signalNumber = 0;
    it = detail::deserialize(it, end(bytes), signalNumber);
    replay.signalNumber = static_cast<int>(signalNumber);

    it = deserialize(it, end(bytes), replay.random);

    std::uint32_t size = 0;
    it = detail::deserialize(it, end(bytes), size);
    replay.size = static_cast<int>(size);

    replay.shrinkPath.clear();
    const auto p = detail::deserializeCompact<std::size_t>(
        it, end(bytes), std::back_inserter(replay.shrinkPath));
    it = p.first;

    if (it != end(bytes)) {
      // Trailing garbage: not a well-formed record.
      return Nothing;
    }
  } catch (const detail::SerializationException &) {
    return Nothing;
  }

  return replay;
}

namespace detail {

std::string isolatedSignalName(int signalNumber) {
  switch (signalNumber) {
  case SIGABRT:
    return "SIGABRT";
  case SIGSEGV:
    return "SIGSEGV";
  case SIGBUS:
    return "SIGBUS";
  case SIGILL:
    return "SIGILL";
  case SIGFPE:
    return "SIGFPE";
  case SIGTRAP:
    return "SIGTRAP";
  case SIGPIPE:
    return "SIGPIPE";
  case SIGKILL:
    return "SIGKILL";
  case SIGTERM:
    return "SIGTERM";
#ifdef SIGSYS
  case SIGSYS:
    return "SIGSYS";
#endif
#ifdef SIGXCPU
  case SIGXCPU:
    return "SIGXCPU";
  case SIGXFSZ:
    return "SIGXFSZ";
#endif
  default:
    break;
  }
  return "signal " + std::to_string(signalNumber);
}

std::string describeIsolatedCrash(int signalNumber) {
  return "child process terminated by " + isolatedSignalName(signalNumber) +
      " (signal " + std::to_string(signalNumber) + ")";
}

std::string encodeIsolatedCaseMessage(const CaseResult &result,
                                      const Tags &tags) {
  std::string out;
  out.push_back(static_cast<char>(result.type));
  appendLE32(static_cast<std::uint32_t>(result.description.size()), out);
  out += result.description;
  appendLE32(static_cast<std::uint32_t>(tags.size()), out);
  for (const auto &tag : tags) {
    appendLE32(static_cast<std::uint32_t>(tag.size()), out);
    out += tag;
  }
  return out;
}

bool decodeIsolatedCaseMessage(const std::string &payload,
                               CaseResult &result,
                               Tags &tags) {
  std::size_t pos = 0;
  if (payload.empty()) {
    return false;
  }

  const auto typeByte = static_cast<std::uint8_t>(payload[pos++]);
  if (typeByte > static_cast<std::uint8_t>(CaseResult::Type::Discard)) {
    return false;
  }
  const auto type = static_cast<CaseResult::Type>(typeByte);

  std::uint32_t descriptionSize = 0;
  if (!readLE32(payload, pos, descriptionSize)) {
    return false;
  }
  pos += 4;
  if (payload.size() - pos < descriptionSize) {
    return false;
  }
  std::string description = payload.substr(pos, descriptionSize);
  pos += descriptionSize;

  std::uint32_t numTags = 0;
  if (!readLE32(payload, pos, numTags)) {
    return false;
  }
  pos += 4;

  Tags decodedTags;
  for (std::uint32_t i = 0; i < numTags; i++) {
    std::uint32_t tagSize = 0;
    if (!readLE32(payload, pos, tagSize)) {
      return false;
    }
    pos += 4;
    if (payload.size() - pos < tagSize) {
      return false;
    }
    decodedTags.push_back(payload.substr(pos, tagSize));
    pos += tagSize;
  }

  if (pos != payload.size()) {
    return false;
  }

  result = CaseResult(type, std::move(description));
  tags = std::move(decodedTags);
  return true;
}

IsolatedChannelStatus readIsolatedChannel(int fd,
                                          std::size_t maxMessageSize,
                                          std::string &messageOut,
                                          std::string &errorOut) {
  std::string data;
  bool tooLarge = false;
  char buffer[65536];
  for (;;) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      errorOut = errnoDescription("read");
      return IsolatedChannelStatus::IoError;
    }
    if (count == 0) {
      break; // EOF: the child's end of the pipe is closed
    }
    if (!tooLarge) {
      if (data.size() + static_cast<std::size_t>(count) > maxMessageSize) {
        // Keep draining so the writer is not blocked, but discard.
        tooLarge = true;
      } else {
        data.append(buffer, static_cast<std::size_t>(count));
      }
    }
  }

  if (tooLarge) {
    return IsolatedChannelStatus::TooLarge;
  }
  if (data.empty()) {
    return IsolatedChannelStatus::NoMessage;
  }

  std::uint32_t payloadSize = 0;
  if (!readLE32(data, 0, payloadSize)) {
    return IsolatedChannelStatus::Truncated;
  }
  if (data.size() - 4 != payloadSize) {
    return IsolatedChannelStatus::Truncated;
  }

  messageOut = data.substr(4);
  return IsolatedChannelStatus::Ok;
}

bool writeIsolatedChannel(int fd,
                          const std::string &payload,
                          std::string &errorOut) {
  std::string frame;
  frame.reserve(payload.size() + 4);
  appendLE32(static_cast<std::uint32_t>(payload.size()), frame);
  frame += payload;

  std::size_t offset = 0;
  while (offset < frame.size()) {
    const ssize_t count =
        write(fd, frame.data() + offset, frame.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      errorOut = errnoDescription("write");
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

IsolatedAttempt runIsolatedAttempt(
    const std::function<std::string()> &childMessage,
    std::size_t maxMessageSize,
    bool disableCoreDumps) {
  IsolatedAttempt attempt;

  if (!childMessage) {
    attempt.errorDescription = "no child action provided";
    return attempt;
  }

  int fds[2];
  if (pipe(fds) != 0) {
    attempt.errorDescription = errnoDescription("pipe");
    return attempt;
  }

  // Flush the caller's stdio and iostream buffers before forking so the
  // child inherits empty buffers. Otherwise a child that terminates
  // abnormally (abort() may flush inherited stdio streams on some
  // platforms) would print the parent's pending output a second time. This
  // only flushes; it does not touch the caller's file descriptors or
  // signal handlers.
  std::cout.flush();
  std::cerr.flush();
  std::clog.flush();
  std::fflush(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    const std::string description = errnoDescription("fork");
    close(fds[0]);
    close(fds[1]);
    attempt.errorDescription = description;
    return attempt;
  }

  if (pid == 0) {
    // ---- Child process -------------------------------------------------
    // The caller guarantees single-threaded entry, so using the inherited
    // address space (including heap) here is safe. The callback and this
    // code never run in the parent.
    close(fds[0]);
    if (disableCoreDumps) {
      disableCoreDumpsInChild();
    }
    resetChildSignalDispositions();

    std::string message;
    try {
      message = childMessage();
    } catch (...) {
      _exit(kChildExitActionThrew);
    }

    if ((maxMessageSize < 4) || (message.size() > maxMessageSize - 4)) {
      _exit(kChildExitMessageTooLarge);
    }

    std::string error;
    if (!writeIsolatedChannel(fds[1], message, error)) {
      _exit(kChildExitWriteFailed);
    }
    // _exit, not exit: never flush the parent's inherited output buffers a
    // second time and never run inherited atexit handlers.
    _exit(0);
  }

  // ---- Parent process --------------------------------------------------
  attempt.childPid = pid;
  close(fds[1]);

  // Read the result message while the child is still running so that
  // messages larger than the pipe capacity do not deadlock the child.
  std::string message;
  std::string ioError;
  const auto channelStatus =
      readIsolatedChannel(fds[0], maxMessageSize, message, ioError);
  close(fds[0]);

  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while ((waited < 0) && (errno == EINTR));

  if (waited != pid) {
    attempt.errorDescription = errnoDescription("waitpid");
    return attempt;
  }

  if (channelStatus == IsolatedChannelStatus::IoError) {
    attempt.errorDescription = ioError;
    return attempt;
  }

  if (WIFSIGNALED(status)) {
    attempt.kind = IsolatedAttempt::Kind::Signalled;
    attempt.signalNumber = WTERMSIG(status);
    return attempt;
  }

  if (!WIFEXITED(status)) {
    attempt.errorDescription = "child process changed state unexpectedly";
    return attempt;
  }

  const int exitCode = WEXITSTATUS(status);
  if (exitCode != 0) {
    switch (exitCode) {
    case kChildExitActionThrew:
      attempt.errorDescription =
          "child process action threw an uncaught exception";
      break;
    case kChildExitMessageTooLarge:
      attempt.errorDescription =
          "result message exceeds the maximum message size of " +
          std::to_string(maxMessageSize) + " bytes";
      break;
    case kChildExitWriteFailed:
      attempt.errorDescription =
          "child process failed to write its result message";
      break;
    default:
      attempt.errorDescription = "child process exited with code " +
          std::to_string(exitCode) + " without a result message";
      break;
    }
    return attempt;
  }

  // Exit code 0: a complete, well-formed message is required.
  switch (channelStatus) {
  case IsolatedChannelStatus::Ok:
    break;
  case IsolatedChannelStatus::NoMessage:
    attempt.errorDescription =
        "child process exited with code 0 without sending a result message";
    return attempt;
  case IsolatedChannelStatus::Truncated:
    attempt.errorDescription =
        "child process sent a truncated or malformed result message";
    return attempt;
  case IsolatedChannelStatus::TooLarge:
    attempt.errorDescription =
        "result message exceeds the maximum message size of " +
        std::to_string(maxMessageSize) + " bytes";
    return attempt;
  case IsolatedChannelStatus::IoError:
    attempt.errorDescription = ioError;
    return attempt;
  }

  CaseResult caseResult;
  Tags tags;
  if (!decodeIsolatedCaseMessage(message, caseResult, tags)) {
    attempt.errorDescription =
        "child process sent a malformed result message";
    return attempt;
  }

  attempt.kind = IsolatedAttempt::Kind::Completed;
  attempt.caseResult = std::move(caseResult);
  attempt.tags = std::move(tags);
  return attempt;
}

} // namespace detail
} // namespace rc

#endif // !defined(_WIN32)
