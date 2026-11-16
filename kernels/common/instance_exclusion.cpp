// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#define RTC_EXPORT_API

#include "instance_exclusion.h"
#include "device.h"
#include "rtcore.h"

#include <map>
#include <utility>

namespace embree
{
  InstanceExclusion::InstanceExclusion(std::vector<std::vector<Edge>>&& paths)
  {
    struct BuildNode
    {
      std::map<Edge, uint32_t> children;
      bool terminal = false;
    };

    /* Build the trie from the validated, deep-copied paths. std::map keeps
     * edges sorted, so flattened child tables support binary search and
     * duplicate paths as well as descendant paths merge automatically. */
    std::vector<BuildNode> built(1);
    for (const auto& path : paths)
    {
      uint32_t cur = 0;
      for (const Edge& edge : path)
      {
        auto it = built[cur].children.find(edge);
        if (it == built[cur].children.end()) {
          const uint32_t child = (uint32_t)built.size();
          built.emplace_back();
          built[cur].children.emplace(edge, child);
          cur = child;
        } else {
          cur = it->second;
        }
      }
      built[cur].terminal = true;
    }

    /* Flatten into cache-friendly parallel node/edge arrays. */
    nodes.reserve(built.size());
    for (const BuildNode& bn : built)
    {
      TrieNode node;
      node.firstEdge = (uint32_t)edges.size();
      node.numEdges = (uint32_t)bn.children.size();
      node.terminal = bn.terminal;
      nodes.push_back(node);
      for (const auto& entry : bn.children)
        edges.push_back(TrieEdge{entry.first, entry.second});
    }
  }
}

RTC_NAMESPACE_BEGIN;

using namespace embree;

RTC_API RTCInstanceExclusion rtcNewInstanceExclusion(
  RTCDevice hdevice,
  const RTCInstanceExclusionPath* const* paths,
  const size_t* pathLengths,
  size_t pathCount)
{
  Device* device = (Device*)hdevice;
  RTC_CATCH_BEGIN;
  RTC_TRACE(rtcNewInstanceExclusion);

  if (device == nullptr) {
    Device::process_error(nullptr, RTC_ERROR_INVALID_ARGUMENT,
                          "rtcNewInstanceExclusion: invalid device");
    return nullptr;
  }

  /* A null table is only legal together with a zero length. */
  if (pathCount != 0 && (paths == nullptr || pathLengths == nullptr)) {
    Device::process_error(device, RTC_ERROR_INVALID_ARGUMENT,
                          "rtcNewInstanceExclusion: null pointer with non-zero path count");
    return nullptr;
  }

  std::vector<std::vector<InstanceExclusion::Edge>> copied;
  copied.reserve(pathCount);

  for (size_t i = 0; i < pathCount; ++i)
  {
    const size_t length = pathLengths[i];

    if (length == 0 || length > (size_t)RTC_MAX_INSTANCE_LEVEL_COUNT) {
      Device::process_error(device, RTC_ERROR_INVALID_ARGUMENT,
                            "rtcNewInstanceExclusion: empty path or path longer than RTC_MAX_INSTANCE_LEVEL_COUNT");
      return nullptr;
    }
    if (paths[i] == nullptr) {
      Device::process_error(device, RTC_ERROR_INVALID_ARGUMENT,
                            "rtcNewInstanceExclusion: null path with non-zero length");
      return nullptr;
    }

    std::vector<InstanceExclusion::Edge> edges;
    edges.reserve(length);
    for (size_t j = 0; j < length; ++j)
    {
      const RTCInstanceExclusionPath& edge = paths[i][j];
      if (edge.geomID == RTC_INVALID_GEOMETRY_ID) {
        Device::process_error(device, RTC_ERROR_INVALID_ARGUMENT,
                              "rtcNewInstanceExclusion: invalid geomID in path");
        return nullptr;
      }
      edges.push_back(InstanceExclusion::Edge{edge.geomID, edge.instPrimID});
    }
    copied.push_back(std::move(edges));
  }

  /* Deep copy complete; from here on the caller arrays are never touched. */
  InstanceExclusion* exclusion = new InstanceExclusion(std::move(copied));
  return (RTCInstanceExclusion)exclusion->refInc();

  RTC_CATCH_END(nullptr);
  return nullptr;
}

RTC_API void rtcRetainInstanceExclusion(RTCInstanceExclusion hexclusion)
{
  InstanceExclusion* exclusion = (InstanceExclusion*)hexclusion;
  RTC_CATCH_BEGIN;
  RTC_TRACE(rtcRetainInstanceExclusion);
  RTC_VERIFY_HANDLE(hexclusion);
  exclusion->refInc();
  RTC_CATCH_END(nullptr);
}

RTC_API void rtcReleaseInstanceExclusion(RTCInstanceExclusion hexclusion)
{
  InstanceExclusion* exclusion = (InstanceExclusion*)hexclusion;
  RTC_CATCH_BEGIN;
  RTC_TRACE(rtcReleaseInstanceExclusion);
  RTC_VERIFY_HANDLE(hexclusion);
  exclusion->refDec();
  RTC_CATCH_END(nullptr);
}
