#pragma once

#include <functional>
#include <type_traits>
#include <utility>

/**
 * Completion token of a unit of device work.
 *
 * A default-constructed token represents work that has already completed.
 * Otherwise the token is constructed from a callable returning true once the
 * work is done.  A token is queried at most until it reports completion; after
 * that the owner must not query it again.
 */
class FrameGraphCompletion {
public:
  using Query = std::function<bool()>;

  /** Work is already complete. */
  FrameGraphCompletion() = default;

  FrameGraphCompletion(const Query &query) : m_query{query} {}
  FrameGraphCompletion(Query &&query) : m_query{std::move(query)} {}

  template <typename F,
            typename = std::enable_if_t<
              std::is_invocable_r_v<bool, F &> &&
              !std::is_same_v<std::decay_t<F>, FrameGraphCompletion> &&
              !std::is_same_v<std::decay_t<F>, Query>>>
  FrameGraphCompletion(F &&query)
      : m_query{Query{std::forward<F>(query)}} {}

  /** @return True if this unit of work is complete. */
  [[nodiscard]] bool isComplete() const { return !m_query || m_query(); }
  [[nodiscard]] bool poll() const { return isComplete(); }
  [[nodiscard]] bool ready() const { return isComplete(); }
  [[nodiscard]] bool complete() const { return isComplete(); }
  [[nodiscard]] bool operator()() const { return isComplete(); }
  [[nodiscard]] explicit operator bool() const { return isComplete(); }

  [[nodiscard]] bool hasQuery() const noexcept {
    return static_cast<bool>(m_query);
  }
  void reset() noexcept { m_query = nullptr; }

private:
  Query m_query;
};
