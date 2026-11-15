#pragma once

#include <FastBVH/BBox.h>
#include <FastBVH/Intersection.h>
#include <FastBVH/Iterable.h>
#include <FastBVH/Ray.h>

#include <cstdint>
#include <vector>

//! \brief This namespace contains all the declarations
//! in the library. All declarations in this namespace,
//! with some exceptions, are available for end-users.
namespace FastBVH {

//! \brief Node descriptor for the flattened tree
//! \tparam Float The floating point type
//! used for the bounding box vectors.
template <typename Float>
struct Node final {
  //! The bounding box of the node.
  BBox<Float> bbox;

  //! The index of the first primitive.
  uint32_t start;

  //! The number of primitives in this node.
  uint32_t primitive_count;

  //! Number of elements to skip in flattened tree to get to a left child's sibling.
  //! (Node+1 == Node's left child , Node + rightOffset == Node's right child)
  uint32_t right_offset;

  //! Indicates if this node is a leaf node.
  //! \return True if this node is a leaf node, false otherwise.
  inline constexpr bool isLeaf() const noexcept { return right_offset == 0; }
};

//! \brief This is a type definition for a node array.
//! Later one, this might be changed to an actual class.
//! \tparam Float The floating point type of the node vectors.
template <typename Float>
using NodeArray = std::vector<Node<Float>>;

//! \author Brandon Pelfrey
//! \brief A Bounding Volume Hierarchy system for fast Ray-Object intersection tests
template <typename Float, typename Primitive>
class BVH final {
  //! An array of nodes used for fast iteration
  //! of the BVH, using iteration.
  NodeArray<Float> nodes;

  //! The primitives from which this BVH was built.
  ConstIterable<Primitive> primitives;

 public:
  //! Constructs a new BVH instance.
  //! This constructor is ideally called internally
  //! from a @ref BuildStrategy.
  //! \param n The nodes to assign to the BVH.
  BVH(NodeArray<Float>&& n, const ConstIterable<Primitive>& p) : nodes(std::move(n)), primitives(p) {}

  //! Counts the number of leafs in the BVH.
  //! This can be useful for performance measurement.
  //! \return The number of leafs in the BVH.
  auto countLeafs() const noexcept {
    std::size_t leaf_count = 0;
    for (const auto& node : nodes) {
      if (node.isLeaf()) {
        leaf_count++;
      }
    }
    return leaf_count;
  }

  //! Accesses the BVH nodes.
  //! \return A read-only iterable container of nodes.
  inline auto getNodes() const noexcept { return ConstIterable<Node<Float>>(nodes.data(), nodes.size()); }

  //! Accesses an iterable container to the primitives in the BVH.
  //! \return An iterable container of the primitive array.
  inline auto getPrimitives() const noexcept { return primitives; }

  //! Refreshes every bounding box in place without changing the tree
  //! topology, the node connections, or the primitive ordering.
  //! Leaf boxes are recomputed from the primitives through \p converter;
  //! internal boxes are merged bottom-up from their children.
  //! An empty tree is a no-op. The flattened tree stores every subtree
  //! contiguously in preorder, so iterating the node array in reverse
  //! visits all descendants before their parent.
  //! \tparam BoxConverter The type of the primitive-to-box converter.
  //! \param converter The converter used to recompute leaf boxes.
  template <typename BoxConverter>
  void refit(const BoxConverter& converter) {
    if (nodes.empty()) {
      return;
    }

    for (std::size_t k = nodes.size(); k-- > 0;) {
      Node<Float>& node = nodes[k];

      if (node.isLeaf()) {
        if (node.primitive_count == 0) {
          continue;
        }

        auto bb = converter(primitives[node.start]);
        for (uint32_t p = 1; p < node.primitive_count; ++p) {
          bb.expandToInclude(converter(primitives[node.start + p]));
        }
        node.bbox = bb;
      } else {
        node.bbox = nodes[k + 1].bbox;
        node.bbox.expandToInclude(nodes[k + node.right_offset].bbox);
      }
    }
  }

 protected:
  //! Build the BVH tree out of build_prims
  //! \param converter The primitive to bounding box converter.
  template <typename BoxConverter>
  void build(const BoxConverter& converter);
};

}  // namespace FastBVH
