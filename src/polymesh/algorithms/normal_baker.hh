#pragma once

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include <polymesh/Mesh.hh>
#include <polymesh/detail/segment_index.hh>

// CPU tangent-space normal baker for a high-poly -> low-poly projection.
//
// The session takes a snapshot of all inputs in its constructor. The meshes
// and attributes passed to the constructor may be destroyed afterwards;
// vertex identities in update_positions() refer to the vertex slot numbers
// that were valid at construction time.
//
// All faces must be manifold triangles. Position attributes store homogeneous
// positions (x, y, z, w) with w == 1. Low-poly UVs live in [0, 1] with v
// pointing up.

namespace polymesh
{
/// Per-texel result. Indexed by y * width + x.
///
/// - uncovered texels: coverage == false, low_face == -1, high_face == -1,
///   t == 0, normal == (0, 0, 0)
/// - covered but no hit: coverage == true, low_face >= 0, high_face == -1,
///   t == 0, normal == (0, 0, 0)
/// - covered and hit: coverage == true, low_face >= 0, high_face >= 0,
///   t is the ray parameter in [0, 2r], normal is the unencoded unit
///   tangent-space normal
struct normal_bake_texel
{
    bool coverage = false;
    bool hit = false;
    int low_face = -1;
    int high_face = -1;
    double t = 0.0;
    std::array<double, 3> normal = {0.0, 0.0, 0.0};

    // Convenience aliases; always kept in sync with the fields above.
    bool covered = false;
    int low_face_index = -1;
    int high_face_index = -1;
    int low_poly_face = -1;
    int high_poly_face = -1;
    double ray_parameter = 0.0;
    std::array<double, 3> tangent_normal = {0.0, 0.0, 0.0};

    bool is_covered() const { return coverage; }
    bool has_coverage() const { return coverage; }
    bool is_hit() const { return hit; }
};

/// Read-only view of a high-poly BVH node.
///
/// The tree connectivity and the face arrangement are fixed at construction
/// time. An update only refreshes the bounds of affected leaves and their
/// ancestors.
struct normal_bake_bvh_node
{
    int parent = -1;
    int left = -1;  ///< -1 for leaves
    int right = -1; ///< -1 for leaves
    /// leaf: range [first, first + count) into bvh_face_order()
    int first = 0;
    int count = 0;
    std::array<double, 3> min = {0.0, 0.0, 0.0};
    std::array<double, 3> max = {0.0, 0.0, 0.0};

    bool is_leaf() const { return count > 0; }
};

/// Result of the most recent update_positions() call.
/// All vectors are sorted and contain unique entries.
struct normal_bake_update
{
    /// constructor-time high-poly face indices that were recomputed
    std::vector<int> faces;
    /// linear texel indices that were re-projected
    std::vector<int> texels;
    /// high-poly BVH node indices whose bounds were updated
    std::vector<int> nodes;

    std::vector<int> const& recomputed_faces() const { return faces; }
    std::vector<int> const& reprojected_texels() const { return texels; }
    std::vector<int> const& updated_nodes() const { return nodes; }

    std::vector<int> const& recomputed() const { return faces; }
    std::vector<int> const& reprojected() const { return texels; }
    std::vector<int> const& bvh_nodes() const { return nodes; }

    bool empty() const { return faces.empty() && texels.empty() && nodes.empty(); }
};

class normal_bake_session
{
public:
    using texel_result = normal_bake_texel;
    using update_info = normal_bake_update;
    using bvh_node = normal_bake_bvh_node;

public:
    /// Constructs the bake session.
    ///
    /// \param high_positions  high-poly positions (x, y, z, w), w must be 1
    /// \param low_positions   low-poly positions (x, y, z, w), w must be 1
    /// \param low_uvs         low-poly per-corner UVs, in [0, 1], v points up
    /// \param width           output width in texels, must be positive
    /// \param height          output height in texels, must be positive
    /// \param r               positive projection half-length
    ///
    /// Throws std::invalid_argument on non-finite input, non-triangular
    /// faces, world-degenerate faces, invalid position slots, or overlapping
    /// UV coverage.
    normal_bake_session(vertex_attribute<std::array<double, 4>> const& high_positions,
                        vertex_attribute<std::array<double, 4>> const& low_positions,
                        halfedge_attribute<std::array<double, 3>> const& low_uvs,
                        int width,
                        int height,
                        double r);

    /// Updates the positions of high-poly vertices.
    ///
    /// Slots are constructor-time high-poly vertex indices. If a slot occurs
    /// more than once, the last value wins. An empty (or effectively empty)
    /// update performs no work.
    ///
    /// Throws std::invalid_argument if a slot is invalid, a value is
    /// non-finite, or an adjacent high-poly face becomes degenerate. In that
    /// case the session state (snapshot, results, and update record) is
    /// unchanged.
    void update_positions(std::vector<std::pair<int, std::array<double, 3>>> const& updates);

    /// The current per-texel result, indexed by y * width + x. Read-only.
    std::vector<normal_bake_texel> const& result() const { return mResult; }

    /// The record of the last update_positions() call. Read-only.
    normal_bake_update const& last_update() const { return mLastUpdate; }

    int width() const { return mWidth; }
    int height() const { return mHeight; }
    double projection_half_length() const { return mR; }

    /// Read-only high-poly BVH connectivity.
    std::vector<normal_bake_bvh_node> const& bvh_nodes() const { return mBvhNodes; }
    /// Read-only leaf face arrangement of the high-poly BVH.
    std::vector<int> const& bvh_face_order() const { return mBvhFaceOrder; }

    // aliases
    std::vector<normal_bake_bvh_node> const& high_bvh_nodes() const { return mBvhNodes; }
    std::vector<int> const& face_order() const { return mBvhFaceOrder; }
    std::vector<int> const& high_face_order() const { return mBvhFaceOrder; }
    std::vector<normal_bake_bvh_node> const& bvh() const { return mBvhNodes; }

    /// Read-only finite-segment index over the covered texel projection rays.
    detail::segment_index const& projection_segment_index() const { return mSegments; }
    detail::segment_index const& segment_index() const { return mSegments; }

    // internal data
private:
    struct low_face
    {
        /// world positions, UV-CCW order
        std::array<double, 3> p[3] = {};
        /// UV coordinates, UV-CCW order
        std::array<double, 3> uv[3] = {};
        /// world positions, original winding order
        std::array<double, 3> p_orig[3] = {};
        /// unit geometric normal (original winding)
        std::array<double, 3> n = {};
        /// unit tangent
        std::array<double, 3> t = {};
        /// unit bitangent (handedness from dP/dv)
        std::array<double, 3> b = {};
    };
    struct texel_ray
    {
        int texel = -1;
        int low_face = -1;
        std::array<double, 3> origin = {}; ///< P + r * N
        std::array<double, 3> dir = {};    ///< -N
        double max_t = 0.0;
        std::array<double, 3> t = {}; ///< tangent frame of the low face
        std::array<double, 3> b = {};
        std::array<double, 3> n = {};
    };

private:
    void build_high_bvh();
    int build_high_bvh_recursive(int begin, int end, int parent);
    void recompute_node_bounds(int node);
    bool cast_ray(texel_ray const& ray, double& out_t, int& out_face) const;
    void project_texel(texel_ray const& ray, double t, int high_face);

    static void set_result(normal_bake_texel& r, bool coverage, int low_face, int high_face, double t, std::array<double, 3> const& n);
    static void clear_result(normal_bake_texel& r);
private:
    int mWidth = 0;
    int mHeight = 0;
    double mR = 0.0;

    // high-poly snapshot
    std::vector<std::array<double, 3>> mHighPositions; ///< indexed by constructor-time vertex slot
    std::vector<std::array<int, 3>> mHighFaces;        ///< indexed by constructor-time face slot
    std::vector<char> mHighFaceValid;
    std::vector<int> mVertexFaceOffsets; ///< CSR offsets into mVertexFaceList
    std::vector<int> mVertexFaceList;

    // low-poly snapshot
    std::vector<char> mLowFaceValid;
    std::vector<low_face> mLowFaces;

    // per-texel coverage and rays
    std::vector<int> mTexelFace; ///< texel -> low face, -1 if uncovered
    std::vector<int> mTexelToRay; ///< texel -> ray index, -1 if uncovered
    std::vector<texel_ray> mRays;

    // high-poly BVH
    std::vector<normal_bake_bvh_node> mBvhNodes;
    std::vector<int> mBvhFaceOrder; ///< leaf order
    std::vector<int> mFaceLeaf;     ///< face slot -> leaf node index, -1 if invalid
    std::vector<std::array<double, 3>> mFaceBoundsMin;
    std::vector<std::array<double, 3>> mFaceBoundsMax;

    // projection segment index (segment id == linear texel index)
    detail::segment_index mSegments;

    // results
    std::vector<normal_bake_texel> mResult;
    normal_bake_update mLastUpdate;
};
} // namespace polymesh
