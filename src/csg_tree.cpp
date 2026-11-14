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

#if MANIFOLD_PAR == 1
#include <tbb/tbb.h>
#endif

#include <algorithm>
#include <cstdint>

#include "boolean3.h"
#include "csg_tree.h"
#include "execution_impl.h"
#include "impl.h"
#include "mesh_fixes.h"
#include "parallel.h"

namespace {
using namespace manifold;

struct MeshCompare {
  bool operator()(const std::pair<std::shared_ptr<CsgLeafNode>, uint64_t>& a,
                  const std::pair<std::shared_ptr<CsgLeafNode>, uint64_t>& b) {
    // Use NumVert() which doesn't trigger transform application.
    const size_t aVert = a.first->NumVert();
    const size_t bVert = b.first->NumVert();
    if (aVert != bVert) return aVert < bVert;
    // Tie-break by insertion order so heap behavior is deterministic across
    return a.second < b.second;
  }
};

// Drain a work list of exclusively-owned structural children without
// recursion, so destroying a depth-N tree never grows the call stack. A child
// with another owner is left untouched for that owner's own destruction.
void DrainReleased(std::vector<std::shared_ptr<CsgNode>> work) {
  while (!work.empty()) {
    auto node = std::move(work.back());
    work.pop_back();
    if (node.use_count() != 1) continue;
    node->ReleaseChildren(work);
  }
}

}  // namespace
namespace manifold {

std::shared_ptr<CsgNode> CsgNode::Boolean(
    const std::shared_ptr<CsgNode>& second, OpType op) {
  if (second->GetNodeType() != CsgNodeType::Leaf) {
    // "this" is not a CsgOpNode (which overrides Boolean), but if "second" is
    // and the operation is commutative, we let it built the tree.
    if ((op == OpType::Add || op == OpType::Intersect)) {
      return std::static_pointer_cast<CsgOpNode>(second)->Boolean(
          shared_from_this(), op);
    }
  }
  std::vector<std::shared_ptr<CsgNode>> children({shared_from_this(), second});
  return std::make_shared<CsgOpNode>(children, op);
}

std::shared_ptr<CsgNode> CsgNode::Translate(const vec3& t) const {
  mat3x4 transform = la::identity;
  transform[3] += t;
  return Transform(transform);
}

std::shared_ptr<CsgNode> CsgNode::Scale(const vec3& v) const {
  mat3x4 transform;
  for (int i : {0, 1, 2}) transform[i][i] = v[i];
  return Transform(transform);
}

std::shared_ptr<CsgNode> CsgNode::Rotate(double xDegrees, double yDegrees,
                                         double zDegrees) const {
  mat3 rX({1.0, 0.0, 0.0},                        //
          {0.0, cosd(xDegrees), sind(xDegrees)},  //
          {0.0, -sind(xDegrees), cosd(xDegrees)});
  mat3 rY({cosd(yDegrees), 0.0, -sind(yDegrees)},  //
          {0.0, 1.0, 0.0},                         //
          {sind(yDegrees), 0.0, cosd(yDegrees)});
  mat3 rZ({cosd(zDegrees), sind(zDegrees), 0.0},   //
          {-sind(zDegrees), cosd(zDegrees), 0.0},  //
          {0.0, 0.0, 1.0});
  mat3x4 transform(rZ * rY * rX, vec3());
  return Transform(transform);
}

CsgLeafNode::CsgLeafNode() : pImpl_(std::make_shared<Manifold::Impl>()) {}

CsgLeafNode::CsgLeafNode(std::shared_ptr<const Manifold::Impl> pImpl_)
    : pImpl_(pImpl_) {}

CsgLeafNode::CsgLeafNode(std::shared_ptr<const Manifold::Impl> pImpl_,
                         mat3x4 transform_)
    : pImpl_(pImpl_), transform_(transform_) {}

CsgLeafNode::CsgLeafNode(const CsgLeafNode& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  pImpl_ = other.pImpl_;
  transform_ = other.transform_;
}

std::shared_ptr<const Manifold::Impl> CsgLeafNode::GetImpl() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (transform_ == mat3x4(la::identity)) return pImpl_;
  pImpl_ =
      std::make_shared<const Manifold::Impl>(pImpl_->Transform(transform_));
  transform_ = la::identity;
  return pImpl_;
}

std::shared_ptr<CsgLeafNode> CsgLeafNode::ToLeafNode(
    ExecutionContext::Impl*) const {
  return std::make_shared<CsgLeafNode>(*this);
}

std::shared_ptr<CsgNode> CsgLeafNode::Transform(const mat3x4& m) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::make_shared<CsgLeafNode>(pImpl_, m * Mat4(transform_));
}

std::shared_ptr<CsgNode> CsgLeafNode::Warp(WarpOp op) const {
  // Snapshot under the lock once and build a fresh leaf via the two-argument
  // constructor: copying *this here would re-enter the non-recursive mutex.
  // The pending affine transform stays deferred on the new leaf.
  std::shared_ptr<const Manifold::Impl> impl;
  mat3x4 transform;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    impl = pImpl_;
    transform = transform_;
  }
  return std::make_shared<CsgWarpNode>(
      std::make_shared<CsgLeafNode>(impl, transform), std::move(op));
}

CsgNodeType CsgLeafNode::GetNodeType() const { return CsgNodeType::Leaf; }

Box CsgLeafNode::GetBoundingBox() const {
  // Compute transformed bounding box without triggering full mesh transform.
  // This is an approximation - the actual bounding box of the transformed mesh
  // may be tighter, but this is sufficient for overlap checks.
  std::lock_guard<std::mutex> lock(mutex_);
  const Box& box = pImpl_->bBox_;
  if (transform_ == mat3x4(la::identity)) {
    return box;
  }

  // Arvo's algorithm for transforming AABBs efficiently.
  // Instead of transforming all 8 corners, we compute min/max directly
  // from the matrix elements based on their signs.
  vec3 newMin = transform_[3];  // translation component
  vec3 newMax = newMin;

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      const auto a = transform_[j][i] * box.min[j];
      const auto b = transform_[j][i] * box.max[j];
      newMin[i] += std::min(a, b);
      newMax[i] += std::max(a, b);
    }
  }
  return Box{newMin, newMax};
}

size_t CsgLeafNode::NumVert() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pImpl_->NumVert();
}

std::shared_ptr<CsgLeafNode> ImplToLeaf(Manifold::Impl&& impl) {
  return std::make_shared<CsgLeafNode>(
      std::make_shared<Manifold::Impl>(std::move(impl)));
}

// Build a leaf with the given error status — used to short-circuit boolean
// evaluation on cancellation.
std::shared_ptr<CsgLeafNode> ErrorLeaf(Manifold::Error err) {
  Manifold::Impl impl;
  impl.status_ = err;
  return ImplToLeaf(std::move(impl));
}

std::shared_ptr<CsgLeafNode> SimpleBoolean(const Manifold::Impl& a,
                                           const Manifold::Impl& b, OpType op,
                                           ExecutionContext::Impl* ctx) {
  if (IsCancelled(ctx)) return ErrorLeaf(Manifold::Error::Cancelled);
#ifdef MANIFOLD_DEBUG
  auto dump = [&]() {
    dump_lock.lock();
    std::cout << "LHS self-intersecting: " << a.IsSelfIntersecting()
              << std::endl;
    std::cout << "RHS self-intersecting: " << b.IsSelfIntersecting()
              << std::endl;
    if (ManifoldParams().verbose > 3) {
      if (op == OpType::Add)
        std::cout << "Add";
      else if (op == OpType::Intersect)
        std::cout << "Intersect";
      else
        std::cout << "Subtract";
      std::cout << std::endl;
      std::cout << a;
      std::cout << b;
    }
    dump_lock.unlock();
  };
  try {
    Boolean3 boolean(a, b, op, ctx);
    auto impl = boolean.Result(op);
    if (ManifoldParams().selfIntersectionChecks && impl.IsSelfIntersecting()) {
      dump_lock.lock();
      std::cout << "self intersections detected" << std::endl;
      dump_lock.unlock();
      throw logicErr("self intersection detected");
    }
    if (ctx) ctx->doneBooleans.fetch_add(1, std::memory_order_relaxed);
    return ImplToLeaf(std::move(impl));
  } catch (logicErr& err) {
    dump();
    throw err;
  } catch (geometryErr& err) {
    dump();
    throw err;
  }
#else
  auto leaf = ImplToLeaf(Boolean3(a, b, op, ctx).Result(op));
  if (ctx) ctx->doneBooleans.fetch_add(1, std::memory_order_relaxed);
  return leaf;
#endif
}

/**
 * Efficient union of a set of pairwise disjoint meshes.
 */
std::shared_ptr<CsgLeafNode> CsgLeafNode::Compose(
    const std::vector<std::shared_ptr<CsgLeafNode>>& nodes) {
  ZoneScoped;
  double epsilon = -1;
  Box bbox;
  int numVert = 0;
  int numEdge = 0;
  int numTri = 0;
  int numPropVert = 0;
  std::vector<int> vertIndices;
  std::vector<int> edgeIndices;
  std::vector<int> triIndices;
  std::vector<int> propVertIndices;
  int numPropOut = 0;
  for (auto& node : nodes) {
    if (node->pImpl_->status_ != Manifold::Error::NoError) {
      Manifold::Impl impl;
      impl.status_ = node->pImpl_->status_;
      return ImplToLeaf(std::move(impl));
    }
    bbox = bbox.Union(node->GetBoundingBox());

    vertIndices.push_back(numVert);
    edgeIndices.push_back(numEdge * 2);
    triIndices.push_back(numTri);
    propVertIndices.push_back(numPropVert);
    numVert += node->pImpl_->NumVert();
    numEdge += node->pImpl_->NumEdge();
    numTri += node->pImpl_->NumTri();
    const int numProp = node->pImpl_->NumProp();
    numPropOut = std::max(numPropOut, numProp);
    numPropVert +=
        numProp == 0 ? 1 : node->pImpl_->properties_.size() / numProp;
  }

  Manifold::Impl combined;
  combined.bBox_ = bbox;
  combined.epsilon_ = Quality::GetRelativePrecision() * combined.bBox_.Scale();
  combined.vertPos_.resize_nofill(numVert);
  combined.vertNormal_.resize_nofill(numVert);
  combined.halfedge_.resize_nofill(2 * numEdge);
  combined.faceNormal_.resize_nofill(numTri);
  combined.halfedgeTangent_.resize(2 * numEdge);
  combined.meshRelation_.triRef.resize_nofill(numTri);
  if (numPropOut > 0) {
    combined.numProp_ = numPropOut;
    combined.properties_.resize(numPropOut * numPropVert, 0);
  }
  auto policy = autoPolicy(numTri);

  // if we are already parallelizing for each node, do not perform multithreaded
  // copying as it will slightly hurt performance
  if (nodes.size() > 1 && policy == ExecutionPolicy::Par)
    policy = ExecutionPolicy::Seq;

  // Snapshot once and reuse for both shifts; two reads can disagree under
  // cross-thread CSG and leave triRef.meshID values not in meshIDtransform.
  const uint32_t meshIDCounterSnapshot = Manifold::Impl::meshIDCounter_;

  for_each_n(
      nodes.size() > 1 ? ExecutionPolicy::Par : ExecutionPolicy::Seq,
      countAt(0), nodes.size(),
      [&nodes, &vertIndices, &edgeIndices, &triIndices, &propVertIndices,
       numPropOut, &combined, policy, meshIDCounterSnapshot](int i) {
        auto& node = nodes[i];
        copy(node->pImpl_->halfedgeTangent_.begin(),
             node->pImpl_->halfedgeTangent_.end(),
             combined.halfedgeTangent_.begin() + edgeIndices[i]);
        const int nextVert = vertIndices[i];
        const int nextEdge = edgeIndices[i];
        const int nextProp = propVertIndices[i];
        const bool hasProp = node->pImpl_->NumProp() > 0;
        for_each_n(
            policy, countAt(0), node->pImpl_->halfedge_.size(), [&](int edge) {
              const int newEdge = edgeIndices[i] + edge;
              combined.halfedge_.SetStart(
                  newEdge, node->pImpl_->halfedge_.Start(edge) + nextVert);
              combined.halfedge_.SetPair(
                  newEdge, node->pImpl_->halfedge_.Pair(edge) + nextEdge);
              const int propVert =
                  hasProp ? node->pImpl_->halfedge_.Prop(edge) + nextProp
                          : nextProp;
              combined.halfedge_.SetProp(newEdge, propVert);
            });

        if (node->pImpl_->NumProp() > 0) {
          const int numProp = node->pImpl_->NumProp();
          auto& oldProp = node->pImpl_->properties_;
          auto& newProp = combined.properties_;
          for (int p = 0; p < numProp; ++p) {
            auto oldRange =
                StridedRange(oldProp.cbegin() + p, oldProp.cend(), numProp);
            auto newRange = StridedRange(
                newProp.begin() + numPropOut * propVertIndices[i] + p,
                newProp.end(), numPropOut);
            copy(oldRange.begin(), oldRange.end(), newRange.begin());
          }
          // Properties copy above doesn't go through the on-the-fly transform
          // applied to vertPos_/faceNormal_ below; eager-transform slot 0..2
          // per-meshID so world-frame normals stay in sync. Covers mixed
          // input nodes (some meshIDs with hasNormals, some without).
          if (numProp >= 3 && node->transform_ != mat3x4(la::identity)) {
            const mat3 normalTransform =
                la::inverse(la::transpose(mat3(node->transform_)));
            Manifold::Impl::EagerTransformPropNormals(
                node->pImpl_->halfedge_, node->pImpl_->meshRelation_,
                normalTransform, newProp, oldProp.size() / numProp, numPropOut,
                propVertIndices[i]);
          }
        }

        if (node->transform_ == mat3x4(la::identity)) {
          copy(node->pImpl_->vertPos_.begin(), node->pImpl_->vertPos_.end(),
               combined.vertPos_.begin() + vertIndices[i]);
          copy(node->pImpl_->vertNormal_.begin(),
               node->pImpl_->vertNormal_.end(),
               combined.vertNormal_.begin() + vertIndices[i]);
          copy(node->pImpl_->faceNormal_.begin(),
               node->pImpl_->faceNormal_.end(),
               combined.faceNormal_.begin() + triIndices[i]);
        } else {
          // no need to apply the transform to the node, just copy the vertices
          // and face normals and apply transform on the fly
          const mat3x4 transform = node->transform_;
          auto vertPosBegin = TransformIterator(
              node->pImpl_->vertPos_.begin(), [&transform](vec3 position) {
                return transform * vec4(position, 1.0);
              });
          mat3 normalTransform =
              la::inverse(la::transpose(mat3(node->transform_)));
          auto vertNormalBegin =
              TransformIterator(node->pImpl_->vertNormal_.begin(),
                                TransformNormals({normalTransform}));
          auto faceNormalBegin =
              TransformIterator(node->pImpl_->faceNormal_.begin(),
                                TransformNormals({normalTransform}));
          copy_n(vertPosBegin, node->pImpl_->vertPos_.size(),
                 combined.vertPos_.begin() + vertIndices[i]);
          copy_n(vertNormalBegin, node->pImpl_->vertNormal_.size(),
                 combined.vertNormal_.begin() + vertIndices[i]);
          copy_n(faceNormalBegin, node->pImpl_->faceNormal_.size(),
                 combined.faceNormal_.begin() + triIndices[i]);

          const bool invert = la::determinant(mat3(node->transform_)) < 0;
          for_each_n(policy, countAt(0), node->pImpl_->halfedgeTangent_.size(),
                     TransformTangents{combined.halfedgeTangent_,
                                       edgeIndices[i], mat3(node->transform_),
                                       invert, node->pImpl_->halfedgeTangent_,
                                       node->pImpl_->halfedge_});
          if (invert)
            for_each_n(policy, countAt(triIndices[i]), node->pImpl_->NumTri(),
                       FlipTris{combined.halfedge_});
        }
        // Since the nodes may be copies containing the same meshIDs, it is
        // important to add an offset so that each node instance gets
        // unique meshIDs.
        const int offset = i * meshIDCounterSnapshot;
        transform(node->pImpl_->meshRelation_.triRef.begin(),
                  node->pImpl_->meshRelation_.triRef.end(),
                  combined.meshRelation_.triRef.begin() + triIndices[i],
                  [offset](TriRef ref) {
                    ref.meshID += offset;
                    return ref;
                  });
      });

  for (size_t i = 0; i < nodes.size(); i++) {
    auto& node = nodes[i];
    const int offset = i * meshIDCounterSnapshot;

    for (const auto& pair : node->pImpl_->meshRelation_.meshIDtransform) {
      Manifold::Impl::Relation rel = pair.second;
      // Apply the node's transform to the mesh relation if not identity.
      // This is necessary because we may not have called GetImpl() which would
      // have applied the transform to the mesh relations.
      if (node->transform_ != mat3x4(la::identity)) {
        rel.transform = node->transform_ * Mat4(rel.transform);
      }
      combined.meshRelation_.meshIDtransform[pair.first + offset] = rel;
    }
  }

  combined.SortGeometry();
  combined.IncrementMeshIDs();
  return ImplToLeaf(std::move(combined));
}

/**
 * Efficient boolean operation on a set of nodes utilizing commutativity of the
 * operation. Only supports union and intersection.
 */
std::shared_ptr<CsgLeafNode> BatchBoolean(
    OpType operation, std::vector<std::shared_ptr<CsgLeafNode>>& results,
    ExecutionContext::Impl* ctx) {
  ZoneScoped;
  DEBUG_ASSERT(operation != OpType::Subtract, logicErr,
               "BatchBoolean doesn't support Difference.");
  // common cases
  if (results.size() == 0) return std::make_shared<CsgLeafNode>();
  if (results.size() == 1) return results.front();
  if (results.size() == 2)
    return SimpleBoolean(*results[0]->GetImpl(), *results[1]->GetImpl(),
                         operation, ctx);
  std::vector<std::pair<std::shared_ptr<CsgLeafNode>, uint64_t>> heapNodes;
  heapNodes.reserve(results.size());
  for (size_t i = 0; i < results.size(); ++i) {
    heapNodes.emplace_back(std::move(results[i]), i);
  }
  results.clear();
  uint64_t nextSerial = heapNodes.size();

  // apply boolean operations starting from smaller meshes
  // the assumption is that boolean operations on smaller meshes is faster,
  // due to less data being copied and processed
  auto cmpFn = MeshCompare();
  std::make_heap(heapNodes.begin(), heapNodes.end(), cmpFn);
  std::vector<std::pair<std::shared_ptr<CsgLeafNode>, uint64_t>> tmp;
#if MANIFOLD_PAR == 1
  tbb::task_group group;
  // make sure the order of result is deterministic
  std::vector<std::shared_ptr<CsgLeafNode>> parallelTmp;
  std::vector<uint64_t> parallelSerial;
  for (int i = 0; i < 4; i++) parallelTmp.push_back(nullptr);
  for (int i = 0; i < 4; i++) parallelSerial.push_back(0);
#endif
  while (heapNodes.size() > 1) {
    if (IsCancelled(ctx)) return ErrorLeaf(Manifold::Error::Cancelled);
    for (size_t i = 0; i < 4 && heapNodes.size() > 1; i++) {
      std::pop_heap(heapNodes.begin(), heapNodes.end(), cmpFn);
      auto a = std::move(heapNodes.back());
      heapNodes.pop_back();
      std::pop_heap(heapNodes.begin(), heapNodes.end(), cmpFn);
      auto b = std::move(heapNodes.back());
      heapNodes.pop_back();
#if MANIFOLD_PAR == 1
      parallelSerial[i] = nextSerial++;
      group.run([&, i, a = std::move(a.first), b = std::move(b.first)]() {
        parallelTmp[i] =
            SimpleBoolean(*a->GetImpl(), *b->GetImpl(), operation, ctx);
      });
#else
      auto result = SimpleBoolean(*a.first->GetImpl(), *b.first->GetImpl(),
                                  operation, ctx);
      tmp.emplace_back(std::move(result), nextSerial++);
#endif
    }
#if MANIFOLD_PAR == 1
    group.wait();
    for (int i = 0; i < 4 && parallelTmp[i]; i++)
      tmp.emplace_back(std::move(parallelTmp[i]), parallelSerial[i]);
#endif
    for (auto& result : tmp) {
      heapNodes.push_back(std::move(result));
      std::push_heap(heapNodes.begin(), heapNodes.end(), cmpFn);
    }
    tmp.clear();
  }
  return heapNodes.front().first;
}

/**
 * Efficient union operation on a set of nodes by doing Compose as much as
 * possible.
 */
std::shared_ptr<CsgLeafNode> BatchUnion(
    std::vector<std::shared_ptr<CsgLeafNode>>& children,
    ExecutionContext::Impl* ctx) {
  ZoneScoped;
  // INVARIANT: children_ is a vector of leaf nodes
  // this kMaxUnionSize is a heuristic to avoid the pairwise disjoint check
  // with O(n^2) complexity to take too long.
  // If the number of children exceeded this limit, we will operate on chunks
  // with size kMaxUnionSize.
  constexpr size_t kMaxUnionSize = 1000;
  DEBUG_ASSERT(!children.empty(), logicErr,
               "BatchUnion should not have empty children");
  while (children.size() > 1) {
    if (IsCancelled(ctx)) return ErrorLeaf(Manifold::Error::Cancelled);
    const size_t start = (children.size() > kMaxUnionSize)
                             ? (children.size() - kMaxUnionSize)
                             : 0;
    Vec<Box> boxes;
    boxes.reserve(children.size() - start);
    for (size_t i = start; i < children.size(); i++) {
      boxes.push_back(children[i]->GetBoundingBox());
    }
    // partition the children into a set of disjoint sets
    // each set contains a set of children that are pairwise disjoint
    std::vector<Vec<size_t>> disjointSets;
    for (size_t i = 0; i < boxes.size(); i++) {
      auto lambda = [&boxes, i](const Vec<size_t>& set) {
        return std::find_if(set.begin(), set.end(), [&boxes, i](size_t j) {
                 // Children touching within the pair's tie tolerance
                 // are booleaned, not composed.
                 const double tol =
                     Quality::GetRelativePrecision() *
                     std::min(boxes[i].Scale(), boxes[j].Scale());
                 return Box(boxes[i].min - tol, boxes[i].max + tol)
                     .DoesOverlap(boxes[j]);
               }) == set.end();
      };
      auto it = std::find_if(disjointSets.begin(), disjointSets.end(), lambda);
      if (it == disjointSets.end()) {
        disjointSets.push_back(std::vector<size_t>{i});
      } else {
        it->push_back(i);
      }
    }
    // compose each set of disjoint children
    std::vector<std::shared_ptr<CsgLeafNode>> impls;
    for (auto& set : disjointSets) {
      if (set.size() == 1) {
        impls.push_back(children[start + set[0]]);
      } else {
        std::vector<std::shared_ptr<CsgLeafNode>> tmp;
        for (size_t j : set) {
          tmp.push_back(children[start + j]);
        }
        impls.push_back(CsgLeafNode::Compose(tmp));
        // Compose absorbs set.size() leaves into 1 leaf, which is
        // set.size() - 1 leaf-reductions toward the final result.
        if (ctx) {
          const int reductions = static_cast<int>(set.size() - 1);
          ctx->doneBooleans.fetch_add(reductions, std::memory_order_relaxed);
          ctx->donePhases.fetch_add(reductions * kPhasesPerBoolean,
                                    std::memory_order_relaxed);
        }
      }
    }

    children.erase(children.begin() + start, children.end());
    children.push_back(BatchBoolean(OpType::Add, impls, ctx));
    // move it to the front as we process from the back, and the newly added
    // child should be quite complicated
    std::swap(children.front(), children.back());
  }
  return children.front();
}

CsgOpNode::CsgOpNode() {}

CsgOpNode::CsgOpNode(const std::vector<std::shared_ptr<CsgNode>>& children,
                     OpType op)
    : impl_(children), op_(op) {}

CsgOpNode::~CsgOpNode() {
  // Iteratively release an exclusively-owned subtree so a deep
  // Boolean/warp expression does not unwind through recursive destructors.
  std::vector<std::shared_ptr<CsgNode>> work;
  ReleaseChildren(work);
  DrainReleased(std::move(work));
}

bool CsgOpNode::IsCached() const { return cache_ != nullptr; }

void CsgOpNode::ReleaseChildren(
    std::vector<std::shared_ptr<CsgNode>>& out) {
  // A shared impl_ backs another live CsgOpNode that may still evaluate it, so
  // only detach when we are its sole holder.
  if (impl_.UseCount() != 1) return;
  auto guard = impl_.GetGuard();
  for (auto& child : *guard) out.push_back(std::move(child));
  guard->clear();
}

void CsgOpNode::CollectChildren(
    std::vector<std::shared_ptr<CsgNode>>& out) const {
  auto guard = impl_.GetGuard();
  for (const auto& child : *guard) out.push_back(child);
}

std::shared_ptr<CsgNode> CsgOpNode::Boolean(
    const std::shared_ptr<CsgNode>& second, OpType op) {
  std::vector<std::shared_ptr<CsgNode>> children;
  children.push_back(shared_from_this());
  children.push_back(second);

  return std::make_shared<CsgOpNode>(children, op);
}

std::shared_ptr<CsgNode> CsgOpNode::Transform(const mat3x4& m) const {
  auto node = std::make_shared<CsgOpNode>();
  node->impl_ = impl_;
  node->transform_ = m * Mat4(transform_);
  node->op_ = op_;
  return node;
}

std::shared_ptr<CsgNode> CsgOpNode::Warp(WarpOp op) const {
  // The warp stays above the Boolean: it is not pushed down onto the operands.
  // ToLeafNode() materializes the whole boolean mesh (applying this node's
  // deferred affine) before the warp stage runs.
  return std::make_shared<CsgWarpNode>(
      std::const_pointer_cast<CsgNode>(shared_from_this()), std::move(op));
}

CsgWarpNode::CsgWarpNode(std::shared_ptr<CsgNode> child, WarpOp op)
    : child_(std::move(child)), op_(std::move(op)) {}

namespace {
// Post-order, fully iterative reduction of a subtree to cached leaves. Warp
// stages are applied bottom-up and Boolean nodes only enter their own
// (iterative) evaluator once every warp stage beneath them is already cached,
// which keeps alternating Boolean/warp chains from mutually recursing.
struct ReduceFrame {
  std::shared_ptr<CsgNode> node;
  std::vector<std::shared_ptr<CsgNode>> kids;
  size_t idx = 0;
};

void ReduceTree(const std::shared_ptr<CsgNode>& root,
                ExecutionContext::Impl* ctx) {
  if (root->IsCached()) return;
  std::vector<ReduceFrame> stack;
  stack.push_back({root, {}, 0});
  while (!stack.empty()) {
    ReduceFrame& frame = stack.back();
    CsgWarpNode* warp = frame.node->AsWarpNode();
    if (warp != nullptr) {
      if (frame.idx == 0) {
        // First visit: descend into the single child before warping.
        frame.idx = 1;
        if (!warp->IsCached()) {
          stack.push_back({warp->Child(), {}, 0});
        }
        continue;
      }
      // The child is fully reduced; apply this stage once. A concurrent
      // reduction that won the cache race makes this a no-op.
      warp->MaterializeSelf(ctx);
      stack.pop_back();
      continue;
    }
    if (frame.node->GetNodeType() == CsgNodeType::Leaf) {
      // An ordinary materialized leaf has nothing to reduce.
      stack.pop_back();
      continue;
    }
    if (frame.node->IsCached()) {
      // A Boolean already evaluated: its result is permanent.
      stack.pop_back();
      continue;
    }
    // Boolean node: reduce every child first, then run its stack-machine
    // evaluator, whose warp callouts now hit cached stages only.
    if (frame.kids.empty()) frame.node->CollectChildren(frame.kids);
    if (frame.idx < frame.kids.size()) {
      auto child = frame.kids[frame.idx++];
      if (!child->IsCached()) stack.push_back({child, {}, 0});
      continue;
    }
    frame.node->ToLeafNode(ctx);
    stack.pop_back();
  }
}
}  // namespace

CsgWarpNode::~CsgWarpNode() {
  // Unwind an exclusively-owned chain (including alternating Boolean nodes)
  // iteratively: an unobserved depth-10000 expression must not consume call
  // stack when destroyed.
  std::vector<std::shared_ptr<CsgNode>> work;
  ReleaseChildren(work);
  DrainReleased(std::move(work));
}

std::shared_ptr<CsgNode> CsgWarpNode::Transform(const mat3x4& m) const {
  // The affine composes *above* the warp (warp-then-translate). The wrapper
  // carries no warp callback of its own (WarpOp::Kind::None), so observing it
  // only reduces the anchored warp expression and applies the matrix - it
  // never re-runs a warp. Chained affines re-anchor to the same warp node and
  // merely accumulate matrices, keeping warp-then-translate distinct from
  // translate-then-warp.
  WarpOp none;
  none.kind = WarpOp::Kind::None;
  std::shared_ptr<CsgNode> anchor =
      op_.kind == WarpOp::Kind::None ? child_
                                     : std::const_pointer_cast<CsgNode>(
                                           shared_from_this());
  auto node = std::make_shared<CsgWarpNode>(std::move(anchor), std::move(none));
  node->transform_ = m * Mat4(transform_);
  return node;
}

std::shared_ptr<CsgNode> CsgWarpNode::Warp(WarpOp op) const {
  // A fresh Warp is a fresh stage, even when stacked on another warp.
  return std::make_shared<CsgWarpNode>(
      std::const_pointer_cast<CsgNode>(shared_from_this()), std::move(op));
}

std::shared_ptr<CsgNode> CsgWarpNode::Boolean(
    const std::shared_ptr<CsgNode>& second, OpType op) {
  // CsgNode's base implementation would try to reinterpret a non-leaf second
  // operand as a CsgOpNode; a warp stage is not one. Always build a fresh op
  // node, keeping the warp stage intact as a Boolean child.
  std::vector<std::shared_ptr<CsgNode>> children({shared_from_this(), second});
  return std::make_shared<CsgOpNode>(children, op);
}

size_t CsgWarpNode::NumLeaves() const {
  // Iterative walk: deep warp chains must not recurse here either.
  const CsgWarpNode* node = this;
  for (;;) {
    if (node->IsCached()) return 1;
    CsgWarpNode* inner = node->child_->AsWarpNode();
    if (inner == nullptr) return node->child_->NumLeaves();
    node = inner;
  }
}

bool CsgWarpNode::IsCached() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cache_ != nullptr;
}

void CsgWarpNode::ReleaseChildren(
    std::vector<std::shared_ptr<CsgNode>>& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (child_ != nullptr) out.push_back(std::move(child_));
}

void CsgWarpNode::CollectChildren(
    std::vector<std::shared_ptr<CsgNode>>& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  out.push_back(child_);
}

std::shared_ptr<CsgNode> CsgWarpNode::Child() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return child_;
}

std::shared_ptr<CsgLeafNode> CsgWarpNode::ToLeafNode(
    ExecutionContext::Impl* ctx) const {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_ != nullptr) return cache_;
  }
  // Iterative reduction of the whole subtree below this stage.
  ReduceTree(std::const_pointer_cast<CsgNode>(shared_from_this()), ctx);
  std::lock_guard<std::mutex> lock(mutex_);
  return cache_;
}

void CsgWarpNode::MaterializeSelf(ExecutionContext::Impl* ctx) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cache_ != nullptr) return;

  // The child has already been reduced by ReduceTree, so this only reads a
  // cached leaf.
  auto srcLeaf = child_->ToLeafNode(ctx);

  if (op_.kind == WarpOp::Kind::None) {
    // An affine-only wrapper: no callback, no mesh copy. Defer the matrix on
    // the returned leaf exactly like the plain affine path.
    cache_ = std::make_shared<CsgLeafNode>(srcLeaf->GetImpl(), transform_);
    return;
  }

  // An errored source skips the callback; its status propagates and every
  // later stage skips as well.
  auto srcImpl = srcLeaf->GetImpl();
  auto impl = std::make_shared<Manifold::Impl>(*srcImpl);
  if (srcImpl->status_ == Manifold::Error::NoError) {
    if (op_.kind == WarpOp::Kind::Scalar) {
      impl->Warp(op_.scalar);
    } else {
      // One callback for the whole vertex array - never split per vertex.
      impl->WarpBatch(op_.batch);
    }
  }

  auto result = std::make_shared<CsgLeafNode>(impl);
  if (transform_ != mat3x4(la::identity)) {
    result = std::static_pointer_cast<CsgLeafNode>(
        result->Transform(transform_));
  }
  cache_ = result;
}

struct CsgStackFrame {
  using Nodes = std::vector<std::shared_ptr<CsgLeafNode>>;

  bool finalize;
  OpType parent_op;
  mat3x4 transform;
  Nodes* positive_dest;
  Nodes* negative_dest;
  std::shared_ptr<const CsgOpNode> op_node;
  Nodes positive_children;
  Nodes negative_children;

  CsgStackFrame(bool finalize, OpType parent_op, mat3x4 transform,
                Nodes* positive_dest, Nodes* negative_dest,
                std::shared_ptr<const CsgOpNode> op_node)
      : finalize(finalize),
        parent_op(parent_op),
        transform(transform),
        positive_dest(positive_dest),
        negative_dest(negative_dest),
        op_node(op_node) {}
};

std::shared_ptr<CsgLeafNode> CsgOpNode::ToLeafNode(
    ExecutionContext::Impl* ctx) const {
  ZoneScoped;
  {
    // cache_ is published under impl_'s guard below; read it under the same
    // lock so a concurrent eval of a shared op node can't tear it.
    auto guard = impl_.GetGuard();
    if (cache_ != nullptr) return cache_;
  }

  // Note: We do need a pointer here to avoid vector pointers from being
  // invalidated after pushing elements into the explicit stack.
  // It is a `shared_ptr` because we may want to drop the stack frame while
  // still referring to some of the elements inside the old frame.
  // It is possible to use `unique_ptr`, extending the lifetime of the frame
  // when we remove it from the stack, but it is a bit more complicated and
  // there is no measurable overhead from using `shared_ptr` here...
  std::vector<std::shared_ptr<CsgStackFrame>> stack;
  // initial node, positive_dest is a nullptr because we don't need to put the
  // result anywhere else (except in the cache_).
  stack.push_back(std::make_shared<CsgStackFrame>(
      false, op_, la::identity, nullptr, nullptr,
      std::static_pointer_cast<const CsgOpNode>(shared_from_this())));

  // Instead of actually using recursion in the algorithm, we use an explicit
  // stack, do DFS and store the intermediate states into `CsgStackFrame` to
  // avoid stack overflow.
  //
  // Before performing boolean operations, we should make sure that all children
  // are `CsgLeafNodes`, i.e. are actual meshes that can be operated on. Hence,
  // we do it in two steps:
  // 1. Populate `children` (`positive_children` and `negative_children`)
  //    If the child is a `CsgOpNode`, we either collapse it or compute its
  //    boolean operation result.
  // 2. Performs boolean after populating the `children` set.
  //    After a boolean operation is completed, we put the result back to its
  //    parent's `children` set.
  //
  // When we populate `children`, we perform collapsing on-the-fly.
  // For example, we want to turn `(Union a (Union b c))` into `(Union a b c)`.
  // This allows more efficient `BatchBoolean`/`BatchUnion` calls.
  // We can do this when the child operation is the same as the parent
  // operation, except when the operation is `Subtract` (see below).
  // Note that to avoid repeating work, we will not collapse nodes that are
  // reused. And in the special case where the children set only contains one
  // element, we don't need any operation, so we can collapse that as well.
  // Instead of moving `b` and `c` into the parent, and running this collapsing
  // check until a fixed point, we remember the `positive_dest` where we should
  // put the `CsgLeafNode` into. Normally, the `positive_dest` pointer point to
  // the parent `children` set. However, when a child is being collapsed, we
  // keep using the old `positive_dest` pointer for the grandchildren. Hence,
  // removing a node by collapsing takes O(1) time. We also need to store the
  // parent operation type for checking if the node is eligible for collapsing,
  // and transform matrix because we need to re-apply the transformation to the
  // children.
  //
  // `Subtract` is handled differently from `Add` and `Intersect`.
  // For the first operand, it is treated as normal subtract. Negative children
  // in this operand is propagated to the parent, which is equivalent to
  // collapsing `(a - b) - c` into `a - (b + c)`.
  // For the remaining operands, they are treated as a nested `Add` node,
  // collapsing `a - (b + (c + d))` into `a - (b + c + d)`.
  //
  // `impl` should always contain either the raw set of children or
  // the NOT transformed result, while `cache_` should contain the transformed
  // result. This is because `impl` can be shared between `CsgOpNode` that
  // differ in `transform_`, so we want it to be able to share the result.
  // ===========================================================================
  // Recursive version (pseudocode only):
  //
  // void f(CsgOpNode node, OpType parent_op, mat3x4 transform,
  //        Nodes *positive_dest, Nodes *negative_dest) {
  //   auto impl = node->impl_.GetGuard();
  //   // can collapse when we have the same operation as the parent and is
  //   // unique, or when we have only one children.
  //   const OpType op = node->op_;
  //   const bool canCollapse = (op == parent_op && IsUnique(node)) ||
  //                            impl->size() == 1;
  //   const mat3x4 transform2 = canCollapse ? transform * node->transform_
  //                                         : la::identity;
  //   Nodes positive_children, negative_children;
  //   Nodes* pos_dest = canCollapse ? positive_dest : &positive_children;
  //   Nodes* neg_dest = canCollapse ? negative_dest : &negative_children;
  //   for (size_t i = 0; i < impl->size(); i++) {
  //     auto child = (*impl)[i];
  //     const bool negative = op == OpType::Subtract && i != 0;
  //     Nodes *dest1 = negative ? neg_dest : pos_dest;
  //     Nodes *dest2 = (op == OpType::Subtract && i == 0) ?
  //                      neg_dest : nullptr;
  //     if (child->GetNodeType() == CsgNodeType::Leaf)
  //       dest1.push_back(child);
  //     else
  //       f(child, op, transform2, dest1, dest2);
  //   }
  //   if (canCollapse) return;
  //   if (node->op_ == OpType::Add)
  //     *impl = {BatchUnion(positive_children)};
  //   else if (node->op_ == OpType::Intersect)
  //     *impl = {BatchBoolean(Intersect, positive_children)};
  //   else // subtract
  //     *impl = { BatchUnion(positive_children) -
  //                         BatchUnion(negative_children)};
  //   // node local transform
  //   node->cache_ = (*impl)[0].Transform(node.transform);
  //   // collapsed node transforms
  //   if (destination)
  //     destination->push_back(node->cache_->Transform(transform));
  // }
  while (!stack.empty()) {
    if (IsCancelled(ctx)) {
      // Poison every op_node currently on the stack, not just `this`.
      // Sub-ops may have had their impl_ partially mutated during finalize
      // (children replaced with an intermediate result); leaving cache_
      // unset would let a later evaluation of a shared sub-op run against
      // a partially-reduced tree. Shared cache ensures any subsequent
      // eval of any op on the stack returns Cancelled.
      auto cancelled = ErrorLeaf(Manifold::Error::Cancelled);
      for (auto& frame : stack) {
        if (!frame->op_node->cache_) frame->op_node->cache_ = cancelled;
      }
      cache_ = cancelled;
      return cache_;
    }
    std::shared_ptr<CsgStackFrame> frame = stack.back();
    auto impl = frame->op_node->impl_.GetGuard();
    if (frame->finalize) {
      if (!frame->op_node->cache_) {
        switch (frame->op_node->op_) {
          case OpType::Add:
            *impl = {BatchUnion(frame->positive_children, ctx)};
            break;
          case OpType::Intersect: {
            *impl = {
                BatchBoolean(OpType::Intersect, frame->positive_children, ctx)};
            break;
          };
          case OpType::Subtract:
            if (frame->positive_children.empty()) {
              // nothing to subtract from, so the result is empty.
              *impl = {std::make_shared<CsgLeafNode>()};
            } else {
              auto positive = BatchUnion(frame->positive_children, ctx);
              if (frame->negative_children.empty()) {
                // nothing to subtract, result equal to the LHS.
                *impl = {frame->positive_children[0]};
              } else {
                auto negative = BatchUnion(frame->negative_children, ctx);
                *impl = {SimpleBoolean(*positive->GetImpl(),
                                       *negative->GetImpl(), OpType::Subtract,
                                       ctx)};
              }
            }
            break;
        }
        frame->op_node->cache_ = std::static_pointer_cast<CsgLeafNode>(
            (*impl)[0]->Transform(frame->op_node->transform_));
      }
      if (frame->positive_dest != nullptr)
        frame->positive_dest->push_back(std::static_pointer_cast<CsgLeafNode>(
            frame->op_node->cache_->Transform(frame->transform)));
      stack.pop_back();
    } else {
      auto add_children =
          [&](std::shared_ptr<CsgNode>& node, OpType op, mat3x4 transform,
              CsgStackFrame::Nodes* dest1, CsgStackFrame::Nodes* dest2) {
            if (node->GetNodeType() == CsgNodeType::Leaf) {
              std::shared_ptr<CsgLeafNode> leaf;
              if (node->AsWarpNode() != nullptr) {
                // A warp stage reports Leaf but is not a CsgLeafNode:
                // materialize it first, so the warp runs on the complete mesh
                // rather than being pushed onto the Boolean operands.
                leaf = node->ToLeafNode(ctx);
                if (transform != mat3x4(la::identity))
                  leaf = std::static_pointer_cast<CsgLeafNode>(
                      leaf->Transform(transform));
              } else {
                leaf = std::static_pointer_cast<CsgLeafNode>(
                    node->Transform(transform));
              }
              dest1->push_back(std::move(leaf));
            } else
              stack.push_back(std::make_shared<CsgStackFrame>(
                  false, op, transform, dest1, dest2,
                  std::static_pointer_cast<const CsgOpNode>(node)));
          };
      // op_node use_count == 2 because it is both inside one CsgOpNode
      // and in our stack.
      // if there is only one child, we can also collapse.
      const OpType op = frame->op_node->op_;
      const bool canCollapse =
          frame->positive_dest != nullptr &&
          ((op == frame->parent_op && frame->op_node.use_count() <= 2 &&
            frame->op_node->impl_.UseCount() == 1) ||
           impl->size() == 1);
      if (canCollapse)
        stack.pop_back();
      else
        frame->finalize = true;

      const mat3x4 transform =
          canCollapse ? (frame->transform * Mat4(frame->op_node->transform_))
                      : la::identity;
      CsgStackFrame::Nodes* pos_dest =
          canCollapse ? frame->positive_dest : &frame->positive_children;
      CsgStackFrame::Nodes* neg_dest =
          canCollapse ? frame->negative_dest : &frame->negative_children;
      for (size_t i = 0; i < impl->size(); i++) {
        const bool negative = op == OpType::Subtract && i != 0;
        CsgStackFrame::Nodes* dest1 = negative ? neg_dest : pos_dest;
        CsgStackFrame::Nodes* dest2 =
            (op == OpType::Subtract && i == 0) ? neg_dest : nullptr;
        add_children((*impl)[i], negative ? OpType::Add : op, transform, dest1,
                     dest2);
      }
    }
  }
  return cache_;
}

CsgNodeType CsgOpNode::GetNodeType() const {
  switch (op_) {
    case OpType::Add:
      return CsgNodeType::Union;
    case OpType::Subtract:
      return CsgNodeType::Difference;
    case OpType::Intersect:
      return CsgNodeType::Intersection;
  }
  // unreachable...
  return CsgNodeType::Leaf;
}

size_t CsgOpNode::NumLeaves() const {
  // An already-evaluated CsgOpNode counts as a single leaf for the purposes
  // of estimating remaining boolean work. Iterative walk: `+=` chains can
  // produce very deep CsgOpNode trees that would blow the call stack if
  // this were recursive.
  if (cache_ != nullptr) return 1;
  size_t total = 0;
  std::vector<const CsgOpNode*> stack;
  stack.push_back(this);
  while (!stack.empty()) {
    const CsgOpNode* op = stack.back();
    stack.pop_back();
    if (op->cache_ != nullptr) {
      total += 1;
      continue;
    }
    auto impl = op->impl_.GetGuard();
    for (const auto& child : *impl) {
      if (child->GetNodeType() == CsgNodeType::Leaf) {
        total += 1;
      } else {
        stack.push_back(static_cast<const CsgOpNode*>(child.get()));
      }
    }
  }
  return total;
}

}  // namespace manifold
