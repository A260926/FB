#pragma once

#include "rapidcheck/detail/Property.h"
#include "rapidcheck/shrinkable/Operations.h"

namespace rc {
namespace detail {

/// Evaluates one candidate value in a forked child. The callback (wrapped in
/// the regular PropertyAdapter, so assertions, discards, exceptions, tags and
/// logs keep their usual meaning) only ever runs in the child.
template <typename T, typename Callable>
IsolatedAttempt evaluateIsolated(const T &value,
                                 const Callable &callable,
                                 std::size_t maxMessageSize,
                                 bool disableCoreDumps) {
  return runIsolatedAttempt(
      [&value, &callable]() -> std::string {
        // This lambda is only invoked in the forked child process.
        PropertyAdapter<Decay<Callable>> adapter(callable);
        const TaggedResult tagged = adapter(value);
        return encodeIsolatedCaseMessage(tagged.result, tagged.tags);
      },
      maxMessageSize,
      disableCoreDumps);
}

/// Same sizing schedule as the regular search (see src/detail/Testing.cpp).
inline int isolatedSizeFor(int maxSuccess, int maxSize, int i) {
  const auto numSizes = maxSize + 1;
  const auto numRegular = (maxSuccess / numSizes) * numSizes;
  if (i < numRegular) {
    return i % numSizes;
  }

  const auto numRest = maxSuccess - numRegular;
  if (numRest == 1) {
    return 0;
  } else {
    return ((i % numSizes) * maxSize) / (numRest - 1);
  }
}

inline IsolatedAttemptInfo makeAttemptInfo(const IsolatedAttempt &attempt,
                                           bool shrinkPhase,
                                           bool accepted) {
  IsolatedAttemptInfo info;
  info.childPid = attempt.childPid;
  info.signalNumber = attempt.signalNumber;
  info.shrinkPhase = shrinkPhase;
  info.accepted = accepted;
  switch (attempt.kind) {
  case IsolatedAttempt::Kind::Completed:
    switch (attempt.caseResult.type) {
    case detail::CaseResult::Type::Success:
      info.outcome = IsolatedAttemptInfo::Outcome::Success;
      break;
    case detail::CaseResult::Type::Failure:
      info.outcome = IsolatedAttemptInfo::Outcome::Failure;
      break;
    case detail::CaseResult::Type::Discard:
      info.outcome = IsolatedAttemptInfo::Outcome::Discard;
      break;
    }
    break;
  case IsolatedAttempt::Kind::Signalled:
    info.outcome = IsolatedAttemptInfo::Outcome::Signalled;
    break;
  case IsolatedAttempt::Kind::Error:
    info.outcome = IsolatedAttemptInfo::Outcome::Error;
    break;
  }
  return info;
}

/// Whether an attempt belongs to the same failure category as the first
/// failure. For crashes the signal number must also match: a different
/// signal, a plain `false` or a precondition discard can never replace the
/// original crash, and vice versa.
inline bool matchesFailureClass(const IsolatedAttempt &attempt,
                                IsolatedFailureClass failureClass,
                                int failureSignal) {
  switch (failureClass) {
  case IsolatedFailureClass::Assertion:
    return attempt.kind == IsolatedAttempt::Kind::Completed &&
        attempt.caseResult.type == CaseResult::Type::Failure;
  case IsolatedFailureClass::Crash:
    return attempt.kind == IsolatedAttempt::Kind::Signalled &&
        attempt.signalNumber == failureSignal;
  }
  return false;
}

/// Shrinks the first failing shrinkable (in the parent, evaluating every
/// candidate in a new child) and builds the Failure result.
template <typename T, typename Callable>
IsolatedCheckResult<T> shrinkAndReportFailure(
    Shrinkable<T> shrinkable,
    const Random &random,
    int size,
    IsolatedFailureClass failureClass,
    int failureSignal,
    std::string failureDescription,
    std::vector<std::string> failureTags,
    const T &originalValue,
    const Callable &callable,
    const IsolatedParams &params,
    int numSuccess,
    int numDiscarded) {
  IsolatedFailureInfo<T> info;
  info.failureClass = failureClass;
  info.signalNumber = failureSignal;
  info.originalInput = originalValue;
  info.description = std::move(failureDescription);
  info.tags = std::move(failureTags);
  info.numShrinkErrors = 0;

  std::vector<std::size_t> path;
  Shrinkable<T> best = std::move(shrinkable);
  if (!params.disableShrinking) {
    auto shrinks = best.shrinks();
    std::size_t i = 0;
    while (auto shrink = shrinks.next()) {
      T candidate = shrink->value();
      const auto attempt = evaluateIsolated(candidate,
                                            callable,
                                            params.maxMessageSize,
                                            params.disableCoreDumps);
      bool accept = false;
      if (attempt.kind == IsolatedAttempt::Kind::Error) {
        // Run errors are not test outcomes; the candidate is rejected.
        info.numShrinkErrors++;
      } else {
        accept = matchesFailureClass(attempt, failureClass, failureSignal);
      }
      if (params.observer) {
        params.observer(makeAttemptInfo(attempt, true, accept));
      }

      if (accept) {
        best = std::move(*shrink);
        shrinks = best.shrinks();
        path.push_back(i);
        i = 0;
        if (attempt.kind == IsolatedAttempt::Kind::Completed) {
          info.description = attempt.caseResult.description;
          info.tags = attempt.tags;
        } else {
          info.description = describeIsolatedCrash(attempt.signalNumber);
          info.tags.clear();
        }
      } else {
        i++;
      }
    }
  }

  info.finalInput = best.value();
  info.shrinkPath = std::move(path);
  info.replay.random = random;
  info.replay.size = size;
  info.replay.shrinkPath = info.shrinkPath;
  info.replay.failureClass = failureClass;
  info.replay.signalNumber = failureSignal;

  IsolatedCheckResult<T> result;
  result.type = IsolatedCheckResult<T>::Type::Failure;
  result.numSuccess = numSuccess;
  result.numDiscarded = numDiscarded;
  result.failure = std::move(info);
  return result;
}

} // namespace detail

template <typename T, typename Callable>
IsolatedCheckResult<T> checkIsolated(Gen<T> gen,
                                     Callable callable,
                                     IsolatedParams params) {
  using detail::IsolatedAttempt;

  IsolatedCheckResult<T> result;
  result.type = IsolatedCheckResult<T>::Type::Success;

  const int maxDiscard = params.maxDiscardRatio * params.maxSuccess;
  int recentDiscards = 0;
  Random r(params.seed);

  // Search phase: generation in the parent, evaluation in children. The
  // first failure is handed off to the shrink phase immediately.
  while (result.numSuccess < params.maxSuccess) {
    const int size =
        detail::isolatedSizeFor(params.maxSuccess, params.maxSize, result.numSuccess) +
        (recentDiscards / 10);
    const Random random = r.split();

    auto shrinkable = gen(random, size);
    T value = shrinkable.value();
    const auto attempt = detail::evaluateIsolated(value,
                                                  callable,
                                                  params.maxMessageSize,
                                                  params.disableCoreDumps);
    if (params.observer) {
      params.observer(detail::makeAttemptInfo(attempt, false, false));
    }

    switch (attempt.kind) {
    case IsolatedAttempt::Kind::Error:
      result.type = IsolatedCheckResult<T>::Type::Error;
      result.description = attempt.errorDescription;
      return result;

    case IsolatedAttempt::Kind::Signalled:
      return detail::shrinkAndReportFailure<T>(
          std::move(shrinkable),
          random,
          size,
          IsolatedFailureClass::Crash,
          attempt.signalNumber,
          detail::describeIsolatedCrash(attempt.signalNumber),
          std::vector<std::string>(),
          value,
          callable,
          params,
          result.numSuccess,
          result.numDiscarded);

    case IsolatedAttempt::Kind::Completed:
      switch (attempt.caseResult.type) {
      case detail::CaseResult::Type::Success:
        result.numSuccess++;
        recentDiscards = 0;
        if (!attempt.tags.empty()) {
          result.distribution[attempt.tags]++;
        }
        break;

      case detail::CaseResult::Type::Discard:
        result.numDiscarded++;
        recentDiscards++;
        if (result.numDiscarded > maxDiscard) {
          result.type = IsolatedCheckResult<T>::Type::GaveUp;
          result.description = attempt.caseResult.description;
          return result;
        }
        break;

      case detail::CaseResult::Type::Failure:
        return detail::shrinkAndReportFailure<T>(
            std::move(shrinkable),
            random,
            size,
            IsolatedFailureClass::Assertion,
            0,
            attempt.caseResult.description,
            attempt.tags,
            value,
            callable,
            params,
            result.numSuccess,
            result.numDiscarded);
      }
      break;
    }
  }

  return result;
}

template <typename T, typename Callable>
IsolatedReplayResult<T> replayIsolated(Gen<T> gen,
                                       Callable callable,
                                       const IsolatedReplay &replay,
                                       IsolatedParams params) {
  using detail::IsolatedAttempt;

  IsolatedReplayResult<T> result;

  const auto expected = [](const IsolatedReplay &r) {
    return r.failureClass == IsolatedFailureClass::Crash
        ? detail::describeIsolatedCrash(r.signalNumber)
        : std::string("property failure (assertion or false return)");
  };

  const auto shrinkable = gen(replay.random, replay.size);
  const auto minShrinkable =
      shrinkable::walkPath(shrinkable, replay.shrinkPath);
  if (!minShrinkable) {
    result.type = IsolatedReplayResult<T>::Type::Mismatch;
    result.expectedDescription = expected(replay);
    result.actualDescription = "shrink path does not exist for regenerated input";
    return result;
  }

  T value = minShrinkable->value();
  result.finalInput = value;

  const auto attempt = detail::evaluateIsolated(value,
                                                callable,
                                                params.maxMessageSize,
                                                params.disableCoreDumps);
  if (params.observer) {
    params.observer(detail::makeAttemptInfo(attempt, false, false));
  }

  if (attempt.kind == IsolatedAttempt::Kind::Error) {
    result.type = IsolatedReplayResult<T>::Type::Error;
    result.errorDescription = attempt.errorDescription;
    return result;
  }

  // Describe what actually happened in the replay execution.
  std::string actualDescription;
  std::string actualFailureDescription;
  std::vector<std::string> actualTags;
  if (attempt.kind == IsolatedAttempt::Kind::Signalled) {
    actualDescription = detail::describeIsolatedCrash(attempt.signalNumber);
  } else {
    switch (attempt.caseResult.type) {
    case detail::CaseResult::Type::Success:
      actualDescription = "case succeeded";
      break;
    case detail::CaseResult::Type::Discard:
      actualDescription =
          "case was discarded (" + attempt.caseResult.description + ")";
      break;
    case detail::CaseResult::Type::Failure:
      actualDescription =
          "property failure: " + attempt.caseResult.description;
      actualFailureDescription = attempt.caseResult.description;
      actualTags = attempt.tags;
      break;
    }
  }

  if (!detail::matchesFailureClass(attempt, replay.failureClass, replay.signalNumber)) {
    // The replay did not reproduce the recorded failure; report the
    // mismatch instead of the recorded (stale) failure.
    result.type = IsolatedReplayResult<T>::Type::Mismatch;
    result.expectedDescription = expected(replay);
    result.actualDescription = std::move(actualDescription);
    return result;
  }

  IsolatedFailureInfo<T> info;
  info.failureClass = replay.failureClass;
  info.signalNumber =
      replay.failureClass == IsolatedFailureClass::Crash ? attempt.signalNumber : 0;
  if (attempt.kind == IsolatedAttempt::Kind::Signalled) {
    info.description = detail::describeIsolatedCrash(attempt.signalNumber);
  } else {
    info.description = std::move(actualFailureDescription);
    info.tags = std::move(actualTags);
  }
  info.originalInput = value;
  info.finalInput = std::move(value);
  info.shrinkPath = replay.shrinkPath;
  info.replay = replay;
  info.numShrinkErrors = 0;

  result.type = IsolatedReplayResult<T>::Type::Matched;
  result.failure = std::move(info);
  return result;
}

} // namespace rc
