#pragma once

//! \file EditableMesh.h
//! \brief A triangle mesh with self-intersection tracking on top of a
//! dynamically refitted FastBVH tree.
//!
//! EditableMesh owns a vertex array and an index array, builds a single
//! BVH over the triangles with FastBVH::DefaultBuilder (build strategy 1),
//! and then keeps two things up to date as vertices are moved:
//!
//!  * the set of self-intersection contacts between mesh triangles, and
//!  * the bounding boxes of the BVH, refit in place (no rebuild, no
//!    re-ordering, no copy of the tree), so the native FastBVH::Traverser
//!    keeps answering ray queries against the current geometry.
//!
//! ---------------------------------------------------------------------------
//! What counts as a self-intersection
//! ---------------------------------------------------------------------------
//!
//! Let T1 and T2 be two *closed* triangles of the mesh and let K be the
//! simplex spanned by the vertex *indices* they share (a shared vertex when
//! they share one index, a shared edge when they share two). A contact is
//! reported for the unordered pair {T1, T2} whenever the intersection
//! T1 ∩ T2 is not entirely contained in K. In other words:
//!
//!  * Ordinary edge/vertex sharing (the intersection is exactly the shared
//!    edge or the shared vertex) is *not* reported.
//!  * Two faces that share an edge but fold back coplanar over each other
//!    *are* reported (the overlap exceeds the shared edge).
//!  * Coincident points with *different* vertex indices do not count as
//!    topological adjacency: two triangles that merely touch at such a
//!    point are reported as a point contact.
//!
//! Each reported pair is classified by the geometry of T1 ∩ T2:
//!
//!  * ContactKind::Point   - the intersection is a single point,
//!  * ContactKind::Segment - the intersection is a line segment
//!                           (typical for transversal crossings),
//!  * ContactKind::Area    - the intersection has positive area
//!                           (coplanar overlap).
//!
//! Every unordered pair appears at most once in contacts(), stored as
//! (smaller face id, larger face id) and sorted by face ids.
//!
//! ---------------------------------------------------------------------------
//! Numeric contract
//! ---------------------------------------------------------------------------
//!
//!  * Vertex coordinates must be finite and satisfy |coord| <= 1e6.
//!  * Every face must be non-degenerate: all edge lengths >= 1e-5 and the
//!    shortest triangle height >= 1e-5. update_vertices() rejects batches
//!    that would violate this.
//!  * Exact coplanarity and exact contact (touching) are supported.
//!    Everything else that does not intersect is assumed to be separated
//!    by at least 1e-7; arbitrarily close-to-degenerate configurations are
//!    outside the contract.
//!  * Internally, signed distances below 1e-8 are treated as zero (exact
//!    contact), which sits one decade below the 1e-7 separation guarantee
//!    and well above the floating-point noise of the supported coordinate
//!    range. Box overlap is only used as a broad-phase filter; it is never
//!    treated as a face intersection, and tolerances are not inflated to
//!    swallow clearly separated faces.
//!  * Compile without "fast math" style flags: the exact-contact
//!    guarantees rely on IEEE semantics of the double-precision tests.
//!
//! ---------------------------------------------------------------------------
//! Work statistics
//! ---------------------------------------------------------------------------
//!
//! UpdateStats reports *actual* work performed by an accepted update:
//!
//!  * leaves_recomputed:        leaf node boxes refit (the leaves holding
//!                              dirty faces; one leaf is counted once even
//!                              if several dirty faces share it).
//!  * internal_nodes_recomputed: internal node boxes refit (the distinct
//!                              ancestors of dirty leaves, deepest first).
//!  * node_pair_checks:         broad-phase AABB overlap tests performed
//!                              while pairing dirty faces against the tree
//!                              (face box vs. tree node box).
//!  * face_box_checks:          broad-phase AABB overlap tests between two
//!                              face boxes (candidate prefilter).
//!  * triangle_pair_tests:      narrow-phase triangle/triangle intersection
//!                              tests actually executed.
//!
//! Contacts that are not incident to a moved vertex are preserved without
//! re-running their narrow-phase test.
//!
//! ---------------------------------------------------------------------------
//! Thread safety
//! ---------------------------------------------------------------------------
//!
//! EditableMesh is not thread-safe. const queries (contacts(), closest(),
//! diagnostics) may run concurrently with each other but not with
//! update_vertices().

#include <FastBVH/BBox.h>
#include <FastBVH/BVH.h>
#include <FastBVH/Ray.h>
#include <FastBVH/Vector3.h>

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace FastBVH {

//! \brief A triangle mesh that tracks self-intersection contacts while
//! its vertices are edited, keeping a single FastBVH tree refit in place.
class EditableMesh final {
 public:
  //! The vertex position type.
  using Vec3 = Vector3<double>;

  //! The index triple of one face. Face ids follow the input order.
  using FaceIndices = std::array<uint32_t, 3>;

  //! \brief Classification of a contact between two triangles.
  enum class ContactKind : uint32_t {
    Point = 0,   //!< The intersection is a single point.
    Segment = 1, //!< The intersection is a line segment.
    Area = 2,    //!< The intersection has positive area (coplanar overlap).
  };

  //! \brief One self-intersection contact record.
  //!
  //! The pair is unordered and stored exactly once: face_a < face_b.
  struct Contact final {
    uint32_t face_a;   //!< The smaller face id of the pair.
    uint32_t face_b;   //!< The larger face id of the pair.
    ContactKind kind;  //!< The geometric classification of the contact.
  };

  //! \brief The primitive the BVH is built over: a handle to one face.
  //!
  //! The builder reorders these handles; the `face` member always carries
  //! the original input face id.
  struct FaceSlot final {
    uint32_t face;  //!< The original face id.
  };

  //! \brief One requested vertex position change.
  struct VertexMove final {
    uint32_t vertex;   //!< The vertex index to move.
    Vec3 position;     //!< The new position of the vertex.
  };

  //! \brief Actual work performed by an accepted update. See the file
  //! level documentation for the exact meaning of every counter.
  struct UpdateStats final {
    uint64_t leaves_recomputed = 0;         //!< Leaf node boxes refit.
    uint64_t internal_nodes_recomputed = 0; //!< Internal node boxes refit.
    uint64_t node_pair_checks = 0;          //!< Face-box vs node-box tests.
    uint64_t face_box_checks = 0;           //!< Face-box vs face-box tests.
    uint64_t triangle_pair_tests = 0;       //!< Narrow-phase triangle tests.
  };

  //! \brief The result of one update_vertices() call.
  //!
  //! When the batch is rejected, `accepted` is false, `error` describes
  //! the reason, `error_vertex` identifies the offending vertex, and the
  //! mesh (vertices, tree, contacts, ray results) is left untouched.
  //! When accepted, `added` and `removed` hold the set difference of the
  //! contact set, including the contact kind: a pair whose kind changed
  //! appears as a removal of the old record plus an addition of the new
  //! one. Both lists are sorted by (face_a, face_b).
  struct UpdateResult final {
    bool accepted = false;          //!< True when the batch was applied.
    const char* error = nullptr;    //!< Rejection reason, nullptr if accepted.
    uint32_t error_vertex = ~0u;    //!< Offending vertex, ~0u if accepted.
    std::vector<Contact> added;     //!< Contacts created by the update.
    std::vector<Contact> removed;   //!< Contacts eliminated by the update.
    UpdateStats stats;              //!< Actual work counters of the update.
  };

  //! \brief The result of a closest() ray query.
  struct RayHit final {
    uint32_t face;  //!< The original face id that was hit.
    double t;       //!< Ray parameter: hit position = ray.o + ray.d * t.
    double u;       //!< Barycentric coordinate of vertex 1 of the face.
    double v;       //!< Barycentric coordinate of vertex 2 of the face.
    //! The barycentric coordinate of vertex 0 is 1 - u - v.
  };

  //! \brief Human-readable diagnostic information about one tree node.
  struct NodeDiag final {
    uint32_t index;     //!< Index of the node in the flattened tree.
    int32_t parent;     //!< Parent node index, or -1 for the root.
    uint32_t depth;     //!< Depth in the tree (root is 0).
    bool leaf;          //!< True when this node is a leaf.
    uint32_t start;     //!< First slot index in the primitive array.
    uint32_t count;     //!< Number of primitives covered by this node.
    BBox<double> bbox;  //!< Current bounding box of the node.
  };

  //! \brief Builds the mesh and its BVH and computes the initial contacts.
  //!
  //! The initial build uses FastBVH::DefaultBuilder (strategy 1); the
  //! builder reorders the internal face slots, while face ids always
  //! follow the input order. The initial contact set is computed with a
  //! dual-tree self-pairing traversal.
  //!
  //! \param vertices The vertex positions (see the numeric contract).
  //! \param faces The triangle index triples. Must be non-empty and every
  //! index must reference a valid vertex.
  //! \throws std::invalid_argument when the input violates the contract
  //! (empty face list, out-of-range index, non-finite or out-of-bound
  //! coordinate).
  EditableMesh(std::vector<Vec3> vertices, std::vector<FaceIndices> faces);

  //! EditableMesh cannot be copied (the tree references the slot array).
  EditableMesh(const EditableMesh&) = delete;
  //! EditableMesh cannot be copy-assigned.
  EditableMesh& operator=(const EditableMesh&) = delete;
  //! EditableMesh can be moved; the tree stays attached to the slot array.
  EditableMesh(EditableMesh&&) = default;
  //! EditableMesh can be move-assigned.
  EditableMesh& operator=(EditableMesh&&) = default;

  //! \brief The current self-intersection contacts.
  //! \return The contact records, sorted by (face_a, face_b). Each
  //! unordered pair appears at most once.
  const std::vector<Contact>& contacts() const noexcept { return contacts_cache_; }

  //! \brief Applies a batch of vertex moves atomically.
  //!
  //! All positions take effect together. The batch is rejected (and the
  //! mesh left completely unchanged) when it contains a vertex index that
  //! does not exist, the same vertex twice, a non-finite position, a
  //! position with |coord| > 1e6, or when the resulting positions would
  //! make any incident face degenerate (edge length or height below 1e-5).
  //!
  //! On success, only the leaves and ancestors of dirty faces are refit,
  //! and only candidate pairs collected through real node pruning are
  //! narrow-phase tested. Contacts not incident to a moved vertex are
  //! preserved without re-testing.
  //!
  //! \param moves The vertex moves to apply (may be empty).
  //! \return The update result; see UpdateResult.
  UpdateResult update_vertices(const std::vector<VertexMove>& moves);

  //! \brief Finds the closest intersection of a ray with the mesh.
  //!
  //! The query runs through the native FastBVH::Traverser on the refit
  //! tree; no separate face array is scanned. The ray direction does not
  //! need to be normalized: `t` is the parameter of the given ray, so the
  //! hit position is always ray.o + ray.d * t.
  //!
  //! \param ray The ray to trace.
  //! \param out Receives the face id, ray parameter and barycentric
  //! coordinates of the closest hit.
  //! \return True when the ray hits the mesh.
  bool closest(const Ray<double>& ray, RayHit& out) const;

  //! \brief The current vertex positions.
  const std::vector<Vec3>& vertices() const noexcept { return verts_; }

  //! \brief The face index triples, in input order.
  const std::vector<FaceIndices>& faces() const noexcept { return faces_; }

  //! \brief The underlying BVH (e.g. for custom traversals). The node
  //! bounding boxes reflect the current vertex positions.
  const BVH<double, FaceSlot>& bvh() const noexcept { return bvh_; }

  //! \brief Human-readable diagnostics for every tree node, in the order
  //! of the flattened node array.
  std::vector<NodeDiag> node_diagnostics() const;

  //! \brief The face ids stored in the slots of one leaf node, in slot
  //! order. Returns an empty list for internal nodes or invalid indices.
  std::vector<uint32_t> leaf_slots(uint32_t node_index) const;

  //! \brief The index of the leaf node that currently holds a face.
  //! \return The leaf node index, or ~0u when the face id is invalid.
  uint32_t face_leaf(uint32_t face) const;

 private:
  //! A key of the contact map: an ordered face pair (first < second).
  using ContactKey = std::pair<uint32_t, uint32_t>;

  //! Builds the BVH over the face slots with the default strategy.
  static BVH<double, FaceSlot> buildTree(std::vector<FaceSlot>& slots,
                                         const std::vector<BBox<double>>& boxes);

  //! The exact bounding box of one face at the current positions.
  BBox<double> faceBox(uint32_t face) const;

  //! Computes the initial contact set with dual-tree self-pairing.
  void detectAllContacts();

  //! Collects contact candidates for one dirty face against the tree and
  //! narrow-phase tests them, filling `found` with the contacts detected.
  void queryPairs(uint32_t face, const std::vector<uint32_t>& dirty_stamp, uint32_t dirty_gen,
                  std::map<ContactKey, ContactKind>& found, UpdateStats& stats) const;

  //! Rebuilds the sorted contacts cache from the contact map.
  void rebuildCache();

  //! The current vertex positions.
  std::vector<Vec3> verts_;

  //! The face index triples, in input order.
  std::vector<FaceIndices> faces_;

  //! The face handles the BVH was built over (builder-reordered). The
  //! address of this array must stay stable: the BVH points into it.
  std::vector<FaceSlot> slots_;

  //! The exact bounding box of every face, in face id order.
  std::vector<BBox<double>> face_boxes_;

  //! The BVH over the face slots. Only node boxes are mutated in place.
  BVH<double, FaceSlot> bvh_;

  //! Point-to-face index: incident faces of every vertex.
  std::vector<std::vector<uint32_t>> vertex_faces_;

  //! Face-to-leaf index: the leaf node holding every face.
  std::vector<uint32_t> face_leaf_;

  //! Parent chain of the flattened tree (~0u for the root).
  std::vector<uint32_t> node_parent_;

  //! Depth of every node (root is 0), used for bottom-up refits.
  std::vector<uint32_t> node_depth_;

  //! The current contact set: ordered face pair -> contact kind.
  std::map<ContactKey, ContactKind> contact_map_;

  //! Reverse index of the contact pairs: face -> incident contact keys.
  std::vector<std::set<ContactKey>> face_contacts_;

  //! The sorted contact cache returned by contacts().
  std::vector<Contact> contacts_cache_;

  //! Generation stamps used to mark dirty faces without clearing arrays.
  std::vector<uint32_t> dirty_stamp_;

  //! The current dirty-face generation.
  uint32_t dirty_gen_ = 0;
};

//! Compares two contacts by (face_a, face_b, kind).
inline bool operator<(const EditableMesh::Contact& a, const EditableMesh::Contact& b) noexcept {
  if (a.face_a != b.face_a) return a.face_a < b.face_a;
  if (a.face_b != b.face_b) return a.face_b < b.face_b;
  return a.kind < b.kind;
}

//! Compares two contacts for equality.
inline bool operator==(const EditableMesh::Contact& a, const EditableMesh::Contact& b) noexcept {
  return a.face_a == b.face_a && a.face_b == b.face_b && a.kind == b.kind;
}

//! \brief A human-readable name for a contact kind ("point", "segment",
//! "area"), useful for logging and diagnostics.
const char* contact_kind_name(EditableMesh::ContactKind kind) noexcept;

}  // namespace FastBVH
