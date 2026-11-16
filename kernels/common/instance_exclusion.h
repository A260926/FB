// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "default.h"
#include "rtcore.h"
#include "../../common/sys/ref.h"

#include <cstdint>
#include <vector>

namespace embree
{
  /*! Immutable sparse trie of instance exclusion paths.
   *
   *  The trie is indexed by the (geomID, instPrimID) edges of instance
   *  paths. Edges inside a node are kept sorted so child lookup is a binary
   *  search whose cost only depends on the node's degree instead of the
   *  total number of rules. The handle stores only IDs, never geometry
   *  pointers, so re-committing scenes does not invalidate it: paths are
   *  reinterpreted with the current instance IDs on every query. */
  class InstanceExclusion : public RefCount
  {
  public:

    struct Edge
    {
      unsigned int geomID;
      unsigned int instPrimID;

      bool operator==(const Edge& other) const {
        return geomID == other.geomID && instPrimID == other.instPrimID;
      }
      bool operator<(const Edge& other) const {
        return geomID != other.geomID ? geomID < other.geomID
                                      : instPrimID < other.instPrimID;
      }
    };

    struct TrieEdge
    {
      Edge key;
      uint32_t child;
    };

    struct TrieNode
    {
      TrieNode() : firstEdge(0), numEdges(0), terminal(false) {}
      uint32_t firstEdge;
      uint32_t numEdges;
      bool terminal;
    };

    InstanceExclusion(std::vector<std::vector<Edge>>&& paths);

    /*! Performs one trie edge transition. Returns the child node index, or
     *  -1 when no edge matches. terminal is set when the matched child node
     *  is an exclusion rule endpoint. Every key probe is counted through
     *  comparisons. */
    int transition(int node, const Edge& key, bool& terminal, uint64_t& comparisons) const
    {
      terminal = false;
      const TrieNode& n = nodes[(size_t)node];
      if (n.numEdges == 0) return -1;

      uint32_t lo = n.firstEdge;
      uint32_t hi = n.firstEdge + n.numEdges;
      while (lo < hi)
      {
        const uint32_t mid = lo + (hi - lo) / 2;
        ++comparisons;
        const TrieEdge& e = edges[mid];
        if (e.key == key) {
          terminal = nodes[e.child].terminal;
          return (int)e.child;
        }
        if (key < e.key) hi = mid;
        else             lo = mid + 1;
      }
      return -1;
    }

    /*! A handle without any rules never rejects an instance edge. */
    bool empty() const { return nodes.size() == 1 && !nodes[0].terminal; }

  private:
    std::vector<TrieNode> nodes;
    std::vector<TrieEdge> edges;
  };

  /*! Per-query, per-lane trie cursors. Embedded directly into the query
   *  entry functions on the stack, so no heap memory is allocated per ray.
   *  The object is shared (read/write only by the owning query) across all
   *  nested RayQueryContext instances of that query. */
  struct ExclusionRunState
  {
    static const size_t MAX_LANES = 16;

    ExclusionRunState() {
      for (size_t k = 0; k < MAX_LANES; ++k) {
        node[k] = 0;
        excluded[k] = false;
        handle[k] = nullptr;
      }
    }

    void setup(const RTCInstanceExclusion* table, size_t tableCount,
               RTCInstanceExclusionStats* statsPtr, size_t laneCount)
    {
      this->table = table;
      this->tableCount = tableCount;
      this->stats = statsPtr;
      this->laneCount = laneCount;
      this->active = (table != nullptr) && (tableCount != 0);
    }

    void initLane(size_t lane, unsigned int rayID)
    {
      node[lane] = 0;
      excluded[lane] = false;
      handle[lane] = nullptr;
      if (active && rayID < tableCount)
        handle[lane] = (const InstanceExclusion*)table[rayID];
    }

    /*! Attempts one trie transition for the given lane. Returns true when
     *  the instance edge is excluded and the subtree must not be entered.
     *  Lanes without a rule perform no table lookup at all. */
    bool transitionLane(size_t lane, unsigned int geomID, unsigned int instPrimID)
    {
      const InstanceExclusion* exc = handle[lane];
      if (!exc || exc->empty()) return false; // no rule: no table lookup at all
      if (excluded[lane]) return true;        // subtree already blocked above

      const int cur = node[lane];
      if (cur < 0) return false; // prefix already diverged, nothing can match below

      if (stats) ++stats->transitions;

      bool terminal = false;
      uint64_t comparisons = 0;
      const int child = exc->transition(cur, InstanceExclusion::Edge{geomID, instPrimID},
                                        terminal, comparisons);
      if (stats) stats->comparisons += comparisons;

      if (terminal) {
        excluded[lane] = true;
        return true; // do not commit: the subtree is never entered
      }
      node[lane] = child; // -1 once the path has no rule prefix anymore
      return false;
    }

    /*! Packet variant: transitions every active lane independently and
     *  returns the lanes whose subtree must be skipped. Inactive lanes and
     *  lanes without a rule are left alone. */
    template<int K>
    vbool<K> transitionPacket(const vbool<K>& valid, unsigned int geomID, unsigned int instPrimID)
    {
      int blockedMask = 0;
      if (active) {
        for (size_t k = 0; k < (size_t)K; ++k) {
          if (valid[k] && transitionLane(k, geomID, instPrimID))
            blockedMask |= (int)(1u << k);
        }
      }
      return vbool<K>(blockedMask);
    }

    const RTCInstanceExclusion* table = nullptr;
    size_t tableCount = 0;
    RTCInstanceExclusionStats* stats = nullptr;
    size_t laneCount = 0;
    bool active = false;

    int32_t node[MAX_LANES];                 // current trie node, -1 = dead prefix
    bool excluded[MAX_LANES];                // terminal rule matched (subtree blocked)
    const InstanceExclusion* handle[MAX_LANES]; // rule selected per lane via ray.id
  };

  /*! Saves and restores one lane cursor around an instance descent, so
   *  instance push/pop and early occlusion returns cannot leak selection
   *  state into sibling traversal. */
  struct ExclusionCursorGuard
  {
    ExclusionCursorGuard(ExclusionRunState* state, size_t lane)
      : state(state), lane(lane), armed(false)
    {
      if (state) {
        savedNode = state->node[lane];
        savedExcluded = state->excluded[lane];
        armed = true;
      }
    }

    ~ExclusionCursorGuard()
    {
      if (armed) {
        state->node[lane] = savedNode;
        state->excluded[lane] = savedExcluded;
      }
    }

    ExclusionRunState* state;
    size_t lane;
    int32_t savedNode = 0;
    bool savedExcluded = false;
    bool armed;
  };

  /*! Packet variant of the lane cursor guard. Each active lane gets its own
   *  save/restore; lanes that ended early (occluded) or were inactive keep
   *  their cursor untouched. */
  template<int K>
  struct ExclusionPacketGuard
  {
    ExclusionPacketGuard(ExclusionRunState* state, const vbool<K>& valid)
      : state(state), laneMask(0)
    {
      if (state) {
        for (size_t k = 0; k < (size_t)K; ++k) {
          if (valid[k]) {
            savedNode[k] = state->node[k];
            savedExcluded[k] = state->excluded[k];
            laneMask |= (unsigned)(1u << k);
          }
        }
      }
    }

    ~ExclusionPacketGuard()
    {
      if (state) {
        for (size_t k = 0; k < (size_t)K; ++k) {
          if (laneMask & (unsigned)(1u << k)) {
            state->node[k] = savedNode[k];
            state->excluded[k] = savedExcluded[k];
          }
        }
      }
    }

    ExclusionRunState* state;
    int32_t savedNode[16];
    bool savedExcluded[16];
    unsigned laneMask;
  };
}
