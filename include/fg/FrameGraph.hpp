#pragma once

#include "fg/PassNode.hpp"
#include "fg/ResourceNode.hpp"
#include "fg/ResourceEntry.hpp"
#include "fg/Completion.hpp"
#include <cstdint>

class FrameGraph {
  friend class FrameGraphPassResources;

public:
  FrameGraph() = default;
  FrameGraph(const FrameGraph &) = delete;
  FrameGraph(FrameGraph &&) noexcept = delete;

  FrameGraph &operator=(const FrameGraph &) = delete;
  FrameGraph &operator=(FrameGraph &&) noexcept = delete;

  friend std::ostream &operator<<(std::ostream &, const FrameGraph &);

  static constexpr auto kFlagsIgnored = ~0;

  class Builder final {
    friend class FrameGraph;

  public:
    Builder() = delete;
    Builder(const Builder &) = delete;
    Builder(Builder &&) noexcept = delete;

    Builder &operator=(const Builder &) = delete;
    Builder &operator=(Builder &&) noexcept = delete;

    template <_VIRTUALIZABLE_CONCEPT(T)>
    /** Declares the creation of a resource. */
    [[nodiscard]] FrameGraphResource create(const std::string_view name,
                                            const typename T::Desc &);
    /** Declares read operation. */
    FrameGraphResource read(FrameGraphResource id,
                            uint32_t flags = kFlagsIgnored);
    /**
     * Declares write operation.
     * @remark Writing to imported resource counts as side-effect.
     */
    [[nodiscard]] FrameGraphResource write(FrameGraphResource id,
                                           uint32_t flags = kFlagsIgnored);

    /** Ensures that this pass is not culled during the compilation phase. */
    Builder &setSideEffect() {
      m_passNode.m_hasSideEffect = true;
      return *this;
    }

  private:
    Builder(FrameGraph &fg, PassNode &node)
        : m_frameGraph{fg}, m_passNode{node} {}

  private:
    FrameGraph &m_frameGraph;
    PassNode &m_passNode;
  };

  void reserve(uint32_t numPasses, uint32_t numResources);

  struct NoData {};
  /**
   * @param setup Callback (lambda, may capture by reference), invoked
   * immediately, declare operations here.
   * @param exec Execution of this lambda is deferred until execute() phase
   * (must capture by value due to this).
   */
  template <typename Data = NoData, typename Setup, typename Execute>
  const Data &addCallbackPass(const std::string_view name, Setup &&setup,
                              Execute &&exec);

  /**
   * Registers a pass whose execution callback submits deferred device work.
   *
   * The setup callback and the Data lifecycle are identical to
   * addCallbackPass. The execution callback returns a FrameGraphCompletion
   * token; the pass is considered finished only once that token reports
   * completion. Such graphs must be run through startExecution/pollExecution.
   */
  template <typename Data = NoData, typename Setup, typename Execute>
  const Data &addAsyncCallbackPass(const std::string_view name, Setup &&setup,
                                   Execute &&exec);

  template <_VIRTUALIZABLE_CONCEPT(T)>
  [[nodiscard]] const typename T::Desc &
  getDescriptor(FrameGraphResource id) const;

  template <_VIRTUALIZABLE_CONCEPT(T)>
  /** Imports the given resource T into FrameGraph. */
  [[nodiscard]] FrameGraphResource import(const std::string_view name,
                                          const typename T::Desc &, T &&);

  /** @return True if the given resource is valid for read/write operation. */
  [[nodiscard]] bool isValid(FrameGraphResource id) const;

  /** Culls unreferenced resources and passes. */
  void compile();
  /** Invokes execution callbacks (synchronous graphs only). */
  void execute(void *context = nullptr, void *allocator = nullptr);

  /**
   * Begins an execution session over this (compiled) graph. All per-session
   * completion and allocation state is reset, so a compiled graph can be
   * started again after the previous session finished.
   */
  void startExecution(void *context = nullptr, void *allocator = nullptr);
  /**
   * Advances the current execution session: submits every pass that can be
   * submitted right now, observes completion tokens and releases resources
   * whose last surviving use has finished.
   * @return True once every pass of the session has completed.
   */
  [[nodiscard]] bool pollExecution();
  [[nodiscard]] bool isExecuting() const { return m_session.active; }

  template <typename Writer>
  std::ostream &debugOutput(std::ostream &, Writer &&) const;

private:
  [[nodiscard]] PassNode &
  _createPassNode(const std::string_view name,
                  std::unique_ptr<FrameGraphPassConcept> &&, bool async);

  template <_VIRTUALIZABLE_CONCEPT(T)>
  [[nodiscard]] FrameGraphResource _create(const ResourceEntry::Type,
                                           const std::string_view name,
                                           const typename T::Desc &, T &&);

  [[nodiscard]] ResourceNode &
  _createResourceNode(const std::string_view name, uint32_t resourceId,
                      uint32_t version = ResourceEntry::kInitialVersion);
  /** Increments ResourceEntry version and produces a renamed handle. */
  [[nodiscard]] FrameGraphResource _clone(FrameGraphResource id);

  [[nodiscard]] const ResourceNode &
  _getResourceNode(FrameGraphResource id) const;
  [[nodiscard]] const ResourceEntry &
  _getResourceEntry(FrameGraphResource id) const;
  [[nodiscard]] const ResourceEntry &
  _getResourceEntry(const ResourceNode &) const;

  [[nodiscard]] decltype(auto) _getResourceNode(FrameGraphResource id) {
    return const_cast<ResourceNode &>(
      const_cast<const FrameGraph *>(this)->_getResourceNode(id));
  }
  [[nodiscard]] decltype(auto) _getResourceEntry(FrameGraphResource id) {
    return const_cast<ResourceEntry &>(
      const_cast<const FrameGraph *>(this)->_getResourceEntry(id));
  }
  [[nodiscard]] decltype(auto) _getResourceEntry(const ResourceNode &node) {
    return const_cast<ResourceEntry &>(
      const_cast<const FrameGraph *>(this)->_getResourceEntry(node));
  }

private:
  /** @return True if all conflicting earlier uses of the pass have completed. */
  [[nodiscard]] bool _isPassReady(uint32_t passId) const;
  void _submitPass(uint32_t passId, const PassNode &);
  /** Releases the uses of a finished pass, destroying resources if needed. */
  void _completePass(uint32_t passId);

private:
  std::vector<PassNode> m_passNodes;
  std::vector<ResourceNode> m_resourceNodes;
  std::vector<ResourceEntry> m_resourceRegistry;
  bool m_compiled{false};

  /**
   * State of one startExecution/pollExecution session. Everything here is
   * reset by startExecution so the same compiled graph can be run again.
   */
  struct PendingCompletion {
    FrameGraphCompletion token;
    uint32_t passId;
    // Poll during which the token was last queried. A not-yet-ready token is
    // never queried twice during the same poll.
    uint32_t epoch;
  };
  struct Session {
    bool active{false};
    bool finished{false};
    uint32_t epoch{0};

    void *context{nullptr};
    void *allocator{nullptr};

    // Physical resource of each virtual (node) resource.
    std::vector<uint32_t> resourceOf;
    // Distinct physical resources touched by each pass.
    std::vector<std::vector<uint32_t>> passResources;
    // Passes that must finish before a given pass may be submitted. These are
    // the conflicting (write/read or write/write or read/write) earlier uses;
    // read/read pairs never conflict, so reads may overlap.
    std::vector<std::vector<uint32_t>> predecessors;

    // Per physical resource lifetime accounting.
    std::vector<uint32_t> lifetime; // Live passes still using the resource.
    std::vector<bool> created;
    std::vector<bool> destroyed;

    // Per pass bookkeeping.
    std::vector<bool> submitted;
    std::vector<bool> completed;
    std::vector<PendingCompletion> completions;

    uint32_t outstanding{0}; // Submitted but unfinished passes.
  };
  Session m_session;
};

class FrameGraphPassResources {
  friend class FrameGraph;

public:
  FrameGraphPassResources() = delete;
  FrameGraphPassResources(const FrameGraphPassResources &) = delete;
  FrameGraphPassResources(FrameGraphPassResources &&) noexcept = delete;
  ~FrameGraphPassResources() = default;

  FrameGraphPassResources &operator=(const FrameGraphPassResources &) = delete;
  FrameGraphPassResources &
  operator=(FrameGraphPassResources &&) noexcept = delete;

  /**
   * @note Causes runtime-error with:
   * - Attempt to use obsolete handle (the one that has been renamed before)
   * - Incorrect resource type T
   */
  template <_VIRTUALIZABLE_CONCEPT(T)>
  [[nodiscard]] T &get(FrameGraphResource id);
  template <_VIRTUALIZABLE_CONCEPT(T)>
  [[nodiscard]] const typename T::Desc &
  getDescriptor(FrameGraphResource id) const;

private:
  FrameGraphPassResources(FrameGraph &fg, const PassNode &node)
      : m_frameGraph{fg}, m_passNode{node} {}

private:
  FrameGraph &m_frameGraph;
  const PassNode &m_passNode;
};

#include "fg/FrameGraph.inl"
