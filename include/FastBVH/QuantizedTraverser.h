#pragma once

#include <FastBVH/BBox.h>
#include <FastBVH/Intersection.h>
#include <FastBVH/QuantizedBVH.h>
#include <FastBVH/Ray.h>
#include <FastBVH/Traverser.h>

#include <cstdint>
#include <utility>

namespace FastBVH {

//! \brief Used for traversing a @ref QuantizedBVH and checking for ray-primitive
//! intersections. Mirrors the interface of @ref Traverser.
//!
//! The traversal reads only the compressed tree: bounding boxes are decoded
//! on the fly, level by level, and the only floating point bounds alive at
//! any moment are the ones carried by the per-call working stack. The whole
//! tree is never decompressed into a floating point cache.
//!
//! \tparam Float The floating point type used by vector components.
//! \tparam Primitive The type of the primitive that the BVH was made with.
//! \tparam Intersector The type of the primitive intersector.
//! \tparam Flags The flags which configure traversal. See @ref TraverserFlags. By default, no flags are active.
template <typename Float, typename Primitive, typename Intersector, TraverserFlags Flags = TraverserFlags(0)>
class QuantizedTraverser final {
  //! The quantized BVH to be traversed.
  const QuantizedBVH<Float, Primitive>& bvh;

  /// The ray-primitive intersector.
  Intersector intersector;

 public:
  //! Constructs a new quantized BVH traverser.
  //! \param bvh_ The quantized BVH to be traversed.
  //! \param intersector_ The ray-primitive intersector.
  constexpr QuantizedTraverser(const QuantizedBVH<Float, Primitive>& bvh_, const Intersector& intersector_) noexcept
      : bvh(bvh_), intersector(intersector_) {}
  //! Traces a single ray throughout the quantized BVH, getting the closest intersection.
  //! \param ray The ray to be traced.
  //! \return An intersection instance.
  //! It may or may not be valid, based on whether or not the ray made a collision.
  Intersection<Float, Primitive> traverse(const Ray<Float>& ray) const;
};

//! \brief Contains implementation details for the @ref QuantizedTraverser class.
namespace QuantizedTraverserImpl {

//! \brief Node for storing state information during traversal.
//!
//! Each entry carries the decoded bounds of its node, so children can be
//! decoded against the actually-decoded parent box while traversing.
template <typename Float>
struct Traversal final {
  //! The index of the node to be traversed.
  uint32_t i;

  //! Minimum hit time for this node.
  Float mint;

  //! The decoded bounding box of the node.
  BBox<Float> bbox;

  //! Constructs an uninitialized traversal context.
  Traversal() noexcept {}

  //! Constructs an initialized traversal context.
  //! \param i_ The index of the node to be traversed.
  //! \param mint_ The minimum hit time of the node.
  //! \param bbox_ The decoded bounding box of the node.
  Traversal(uint32_t i_, Float mint_, const BBox<Float>& bbox_) noexcept : i(i_), mint(mint_), bbox(bbox_) {}
};

}  // namespace QuantizedTraverserImpl

template <typename Float, typename Primitive, typename Intersector, TraverserFlags Flags>
Intersection<Float, Primitive> QuantizedTraverser<Float, Primitive, Intersector, Flags>::traverse(
    const Ray<Float>& ray) const {
  using Traversal = QuantizedTraverserImpl::Traversal<Float>;

  // Intersection result
  Intersection<Float, Primitive> intersection;

  const auto nodes = bvh.getNodes();

  // An empty tree cannot be intersected with anything.
  if (nodes.size() == 0) {
    return intersection;
  }

  const auto build_prims = bvh.getPrimitives();

  // Bounding box min-t/max-t for left/right children at some point in the tree
  Float bbhits[4];
  int32_t closer, other;

  // Working set, held per call. It carries the decoded boxes of the
  // pending nodes; no tree-wide floating point cache is ever built.
  Traversal todo[64];
  int32_t stackptr = -1;

  // Test the stored root bounds first; this is also what keeps
  // well-separated scenes cheap for rays that miss everything.
  Float root_tnear, root_tfar;
  const BBox<Float> root_bounds = bvh.getRootBounds();
  if (!root_bounds.intersect(ray, &root_tnear, &root_tfar)) {
    return intersection;
  }

  // "Push" the root node on the working set
  todo[++stackptr] = Traversal(0, root_tnear, root_bounds);

  while (stackptr >= 0) {
    // Pop off the next node to work on.
    const Traversal cur = todo[stackptr];
    stackptr--;

    const auto& node(nodes[cur.i]);

    // If this node is further than the closest found intersection, continue
    if (cur.mint > intersection.t) continue;

    // Is leaf -> Intersect
    if (node.isLeaf()) {
      for (uint32_t o = 0; o < node.primitive_count; ++o) {
        const auto& obj = build_prims[node.start + o];

        auto current = intersector(obj, ray);
        if (current) {
          // If we're only looking for occlusion, then any hit is good enough to return true
          if (Flags & TraverserFlags::OnlyTestOcclusion) {
            return current;
          }
          intersection = closest(intersection, current);
        }
      }

    } else {  // Not a leaf

      // Decode the children against the actually-decoded box of this node.
      const BBox<Float> bbox0 = decodeQuantizedBounds(cur.bbox, nodes[cur.i + 1]);
      const BBox<Float> bbox1 = decodeQuantizedBounds(cur.bbox, nodes[cur.i + node.right_offset]);

      bool hitc0 = bbox0.intersect(ray, bbhits, bbhits + 1);
      bool hitc1 = bbox1.intersect(ray, bbhits + 2, bbhits + 3);

      // Did we hit both nodes?
      if (hitc0 && hitc1) {
        // We assume that the left child is a closer hit...
        closer = cur.i + 1;
        other = cur.i + node.right_offset;

        BBox<Float> closer_bbox = bbox0;
        BBox<Float> other_bbox = bbox1;

        // ... If the right child was actually closer, swap the relavent values.
        if (bbhits[2] < bbhits[0]) {
          std::swap(bbhits[0], bbhits[2]);
          std::swap(bbhits[1], bbhits[3]);
          std::swap(closer, other);
          std::swap(closer_bbox, other_bbox);
        }

        // It's possible that the nearest object is still in the other side, but
        // we'll check the further-awar node later...

        // Push the farther first
        todo[++stackptr] = Traversal(other, bbhits[2], other_bbox);

        // And now the closer (with overlap test)
        todo[++stackptr] = Traversal(closer, bbhits[0], closer_bbox);
      }

      else if (hitc0) {
        todo[++stackptr] = Traversal(cur.i + 1, bbhits[0], bbox0);
      }

      else if (hitc1) {
        todo[++stackptr] = Traversal(cur.i + node.right_offset, bbhits[2], bbox1);
      }
    }
  }

  return intersection;
}

}  // namespace FastBVH
