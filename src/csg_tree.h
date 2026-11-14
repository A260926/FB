// Copyright 2022 The Manifold Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include <mutex>

#include "manifold/manifold.h"
#include "utils.h"

namespace manifold {

enum class CsgNodeType { Union, Intersection, Difference, Leaf };

class CsgLeafNode;
class CsgWarpNode;

// Deferred vertex warp. Holds the user's std::function (and therefore the
// lifetime of any state it captures, including reference captures, which
// remain the caller's responsibility) until the first observation forces the
// owning CsgWarpNode to materialize. Warp invokes the scalar callback once per
// vertex; WarpBatch invokes its callback exactly once with a VecView over the
// whole vertex array.
struct WarpOp {
  enum class Kind { Scalar, Batch, None };
  Kind kind;
  std::function<void(vec3&)> scalar;
  std::function<void(VecView<vec3>)> batch;
};

class CsgNode : public std::enable_shared_from_this<CsgNode> {
 public:
  virtual std::shared_ptr<CsgLeafNode> ToLeafNode(
      ExecutionContext::Impl* ctx = nullptr) const = 0;
  virtual std::shared_ptr<CsgNode> Transform(const mat3x4& m) const = 0;
  virtual CsgNodeType GetNodeType() const = 0;
  // Build a deferred warp stage over this node. The warp is not applied until
  // the returned node is reduced to a leaf by an observation (Status, Volume,
  // GetMeshGL, ...), at which point it runs on the fully materialized input
  // mesh - never pushed through a Boolean onto its operands.
  virtual std::shared_ptr<CsgNode> Warp(WarpOp op) const = 0;
  // Downcast hook used to walk chains of warp stages iteratively without
  // relying on RTTI. Only CsgWarpNode returns itself. The returned pointer is
  // only valid while a shared_ptr keeping the node alive is held.
  virtual CsgWarpNode* AsWarpNode() const { return nullptr; }
  // Move this node's exclusively-owned structural children into `out`, so the
  // caller can destroy a deep tree with an explicit work list instead of
  // recursive destructors. Leaf nodes have nothing to release.
  virtual void ReleaseChildren(
      std::vector<std::shared_ptr<CsgNode>>& /*out*/) {}
  // Append the structural children (without detaching them) for iterative
  // tree walks.
  virtual void CollectChildren(
      std::vector<std::shared_ptr<CsgNode>>& /*out*/) const {}
  // True once lazy evaluation has published a permanent cached result.
  virtual bool IsCached() const { return false; }
  /// Count the leaves in the subtree rooted at this node. A CsgOpNode with
  /// a cached evaluation counts as a single leaf. Used for pre-pass progress
  /// denominator: a CSG tree with N leaves reduces to 1 result in N-1 ops.
  virtual size_t NumLeaves() const = 0;

  virtual std::shared_ptr<CsgNode> Boolean(
      const std::shared_ptr<CsgNode>& second, OpType op);

  std::shared_ptr<CsgNode> Translate(const vec3& t) const;
  std::shared_ptr<CsgNode> Scale(const vec3& s) const;
  std::shared_ptr<CsgNode> Rotate(double xDegrees = 0, double yDegrees = 0,
                                  double zDegrees = 0) const;
};

class CsgLeafNode final : public CsgNode {
 public:
  CsgLeafNode();
  CsgLeafNode(std::shared_ptr<const Manifold::Impl> pImpl_);
  CsgLeafNode(std::shared_ptr<const Manifold::Impl> pImpl_, mat3x4 transform_);
  // mutex_ is not copyable; snapshot the source under its lock.
  CsgLeafNode(const CsgLeafNode& other);

  std::shared_ptr<const Manifold::Impl> GetImpl() const;

  std::shared_ptr<CsgLeafNode> ToLeafNode(
      ExecutionContext::Impl* ctx = nullptr) const override;

  std::shared_ptr<CsgNode> Transform(const mat3x4& m) const override;

  std::shared_ptr<CsgNode> Warp(WarpOp op) const override;

  CsgNodeType GetNodeType() const override;

  size_t NumLeaves() const override { return 1; }

  static std::shared_ptr<CsgLeafNode> Compose(
      const std::vector<std::shared_ptr<CsgLeafNode>>& nodes);

  // Returns the bounding box without triggering eager transform application.
  // This is more efficient for overlap checks when we don't need the full mesh.
  Box GetBoundingBox() const;

  // Returns the number of vertices without triggering eager transform.
  size_t NumVert() const;

 private:
  mutable std::shared_ptr<const Manifold::Impl> pImpl_;
  mutable mat3x4 transform_ = la::identity;
  // Lazy realization mutates these through const accessors on a shared node,
  // so every access takes mutex_.
  mutable std::mutex mutex_;
};

class CsgOpNode final : public CsgNode {
 public:
  CsgOpNode();

  CsgOpNode(const std::vector<std::shared_ptr<CsgNode>>& children, OpType op);

  std::shared_ptr<CsgNode> Boolean(const std::shared_ptr<CsgNode>& second,
                                   OpType op) override;

  std::shared_ptr<CsgNode> Transform(const mat3x4& m) const override;

  std::shared_ptr<CsgNode> Warp(WarpOp op) const override;

  std::shared_ptr<CsgLeafNode> ToLeafNode(
      ExecutionContext::Impl* ctx = nullptr) const override;

  CsgNodeType GetNodeType() const override;

  size_t NumLeaves() const override;

  bool IsCached() const override;

  void ReleaseChildren(std::vector<std::shared_ptr<CsgNode>>& out) override;
  void CollectChildren(
      std::vector<std::shared_ptr<CsgNode>>& out) const override;

  ~CsgOpNode();

 private:
  mutable ConcurrentSharedPtr<std::vector<std::shared_ptr<CsgNode>>> impl_ =
      ConcurrentSharedPtr<std::vector<std::shared_ptr<CsgNode>>>({});
  OpType op_;
  mat3x4 transform_ = la::identity;
  // the following fields are for lazy evaluation, so they are mutable
  mutable std::shared_ptr<CsgLeafNode> cache_ = nullptr;
};

// A single deferred warp stage. The warp callback runs at most once, on the
// whole materialized mesh of `child_`, when the node is first observed. The
// cached result is shared by every Manifold holding (a copy of) this node, so
// copies and concurrent observations never re-run the callback. A fresh Warp
// call builds a fresh node and therefore a fresh stage.
class CsgWarpNode final : public CsgNode {
 public:
  CsgWarpNode(std::shared_ptr<CsgNode> child, WarpOp op);
  ~CsgWarpNode();

  std::shared_ptr<CsgLeafNode> ToLeafNode(
      ExecutionContext::Impl* ctx = nullptr) const override;

  // Affine ops stay deferred and compose above the warp: warp-then-translate
  // and translate-then-warp remain distinct expressions because the new node
  // wraps the existing one rather than merging into it.
  std::shared_ptr<CsgNode> Transform(const mat3x4& m) const override;

  std::shared_ptr<CsgNode> Warp(WarpOp op) const override;

  std::shared_ptr<CsgNode> Boolean(const std::shared_ptr<CsgNode>& second,
                                   OpType op) override;

  CsgWarpNode* AsWarpNode() const override {
    return const_cast<CsgWarpNode*>(this);
  }

  CsgNodeType GetNodeType() const override { return CsgNodeType::Leaf; }

  size_t NumLeaves() const override;

  bool IsCached() const override;

  void ReleaseChildren(std::vector<std::shared_ptr<CsgNode>>& out) override;
  void CollectChildren(
      std::vector<std::shared_ptr<CsgNode>>& out) const override;

  // Snapshot the child pointer under the lock. Used by the iterative tree
  // reducer, which never holds the mutex across a descent.
  std::shared_ptr<CsgNode> Child() const;
  // Apply this single warp stage to the already-reduced child and publish the
  // result. No-op when another observation won the cache race.
  void MaterializeSelf(ExecutionContext::Impl* ctx) const;

 private:
  std::shared_ptr<CsgNode> child_;
  WarpOp op_;
  // Affine transforms composed above this warp stage are folded here and
  // applied to the materialized mesh after the warp callback.
  mat3x4 transform_ = la::identity;
  // The first materialized result. Published under mutex_; a successful
  // materialization is permanent, so later copies and concurrent reads observe
  // it without invoking the callback again.
  mutable std::shared_ptr<CsgLeafNode> cache_;
  mutable std::mutex mutex_;
};

}  // namespace manifold
