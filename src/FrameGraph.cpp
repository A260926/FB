#include "fg/FrameGraph.hpp"
#include "fg/GraphvizWriter.hpp"
#include <algorithm>
#include <stdexcept>
#include <stack>

//
// FrameGraph class:
//

void FrameGraph::reserve(uint32_t numPasses, uint32_t numResources) {
  m_passNodes.reserve(numPasses);
  m_resourceNodes.reserve(numResources);
  m_resourceRegistry.reserve(numResources);
}

bool FrameGraph::isValid(FrameGraphResource id) const {
  const auto &node = _getResourceNode(id);
  return node.getVersion() == _getResourceEntry(node).getVersion();
}

void FrameGraph::compile() {
  for (auto &pass : m_passNodes) {
    pass.m_refCount = static_cast<int32_t>(pass.m_writes.size());
    for (const auto [id, _] : pass.m_reads) {
      auto &consumed = m_resourceNodes[id];
      consumed.m_refCount++;
    }
    for (const auto [id, _] : pass.m_writes) {
      auto &written = m_resourceNodes[id];
      written.m_producer = &pass;
    }
  }

  // -- Culling:

  std::stack<ResourceNode *> unreferencedResources;
  for (auto &node : m_resourceNodes) {
    if (node.m_refCount == 0) unreferencedResources.push(&node);
  }
  while (!unreferencedResources.empty()) {
    auto *unreferencedResource = unreferencedResources.top();
    unreferencedResources.pop();
    PassNode *producer{unreferencedResource->m_producer};
    if (producer == nullptr || producer->hasSideEffect()) continue;

    assert(producer->m_refCount >= 1);
    if (--producer->m_refCount == 0) {
      for (const auto [id, _] : producer->m_reads) {
        auto &node = m_resourceNodes[id];
        if (--node.m_refCount == 0) unreferencedResources.push(&node);
      }
    }
  }

  // -- Calculate resources lifetime:

  for (auto &pass : m_passNodes) {
    if (pass.m_refCount == 0) continue;

    for (const auto id : pass.m_creates)
      _getResourceEntry(id).m_producer = &pass;
    for (const auto [id, _] : pass.m_writes)
      _getResourceEntry(id).m_last = &pass;
    for (const auto [id, _] : pass.m_reads)
      _getResourceEntry(id).m_last = &pass;
  }

  m_compiled = true;
}
void FrameGraph::execute(void *context, void *allocator) {
  if (m_session.active && !m_session.finished)
    throw std::logic_error{
      "FrameGraph::execute: an execution session is active; use "
      "pollExecution()"};
  if (std::any_of(m_passNodes.cbegin(), m_passNodes.cend(),
                  [](const PassNode &pass) { return pass.isAsync(); }))
    throw std::logic_error{
      "FrameGraph::execute: the graph contains asynchronous passes; use "
      "startExecution()/pollExecution()"};

  for (const auto &pass : m_passNodes) {
    if (!pass.canExecute()) continue;

    for (const auto id : pass.m_creates)
      _getResourceEntry(id).create(allocator);

    for (const auto [id, flags] : pass.m_reads) {
      if (flags != kFlagsIgnored) _getResourceEntry(id).preRead(flags, context);
    }
    for (const auto [id, flags] : pass.m_writes) {
      if (flags != kFlagsIgnored)
        _getResourceEntry(id).preWrite(flags, context);
    }
    FrameGraphPassResources resources{*this, pass};
    // Synchronous passes are expected to return no completion token. If one
    // is returned anyway it is ignored: execute() is the legacy blocking path.
    (void)std::invoke(*pass.m_exec, resources, context);

    for (auto &entry : m_resourceRegistry) {
      if (entry.m_last == &pass && entry.isTransient())
        entry.destroy(allocator);
    }
  }
}

void FrameGraph::startExecution(void *context, void *allocator) {
  if (m_session.active && !m_session.finished)
    throw std::logic_error{
      "FrameGraph::startExecution: an execution session is already active"};
  if (!m_compiled)
    throw std::logic_error{
      "FrameGraph::startExecution: the graph has not been compiled"};

  const auto numPasses = m_passNodes.size();
  const auto numResources = m_resourceRegistry.size();
  const auto numNodes = m_resourceNodes.size();

  m_session = Session{};
  m_session.active = true;
  m_session.finished = false;
  m_session.context = context;
  m_session.allocator = allocator;

  m_session.resourceOf.resize(numNodes);
  for (uint32_t i = 0; i < numNodes; ++i)
    m_session.resourceOf[i] = _getResourceEntry(_getResourceNode(i)).getId();

  m_session.created.assign(numResources, false);
  m_session.destroyed.assign(numResources, false);
  m_session.lifetime.assign(numResources, 0u);

  m_session.submitted.assign(numPasses, false);
  m_session.completed.assign(numPasses, false);

  // Distinct physical resources touched by each live pass, plus the number of
  // uses that must still happen before a resource may be destroyed. Future
  // uses of passes that are later culled-out are irrelevant; a pass that is
  // compiled but never submitted keeps its uses alive.
  m_session.passResources.resize(numPasses);
  for (uint32_t passId = 0; passId < numPasses; ++passId) {
    const auto &pass = m_passNodes[passId];
    if (!pass.canExecute()) continue;

    auto &resources = m_session.passResources[passId];
    const auto note = [this, &resources](FrameGraphResource id) {
      const auto res = m_session.resourceOf[id];
      if (std::find(resources.cbegin(), resources.cend(), res) ==
          resources.cend()) {
        resources.emplace_back(res);
        ++m_session.lifetime[res];
      }
    };
    for (const auto [id, _] : pass.m_reads) note(id);
    for (const auto [id, _] : pass.m_writes) note(id);
    for (const auto id : pass.m_creates) note(id);
  }

  // Build the conflict edges from the create/read/write declarations. A pass
  // conflicts with an earlier pass when at least one of them writes the same
  // physical resource (creating counts as writing the initial version):
  // write-after-read, write-after-write, read-after-write. Read/read pairs
  // stay independent and may overlap. Edges are only per resource; no global
  // barrier is introduced.
  const auto collect = [this](const PassNode &pass, bool writes) {
    std::vector<uint32_t> resources;
    const auto note = [this, &resources](FrameGraphResource id) {
      const auto res = m_session.resourceOf[id];
      if (std::find(resources.cbegin(), resources.cend(), res) ==
          resources.cend())
        resources.emplace_back(res);
    };
    for (const auto [id, _] : pass.m_reads)
      if (!writes) note(id);
    for (const auto [id, _] : pass.m_writes) note(id);
    if (writes)
      for (const auto id : pass.m_creates) note(id);
    return resources;
  };
  const auto intersects = [](const std::vector<uint32_t> &lhs,
                             const std::vector<uint32_t> &rhs) {
    for (const auto l : lhs)
      if (std::find(rhs.cbegin(), rhs.cend(), l) != rhs.cend()) return true;
    return false;
  };

  std::vector<std::vector<uint32_t>> reads(numPasses);
  std::vector<std::vector<uint32_t>> writes(numPasses);
  for (uint32_t passId = 0; passId < numPasses; ++passId) {
    if (!m_passNodes[passId].canExecute()) continue;
    reads[passId] = collect(m_passNodes[passId], false);
    writes[passId] = collect(m_passNodes[passId], true);
  }

  m_session.predecessors.resize(numPasses);
  for (uint32_t a = 0; a < numPasses; ++a) {
    if (!m_passNodes[a].canExecute()) continue;
    for (uint32_t b = 0; b < a; ++b) {
      if (!m_passNodes[b].canExecute()) continue;
      if (intersects(writes[a], reads[b]) ||
          intersects(writes[a], writes[b]) ||
          intersects(reads[a], writes[b])) {
        m_session.predecessors[a].emplace_back(b);
      }
    }
  }
}

bool FrameGraph::pollExecution() {
  if (!m_session.active)
    throw std::logic_error{
      "FrameGraph::pollExecution: no execution session is active"};
  if (m_session.finished) return true;
  ++m_session.epoch;

  bool progressed = true;
  while (progressed) {
    progressed = false;

    // -- 1. Observe pending completion tokens. A token is queried at most once
    //       per poll and never again after it reported completion.

    std::size_t i = 0;
    while (i < m_session.completions.size()) {
      auto &pending = m_session.completions[i];
      if (pending.epoch == m_session.epoch) {
        ++i;
        continue;
      }
      pending.epoch = m_session.epoch;
      if (!pending.token.isComplete()) {
        ++i;
        continue;
      }
      const auto passId = pending.passId;
      m_session.completions[i] = std::move(m_session.completions.back());
      m_session.completions.pop_back();
      --m_session.outstanding;
      _completePass(passId);
      progressed = true;
    }

    // -- 2. Submit every pass that is ready, in registration order. Blocked
    //       passes are skipped so later independent passes can still go.

    for (uint32_t passId = 0; passId < m_passNodes.size(); ++passId) {
      if (!m_passNodes[passId].canExecute()) continue;
      if (m_session.submitted[passId] || !_isPassReady(passId)) continue;
      _submitPass(passId, m_passNodes[passId]);
      progressed = true;
    }
  }

  bool done = true;
  for (uint32_t passId = 0; passId < m_passNodes.size(); ++passId) {
    if (!m_passNodes[passId].canExecute()) continue; // Culled: never runs.
    if (!m_session.completed[passId]) {
      done = false;
      break;
    }
  }
  done = done && m_session.outstanding == 0;
  if (done) m_session.finished = true;
  return done;
}
bool FrameGraph::_isPassReady(uint32_t passId) const {
  for (const auto predecessor : m_session.predecessors[passId]) {
    if (!m_session.completed[predecessor]) return false;
  }
  return true;
}

void FrameGraph::_submitPass(uint32_t passId, const PassNode &pass) {
  m_session.submitted[passId] = true;

  // Resources are created once, right before the first actual use. Imported
  // resources belong to the caller and are never created.
  for (const auto res : m_session.passResources[passId]) {
    if (m_session.created[res]) continue;
    m_session.created[res] = true;
    if (m_resourceRegistry[res].isImported()) continue;
    m_resourceRegistry[res].create(m_session.allocator);
  }

  for (const auto [id, flags] : pass.m_reads) {
    if (flags != kFlagsIgnored)
      m_resourceRegistry[m_session.resourceOf[id]].preRead(flags,
                                                           m_session.context);
  }
  for (const auto [id, flags] : pass.m_writes) {
    if (flags != kFlagsIgnored)
      m_resourceRegistry[m_session.resourceOf[id]].preWrite(flags,
                                                            m_session.context);
  }

  FrameGraphPassResources resources{*this, pass};
  auto completion = std::invoke(*pass.m_exec, resources, m_session.context);
  ++m_session.outstanding;
  if (!completion.isComplete()) {
    m_session.completions.emplace_back(
      PendingCompletion{std::move(completion), passId, m_session.epoch});
  } else {
    // An immediately-finished submission unlocks downstream passes during the
    // same poll (the completion loop above re-runs).
    --m_session.outstanding;
    _completePass(passId);
  }
}

void FrameGraph::_completePass(uint32_t passId) {
  m_session.completed[passId] = true;

  for (const auto res : m_session.passResources[passId]) {
    assert(m_session.lifetime[res] > 0);
    --m_session.lifetime[res];
    if (m_session.lifetime[res] != 0) continue;
    if (!m_session.created[res] || m_session.destroyed[res]) continue;
    // Imported resources are owned by the caller: never create or destroy.
    if (m_resourceRegistry[res].isImported()) continue;
    m_session.destroyed[res] = true;
    m_resourceRegistry[res].destroy(m_session.allocator);
  }
}

//
// (private):
//

PassNode &FrameGraph::_createPassNode(
  const std::string_view name, std::unique_ptr<FrameGraphPassConcept> &&base,
  bool async) {
  const auto id = static_cast<uint32_t>(m_passNodes.size());
  return m_passNodes.emplace_back(PassNode{name, id, std::move(base), async});
}

ResourceNode &FrameGraph::_createResourceNode(const std::string_view name,
                                              uint32_t resourceId,
                                              uint32_t version) {
  const auto id = static_cast<uint32_t>(m_resourceNodes.size());
  return m_resourceNodes.emplace_back(
    ResourceNode{name, id, resourceId, version});
}
FrameGraphResource FrameGraph::_clone(FrameGraphResource id) {
  const auto &node = _getResourceNode(id);
  auto &entry = _getResourceEntry(node);
  entry.m_version++;

  const auto &clone = _createResourceNode(node.getName(), node.getResourceId(),
                                          entry.getVersion());
  return clone.getId();
}

const ResourceNode &FrameGraph::_getResourceNode(FrameGraphResource id) const {
  assert(id < m_resourceNodes.size());
  return m_resourceNodes[id];
}
const ResourceEntry &
FrameGraph::_getResourceEntry(FrameGraphResource id) const {
  return _getResourceEntry(_getResourceNode(id));
}
const ResourceEntry &
FrameGraph::_getResourceEntry(const ResourceNode &node) const {
  assert(node.m_resourceId < m_resourceRegistry.size());
  return m_resourceRegistry[node.m_resourceId];
}

// ---

std::ostream &operator<<(std::ostream &os, const FrameGraph &fg) {
  return fg.debugOutput(os, graphviz::Writer{});
}

//
// FrameGraph::Builder class:
//

FrameGraphResource FrameGraph::Builder::read(FrameGraphResource id,
                                             uint32_t flags) {
  assert(m_frameGraph.isValid(id));
  return m_passNode._read(id, flags);
}
FrameGraphResource FrameGraph::Builder::write(FrameGraphResource id,
                                              uint32_t flags) {
  assert(m_frameGraph.isValid(id));
  if (m_frameGraph._getResourceEntry(id).isImported()) setSideEffect();

  if (m_passNode.creates(id)) {
    return m_passNode._write(id, flags);
  } else {
    // Writing to a texture produces a renamed handle.
    // This allows us to catch errors when resources are modified in
    // undefined order (when same resource is written by different passes).
    // Renaming resources enforces a specific execution order of the render
    // passes.
    m_passNode._read(id, kFlagsIgnored);
    return m_passNode._write(m_frameGraph._clone(id), flags);
  }
}
