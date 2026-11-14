#pragma once

#include <FastBVH/BBox.h>
#include <FastBVH/BVH.h>
#include <FastBVH/Iterable.h>
#include <FastBVH/Vector3.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace FastBVH {

//! \brief A BVH node with a quantized bounding box.
//!
//! The bounding box of each node is stored as 16-bit grid
//! coordinates per axis, relative to the *decoded* bounds of
//! the parent node. Together with the layout fields this takes
//! 24 bytes per node, instead of the full-precision @ref Node.
//!
//! The layout fields (`start`, `primitive_count`, `right_offset`)
//! are copied verbatim from the source tree, so leaf slots and
//! child offsets keep the exact same meaning they had in the
//! original @ref BVH.
struct QuantizedNode final {
  //! Quantized minimum point, one grid coordinate per axis.
  uint16_t min[3];

  //! Quantized maximum point, one grid coordinate per axis.
  uint16_t max[3];

  //! The index of the first primitive.
  uint32_t start;

  //! The number of primitives in this node.
  uint32_t primitive_count;

  //! Number of elements to skip in the flattened tree to get to a left child's sibling.
  //! (Node+1 == Node's left child , Node + right_offset == Node's right child)
  uint32_t right_offset;

  //! Indicates if this node is a leaf node.
  //! \return True if this node is a leaf node, false otherwise.
  inline constexpr bool isLeaf() const noexcept { return right_offset == 0; }
};

static_assert(sizeof(QuantizedNode) <= 24, "QuantizedNode must not exceed 24 bytes");

//! \brief Contains implementation details for quantized BVH encoding.
namespace QuantizedBVHImpl {

//! The number of the last grid point on each axis.
//! Grid coordinates range from 0 to this value, inclusive.
constexpr uint32_t kGridMax = 65535;

//! \brief Decodes a single quantized coordinate on a single axis.
//!
//! Grid point `q` corresponds to `parent_min + (parent_max - parent_min) * q / 65535`.
//! The two endpoints are mapped exactly onto the parent corners so that
//! repeated decoding over many levels cannot drift inwards, and a zero-width
//! parent axis simply keeps its coordinate (no division ever happens here).
//!
//! \param parent_min The decoded parent minimum on this axis.
//! \param parent_max The decoded parent maximum on this axis.
//! \param q The quantized coordinate, in [0, 65535].
//! \return The decoded floating point coordinate.
template <typename Float>
Float decodeAxisCoord(Float parent_min, Float parent_max, uint32_t q) noexcept {
  if (q == 0) {
    return parent_min;
  }
  if (q == kGridMax) {
    return parent_max;
  }
  const Float span = parent_max - parent_min;
  // For a zero-width parent axis, span == 0 and this evaluates to parent_min.
  return parent_min + span * (static_cast<Float>(q) / static_cast<Float>(kGridMax));
}

//! \brief Encodes one axis of a source box against the decoded parent range.
//!
//! The emitted grid coordinates are outward-rounded: decoding them always
//! yields an interval that covers `[lo, hi]`. Small fixup loops compensate
//! for floating point rounding so the result is the tightest covering pair
//! of grid points.
//!
//! \param lo The source box minimum on this axis.
//! \param hi The source box maximum on this axis.
//! \param parent_min The decoded parent minimum on this axis.
//! \param parent_max The decoded parent maximum on this axis.
//! \param out_min Receives the quantized minimum.
//! \param out_max Receives the quantized maximum.
template <typename Float>
void encodeAxis(Float lo, Float hi, Float parent_min, Float parent_max, uint16_t& out_min,
                uint16_t& out_max) noexcept {
  const Float span = parent_max - parent_min;
  if (!(span > 0)) {
    // Zero-width parent axis: keep the coordinate, never divide by zero.
    out_min = 0;
    out_max = 0;
    return;
  }

  const Float grid = static_cast<Float>(kGridMax);

  Float t0 = (lo - parent_min) / span * grid;
  Float t1 = (hi - parent_min) / span * grid;

  // Clamp into range; this also maps any NaN to the nearest endpoint.
  if (!(t0 > 0)) {
    t0 = 0;
  } else if (t0 > grid) {
    t0 = grid;
  }
  if (!(t1 > 0)) {
    t1 = 0;
  } else if (t1 > grid) {
    t1 = grid;
  }

  uint32_t q0 = static_cast<uint32_t>(t0);  // truncation == floor for t0 >= 0
  uint32_t q1 = static_cast<uint32_t>(std::ceil(t1));

  // Outward-rounding fixups: pick the largest q0 whose decode is <= lo ...
  while (q0 > 0 && decodeAxisCoord(parent_min, parent_max, q0) > lo) {
    --q0;
  }
  while (q0 < kGridMax && decodeAxisCoord(parent_min, parent_max, q0 + 1) <= lo) {
    ++q0;
  }
  // ... and the smallest q1 whose decode is >= hi.
  while (q1 < kGridMax && decodeAxisCoord(parent_min, parent_max, q1) < hi) {
    ++q1;
  }
  while (q1 > 0 && decodeAxisCoord(parent_min, parent_max, q1 - 1) >= hi) {
    --q1;
  }

  out_min = static_cast<uint16_t>(q0);
  out_max = static_cast<uint16_t>(q1);
}

}  // namespace QuantizedBVHImpl

//! \brief Decodes the bounding box of a quantized node.
//!
//! On every axis, grid point `q` corresponds to
//! `parent.min + (parent.max - parent.min) * q / 65535`, with the endpoints
//! mapped exactly onto the parent corners. Decoding is allowed to round
//! outwards; the result always covers the closed source box of the node.
//!
//! \param parent The *decoded* bounds of the parent node.
//! For the root node this is the stored root bounds
//! (see @ref QuantizedBVH::getRootBounds).
//! \param node The quantized node to decode.
//! \return The decoded bounding box of @p node.
template <typename Float>
BBox<Float> decodeQuantizedBounds(const BBox<Float>& parent, const QuantizedNode& node) noexcept {
  BBox<Float> out;
  for (unsigned axis = 0; axis < 3; ++axis) {
    out.min[axis] = QuantizedBVHImpl::decodeAxisCoord(parent.min[axis], parent.max[axis], node.min[axis]);
    out.max[axis] = QuantizedBVHImpl::decodeAxisCoord(parent.min[axis], parent.max[axis], node.max[axis]);
  }
  out.extent = out.max - out.min;
  return out;
}

//! \brief A memory-efficient, read-only BVH made of quantized nodes.
//!
//! This is exported from an already-built @ref BVH. The exported tree owns
//! only the quantized node array plus one full-precision copy of the root
//! bounds; it never keeps the original floating point nodes or a copy of
//! the per-node bounding box array, and it never decompresses the whole
//! tree. The source BVH may be destroyed after the export.
//!
//! Primitives are *not* copied: like the original @ref BVH, this class
//! simply borrows the caller-managed primitive array, so leaf slots keep
//! referring to the exact same primitives.
//!
//! \tparam Float The floating point type used for decoded bounds.
//! \tparam Primitive The type of primitive the BVH was made with.
template <typename Float, typename Primitive>
class QuantizedBVH final {
  //! The quantized nodes, in the exact same order as the source tree.
  std::vector<QuantizedNode> nodes;

  //! The primitives borrowed from the source BVH (owned by the caller).
  ConstIterable<Primitive> primitives;

  //! The full-precision bounds of the root node.
  //! Every other node is decoded relative to this, level by level.
  BBox<Float> root_bounds;

 public:
  //! \brief Exports a quantized tree from a built BVH.
  //!
  //! The export only reads the source tree and the primitives; neither is
  //! modified. Node order, leaf slots and child offsets are preserved.
  //! \param src The source BVH. It may be destroyed once this returns.
  explicit QuantizedBVH(const BVH<Float, Primitive>& src) : primitives(src.getPrimitives()) {
    const auto src_nodes = src.getNodes();

    if (src_nodes.size() == 0) {
      // Empty tree: no nodes, and an empty (inverted) root box.
      const Float inf = std::numeric_limits<Float>::infinity();
      root_bounds = BBox<Float>(Vector3<Float>{inf, inf, inf}, Vector3<Float>{-inf, -inf, -inf});
      return;
    }

    // The root box is stored separately, in full precision.
    // The root node itself spans the whole grid: 0 to 65535 on every axis.
    root_bounds = src_nodes[0].bbox;

    nodes.resize(src_nodes.size());

    // Depth-first walk of the source tree. Every node is encoded against
    // the *decoded* bounds of its parent (never against the original
    // floating point parent box), so only O(depth) decoded boxes are alive
    // at any moment; no floating point node array is kept around.
    struct Entry {
      uint32_t index;
      BBox<Float> parent;
    };

    std::vector<Entry> stack;
    stack.push_back(Entry{0, root_bounds});

    while (!stack.empty()) {
      const Entry entry = stack.back();
      stack.pop_back();

      const auto& src_node = src_nodes[entry.index];

      QuantizedNode qnode;
      for (unsigned axis = 0; axis < 3; ++axis) {
        QuantizedBVHImpl::encodeAxis(src_node.bbox.min[axis], src_node.bbox.max[axis], entry.parent.min[axis],
                                     entry.parent.max[axis], qnode.min[axis], qnode.max[axis]);
      }
      qnode.start = src_node.start;
      qnode.primitive_count = src_node.primitive_count;
      qnode.right_offset = src_node.right_offset;

      nodes[entry.index] = qnode;

      if (!src_node.isLeaf()) {
        // Decode this node once, here, so the children are encoded against
        // the actually-decoded parent box.
        const BBox<Float> decoded = decodeQuantizedBounds(entry.parent, qnode);
        stack.push_back(Entry{entry.index + 1, decoded});
        stack.push_back(Entry{entry.index + src_node.right_offset, decoded});
      }
    }
  }

  //! Accesses the quantized nodes.
  //! \return A read-only iterable container of quantized nodes.
  inline auto getNodes() const noexcept { return ConstIterable<QuantizedNode>(nodes.data(), nodes.size()); }

  //! Accesses the primitives borrowed from the source BVH.
  //! \return A read-only iterable container of the primitives.
  inline auto getPrimitives() const noexcept { return primitives; }

  //! Accesses the full-precision bounds of the root node.
  //! \return The stored root bounds.
  inline const BBox<Float>& getRootBounds() const noexcept { return root_bounds; }
};

}  // namespace FastBVH
