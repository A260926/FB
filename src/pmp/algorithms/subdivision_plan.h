// Copyright 2011-2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "pmp/surface_mesh.h"
#include "pmp/types.h"

namespace pmp {

//! \addtogroup algorithms
//!@{

//! \brief One texture channel for a SubdivisionPlan.
//! \details A channel has its own value table and one value index per
//! source face corner. The corner array is indexed by Halfedge::idx() and
//! therefore has to have size mesh.halfedges_size(). The corner stored in
//! corner_values[h] belongs to the vertex Halfedge h points to, i.e., to
//! to_vertex(h). This is the same convention as the "h:tex" halfedge
//! property used by the OBJ reader/writer.
//!
//! Corners in different faces may reference different indices even if those
//! indices store the same texture coordinate. Such index seams are preserved
//! by the plan.
struct SubdivisionPlanChannel
{
    //! Channel name, only used for diagnostics and reporting.
    std::string name;

    //! Independent texture coordinate value table.
    std::vector<TexCoord> values;

    //! One value index per source halfedge/corner, indexed by Halfedge::idx().
    std::vector<IndexType> corner_values;
};

//! \brief A compressed sparse row (CSR) linear stencil.
//! \details Rows are output entities, columns are source control entities.
//! Same-column contributions have already been merged. Each row sums to one.
struct SparseStencil
{
    //! Row offsets, size rows() + 1.
    std::vector<IndexType> row_offsets;

    //! Column (source control entity) indices.
    std::vector<IndexType> column_indices;

    //! Numerical weights, parallel to column_indices.
    std::vector<Scalar> weights;

    //! Number of stencil rows.
    size_t rows() const
    {
        return row_offsets.empty() ? 0 : row_offsets.size() - 1;
    }

    //! Number of stored non-zeros.
    size_t non_zeros() const { return column_indices.size(); }
};

//! \brief Owning Catmull-Clark subdivision plan.
//! \details The plan pre-computes the combinatorial subdivision hierarchy
//! and sparse linear stencils for positions and for every texture channel.
//! After construction the source SurfaceMesh is no longer needed: evaluate()
//! applies the stored stencils to new control positions and new channel value
//! tables.
//!
//! Geometry follows the regular Catmull-Clark rules of PMP, including the
//! permanent feature rules ("v:feature" / "e:feature") and
//! BoundaryHandling::Interpolate on geometric boundaries. Every texture
//! channel is subdivided independently on its own cut face-corner topology:
//! an interior geometric edge connects the two sides in a channel only if the
//! corner indices at *both* endpoints agree on the two sides. Otherwise the
//! edge is a seam and splits the corner fans; UV values and vertex positions
//! are never used to weld corners. The geometry itself is never split.
//!
//! The plan does not implement limit-surface evaluation, semi-sharp creases,
//! or local incremental updates.
class SubdivisionPlan
{
public:
    //! Invalid compact index.
    static constexpr IndexType invalid_index = PMP_MAX_INDEX;

    //! Maximum supported number of subdivision levels.
    static constexpr unsigned max_levels = 5;

    //! Construct an empty plan.
    SubdivisionPlan() = default;

    //! \brief Build a plan from \p mesh.
    //! \param mesh Source control cage. Read only; not referenced afterwards.
    //! \param levels Number of Catmull-Clark steps, in [1, max_levels].
    //! \param channels Texture channels, each with its own value table and
    //! per-corner value indices.
    //! \throw InvalidInputException or TopologyException on invalid input.
    SubdivisionPlan(const SurfaceMesh& mesh, unsigned levels,
                    const std::vector<SubdivisionPlanChannel>& channels);

    //! \brief Rebuild the plan, keeping the previous plan on failure.
    //! \details Same requirements as the constructor. If validation or
    //! construction fails, the previous plan and its evaluated results stay
    //! intact.
    void rebuild(const SurfaceMesh& mesh, unsigned levels,
                 const std::vector<SubdivisionPlanChannel>& channels);

    //! Whether a plan has been built successfully.
    bool is_built() const { return built_; }

    //! Number of subdivision levels.
    unsigned levels() const { return levels_; }

    //! Number of texture channels.
    size_t channel_count() const { return channel_names_.size(); }

    //! Name of channel \p channel.
    const std::string& channel_name(size_t channel) const;

    //! \name Compact source mappings
    //!@{

    size_t source_vertex_count() const { return source_vertex_count_; }
    size_t source_face_count() const { return source_face_count_; }
    size_t source_corner_count() const { return source_corner_count_; }

    //! Compact id of source vertex \p v, or invalid_index.
    IndexType source_vertex_id(Vertex v) const;
    //! Compact id of source face \p f, or invalid_index.
    IndexType source_face_id(Face f) const;
    //! Compact id of the corner at halfedge \p h, or invalid_index.
    IndexType source_corner_id(Halfedge h) const;

    //! Original vertex handle of compact vertex \p id.
    Vertex source_vertex(IndexType id) const;
    //! Original face handle of compact face \p id.
    Face source_face(IndexType id) const;
    //! Original halfedge handle of compact corner \p id.
    Halfedge source_corner_halfedge(IndexType id) const;

    //! Contact face of compact corner \p id.
    IndexType source_corner_face(IndexType id) const;
    //! Compact geometry vertex of compact corner \p id.
    IndexType source_corner_vertex(IndexType id) const;
    //! Local corner index (within its face) of compact corner \p id.
    IndexType source_corner_in_face(IndexType id) const;

    //! Compact source faces, each as a list of compact source vertices.
    const std::vector<std::vector<IndexType>>& source_faces() const
    {
        return source_faces_;
    }

    //!@}

    //! \name Subdivision result topology
    //!@{

    //! Number of vertices of the subdivided geometry.
    size_t vertex_count() const;
    //! Number of faces of the subdivided geometry.
    size_t face_count() const { return faces_.size(); }

    //! Shared subdivided geometry connectivity (compact result vertex ids).
    const std::vector<std::vector<IndexType>>& faces() const { return faces_; }

    //! Compact source face id for each subdivided face.
    const std::vector<IndexType>& face_sources() const { return face_sources_; }

    //! Position stencil: rows are result vertices, columns source vertices.
    const SparseStencil& position_stencil() const { return position_stencil_; }

    //!@}

    //! \name Channel results
    //!@{

    //! Number of values (channel vertices) in the subdivided channel topology.
    size_t channel_value_count(size_t channel) const;
    //! Number of channel vertices at subdivision level zero (control fan
    //! vertices after seam splitting).
    size_t channel_control_vertex_count(size_t channel) const;
    //! Simplified name: alias for channel_control_vertex_count().
    size_t channel_source_vertex_count(size_t channel) const
    {
        return channel_control_vertex_count(channel);
    }

    //! Build-time value index for every compact source corner.
    const std::vector<IndexType>&
    channel_source_value_indices(size_t channel) const;
    //! Build-time channel control vertex for every compact source corner.
    const std::vector<IndexType>&
    channel_source_corner_vertices(size_t channel) const;

    //! Channel stencil: rows are result channel values, columns are value
    //! indices into the channel value table (merged by column, rows sum to 1).
    const SparseStencil& channel_stencil(size_t channel) const;

    //! Per-face, per-corner result channel value indices. The face and corner
    //! layout is identical to faces().
    const std::vector<std::vector<IndexType>>&
    channel_corner_indices(size_t channel) const;

    //! Evaluated result channel value table.
    const std::vector<TexCoord>& channel_values(size_t channel) const;

    //!@}

    //! \name Evaluation
    //!@{

    //! \brief Apply all stored stencils.
    //! \param source_positions Control positions, indexed by compact source
    //! vertex id (source_vertex_count() entries).
    //! \param channel_value_tables New value tables, one per channel. Each
    //! table must have the same length as the table supplied at build time.
    //! \throw InvalidInputException if an argument has the wrong size or a
    //! non-finite value. On failure all previously evaluated results are kept.
    void evaluate(const std::vector<Point>& source_positions,
                  const std::vector<std::vector<TexCoord>>&
                      channel_value_tables);

    //! Evaluated result positions, indexed by result vertex id.
    const std::vector<Point>& positions() const { return positions_; }

    //!@}

private:
    void build_impl(const SurfaceMesh& mesh, unsigned levels,
                    const std::vector<SubdivisionPlanChannel>& channels);
    void require_built() const;

    bool built_ = false;
    unsigned levels_ = 0;

    std::vector<std::string> channel_names_;

    size_t source_vertex_count_ = 0;
    size_t source_face_count_ = 0;
    size_t source_corner_count_ = 0;

    std::vector<IndexType> source_vertex_index_; // compact -> original index
    std::vector<IndexType> source_face_index_;   // compact -> original index
    std::vector<IndexType> corner_halfedge_;     // compact -> original index
    std::vector<IndexType> corner_face_;         // compact -> compact face
    std::vector<IndexType> corner_vertex_;       // compact -> compact vertex
    std::vector<IndexType> corner_in_face_;      // compact -> local corner
    std::vector<size_t> corner_face_offsets_;    // per face corner range

    std::vector<IndexType> vertex_lookup_;    // original idx -> compact
    std::vector<IndexType> face_lookup_;      // original idx -> compact
    std::vector<IndexType> halfedge_lookup_;  // original idx -> compact corner

    std::vector<std::vector<IndexType>> source_faces_;

    // Build-time channel data.
    std::vector<std::vector<IndexType>> channel_source_value_indices_;
    std::vector<std::vector<IndexType>> channel_source_corner_vertices_;
    std::vector<size_t> channel_control_vertex_count_;
    std::vector<size_t> channel_source_value_count_;

    // Result topology and stencils.
    std::vector<std::vector<IndexType>> faces_;
    std::vector<IndexType> face_sources_;
    size_t result_vertex_count_ = 0;
    SparseStencil position_stencil_;
    std::vector<SparseStencil> channel_stencils_;
    std::vector<std::vector<std::vector<IndexType>>> channel_corner_indices_;

    // Last successful evaluation.
    std::vector<Point> positions_;
    std::vector<std::vector<TexCoord>> channel_values_;
};

//!@}

} // namespace pmp
