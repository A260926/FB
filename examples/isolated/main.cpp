// Local example: isolated checking of a crash-prone parser.
//
// Build and run from the repository root:
//   c++ -std=c++11 -Iinclude examples/isolated/main.cpp \
//     src/*.cpp src/detail/*.cpp src/gen/*.cpp src/gen/detail/*.cpp \
//     -o /tmp/isolated_example && /tmp/isolated_example
//
// The property below drives a tiny length-prefixed record parser whose old
// version aborts when the declared item count exceeds the bytes that
// actually follow. With the regular in-process check, the first abort would
// kill the whole test runner before any counterexample could be reported.
// checkIsolated runs every test case in a freshly forked child process, so
// the crash is found, shrunk and reported as structured data instead.

// Expose POSIX declarations in strict -std=c++11 mode on glibc. Must come
// before any system header.
#if !defined(__APPLE__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/Isolated.h>

namespace {

// Legacy length-prefixed record parser: buffer[0] is the claimed item
// count, followed by that many payload bytes. The legacy bounds guard
// aborts when the claimed count exceeds the bytes actually present.
void parseLegacy(const std::vector<std::uint8_t> &buffer) {
  if (buffer.empty()) {
    return;
  }
  const std::size_t claimed = buffer[0];
  const std::size_t available = buffer.size() - 1;
  if (claimed > available) {
    std::abort(); // legacy bounds guard
  }
  // ... consume `claimed` payload bytes ...
}

// The property, shared by the check and the replay: generation lives in the
// explicit Gen below, the callback only receives the value.
bool property(const std::vector<std::uint8_t> &buffer) {
  parseLegacy(buffer);
  return true;
}

} // namespace

int main() {
  using namespace rc;

  const auto input =
      gen::container<std::vector<std::uint8_t>>(gen::arbitrary<std::uint8_t>());

  IsolatedParams params;
  params.seed = 1234;
  params.maxSuccess = 200;

  const auto result = checkIsolated(input, &property, params);
  using Result = IsolatedCheckResult<std::vector<std::uint8_t>>;
  if (result.type != Result::Type::Failure) {
    std::cout << "no failure found (unexpected)" << std::endl;
    return 1;
  }

  const auto &failure = *result.failure;
  std::cout << "class:       "
            << (failure.failureClass == IsolatedFailureClass::Crash
                    ? "native crash"
                    : "assertion")
            << std::endl;
  std::cout << "description: " << failure.description << std::endl;
  std::cout << "original:    " << failure.originalInput.size() << " bytes"
            << std::endl;
  std::cout << "minimal:     " << failure.finalInput.size() << " bytes"
            << std::endl;
  std::cout << "shrink path: " << failure.shrinkPath.size() << " accepted steps"
            << std::endl;

  // The replay record is a plain copyable string: print it, store it, and
  // replay it later (even in another process) to re-run the minimal input
  // in a new child.
  const std::string record = serializeIsolatedReplay(failure.replay);
  std::cout << "replay record: " << record << std::endl;

  const auto parsed = deserializeIsolatedReplay(record);
  if (!parsed) {
    std::cout << "could not parse replay record" << std::endl;
    return 1;
  }

  const auto replay = replayIsolated(input, &property, *parsed);
  using ReplayResult = IsolatedReplayResult<std::vector<std::uint8_t>>;
  if (replay.type != ReplayResult::Type::Matched) {
    std::cout << "replay did not reproduce the failure" << std::endl;
    return 1;
  }
  std::cout << "replay reproduced: " << replay.failure->description
            << std::endl;
  return 0;
}
