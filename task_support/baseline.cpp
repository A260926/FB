#include <rapidcheck.h>
#include <rapidcheck/detail/Property.h>
#include <rapidcheck/detail/TestListenerAdapter.h>
#include <rapidcheck/shrinkable/Create.h>
#include "detail/Testing.h"
#include <cassert>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
  using namespace rc;
  using namespace rc::detail;
  const auto parent = getpid();
  int calls = 0;
  Gen<int> input = [](const Random &, int) {
    return shrinkable::shrinkRecur(8, [](int x) {
      return x > 0 ? seq::just(x - 1) : Seq<int>();
    });
  };
  auto property = toProperty([&] {
    int x = *input;
    ++calls;
    assert(getpid() == parent);
    RC_ASSERT(x < 3);
  });
  TestParams params;
  params.seed = 1121;
  params.maxSuccess = 4;
  params.maxSize = 20;
  TestListenerAdapter listener;
  auto result = testProperty(property, TestMetadata(), params, listener);
  FailureResult failure;
  assert(result.match(failure));
  assert(failure.counterExample.size() == 1);
  assert(failure.counterExample.front().second == "3");
  assert(failure.reproduce.shrinkPath.size() == 5);
  const int searchCalls = calls;
  auto replay = reproduceProperty(property, failure.reproduce);
  FailureResult replayFailure;
  assert(replay.match(replayFailure));
  assert(replayFailure.counterExample == failure.counterExample);
  assert(calls > searchCalls);
  std::cout << "SERIAL_SHRINK_OK minimum=3 accepted=5 parent_calls=" << searchCalls << "\n";
  std::cout << "SERIAL_REPRODUCE_OK minimum=" << replayFailure.counterExample.front().second << "\n";

  // Observation only: the original property engine cannot return a result
  // after its callback raises a fatal native signal. No isolated API exists
  // here, and the outer observer does not generate or shrink candidates.
  std::cout.flush();
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    struct rlimit noCore = {0, 0};
    if (setrlimit(RLIMIT_CORE, &noCore) != 0) _exit(90);
    signal(SIGABRT, SIG_DFL);
    auto fatal = toProperty([] {
      std::raise(SIGABRT);
      return true;
    });
    testProperty(fatal, TestMetadata(), params, listener);
    _exit(91);
  }
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  assert(waited == child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
  std::cout << "NATIVE_SIGNAL_OBSERVED kind=SIGABRT no_case_result=1\n";
}
