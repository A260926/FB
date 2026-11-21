#pragma once

#include "fg/Completion.hpp"
#include <functional>
#include <type_traits>
#include <utility>

class FrameGraphPassResources;

struct FrameGraphPassConcept {
  FrameGraphPassConcept() = default;
  FrameGraphPassConcept(const FrameGraphPassConcept &) = delete;
  FrameGraphPassConcept(FrameGraphPassConcept &&) noexcept = delete;
  virtual ~FrameGraphPassConcept() = default;

  FrameGraphPassConcept &operator=(const FrameGraphPassConcept &) = delete;
  FrameGraphPassConcept &operator=(FrameGraphPassConcept &&) noexcept = delete;

  /**
   * Submits the work of this pass.
   * @return Completion token of the submitted device work.
   */
  virtual FrameGraphCompletion operator()(FrameGraphPassResources &, void *) = 0;
};

template <typename Data, typename Execute>
struct FrameGraphPass final : FrameGraphPassConcept {
  explicit FrameGraphPass(Execute &&exec)
      : execFunction{std::forward<Execute>(exec)} {}

  FrameGraphCompletion operator()(FrameGraphPassResources &resources,
                                  void *context) override {
    using Result =
      decltype(std::invoke(execFunction, data, resources, context));
    if constexpr (std::is_void_v<Result>) {
      std::invoke(execFunction, data, resources, context);
      return {};
    } else {
      return FrameGraphCompletion{
        std::invoke(execFunction, data, resources, context)};
    }
  }

  Execute execFunction;
  Data data{};
};
