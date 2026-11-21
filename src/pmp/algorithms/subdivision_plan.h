// Copyright 2011-2022 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include "pmp/surface_mesh.h"

namespace pmp {

//! \brief An owning, reusable Catmull-Clark subdivision plan.
//! \details A SubdivisionPlan captures the topology of a polygonal control
//! cage together with any number of texture coordinate channels and
//! precomputes sparse linear stencils that map source control values
//! (vertex positions and per-channel texture coordinates) to the subdivided
//! result. Once built, the plan owns everything it needs: the source mesh may
//! be modified or destroyed and the plan can still be evaluated repeatedly
//! with new control values. If the source topology or the channel corner
//! indices change, build a new plan.
//!
//! Position rules follow catmull_clark_subdivision() with
//! BoundaryHandling::Interpolate, including the permanent feature rules for
//! vertices/edges flagged through the \c v:feature and \c e:feature
//! properties. Every texture channel builds its own face-corner topology: the
//! two sides of a geometric interior edge are connected only if both edge
//! endpoints reference the same channel value index on both sides. Corners
//! are never welded by value or by spatial position, and face fans around a
//! vertex that are separated by seams stay separate even if they reuse the
//! same value index. Channels are subdivided with the smooth Catmull-Clark
//! rules on their cut topology (boundary vertices use (6P+prev+next)/8, edge
//! points on the boundary take the midpoint); geometric features do not alter
//! channel rules. All channels share the refined geometric positions and the
//! subfaces of every channel correspond one-to-one to the geometric ones.
//!
//! The refinement ordering is deterministic. Level zero vertices are the
//! source vertices in SurfaceMesh::vertices() order; level zero faces are the
//! source faces in SurfaceMesh::faces() order; level zero corners enumerate
//! the halfedges of each face starting at SurfaceMesh::halfedge(Face), each
//! corner pointing to the to-vertex of its halfedge. Each refinement step
//! emits first the smoothed old vertices, then one vertex per old edge, then
//! one vertex per old face; every old face spawns one quad per corner in
//! corner order, (vertex point, next edge point, face point, previous edge
//! point). Channel level zero vertices are the welded corner components in
//! corner scan order and refine with the same scheme.
//!
//! \ingroup algorithms
class SubdivisionPlan
{
public:
    //! \brief One texture coordinate channel.
    //! \details An independent table of texture coordinates plus one value
    //! index per source face corner. Corner indices are ordered by the
    //! halfedges of each source face (see class docs); corner_indices.size()
    //! must equal the total number of source face corners.
    struct Channel
    {
        std::vector<TexCoord> values;          //!< texcoord value table
        std::vector<IndexType> corner_indices; //!< value index per corner
    };

    //! \brief Sparse linear stencil in CSR form.
    //! \details Row i of the matrix is given by weights[row_offsets[i],
    //! row_offsets[i+1]) and maps source control values columns[j] to output
    //! value i. Columns within each row are sorted and merged; every row sums
    //! to one.
    struct Stencil
    {
        std::vector<size_t> row_offsets;   //!< row starts, size rows()+1
        std::vector<IndexType> columns;    //!< source value index per nonzero
        std::vector<Scalar> weights;       //!< weight per nonzero

        //! number of rows (output values) of the stencil
        size_t rows() const
        {
            return row_offsets.empty() ? 0 : row_offsets.size() - 1;
        }
    };

    //! default constructor: empty plan
    SubdivisionPlan() = default;

    //! \brief Build the plan from a control cage and texture channels.
    //! \param mesh source control cage; oriented 2-manifold polygon mesh,
    //! possibly with boundary, several components, extraordinary vertices,
    //! and pending (not garbage-collected) deletions. Only read from.
    //! \param levels number of Catmull-Clark refinement steps, 1 to 5.
    //! \param channels texture coordinate channels, may be empty.
    //! \throw InvalidInputException on invalid levels, isolated vertices,
    //! non-finite positions or channel values, malformed corner indices, or
    //! non-manifold source/channel topology. On failure the previously built
    //! plan is kept unchanged.
    void build(const SurfaceMesh& mesh, size_t levels,
               const std::vector<Channel>& channels = {});

    //! \brief Evaluate the plan with new control values.
    //! \param positions new source positions, one per source vertex in
    //! compact source order (see source_vertex()).
    //! \param channel_values new value tables, one per channel, each with
    //! channel_value_count() entries.
    //! \throw std::logic_error if the plan has not been built yet.
    //! \throw InvalidInputException on size mismatches or non-finite values.
    //! On failure the previous evaluation results are kept unchanged.
    void evaluate(const std::vector<Point>& positions,
                  const std::vector<std::vector<TexCoord>>& channel_values);

    //! \name Source mapping
    //!@{

    //! whether build() completed successfully
    bool is_built() const { return built_; }

    //! number of refinement steps of the built plan
    size_t levels() const { return levels_; }

    //! number of source vertices in compact order
    size_t n_source_vertices() const { return source_vertices_.size(); }

    //! number of source faces in compact order
    size_t n_source_faces() const { return source_faces_.size(); }

    //! total number of source face corners
    size_t n_source_corners() const { return source_corners_.size(); }

    //! source vertex of compact source index \p i
    Vertex source_vertex(size_t i) const { return source_vertices_[i]; }

    //! source face of compact source face index \p i
    Face source_face(size_t i) const { return source_faces_[i]; }

    //! source halfedge of compact source corner index \p i
    Halfedge source_corner(size_t i) const { return source_corners_[i]; }

    //!@}
    //! \name Refined geometry
    //!@{

    //! number of refined vertices
    size_t n_vertices() const { return n_vertices_; }

    //! number of refined faces; every face is a quad
    size_t n_faces() const { return face_sources_.size(); }

    //! refined face connectivity: four vertex indices per face
    const std::vector<IndexType>& faces() const { return face_corners_; }

    //! compact source face each refined face descends from
    const std::vector<IndexType>& face_sources() const { return face_sources_; }

    //! stencil mapping source positions to refined positions
    const Stencil& position_stencil() const { return position_stencil_; }

    //! refined positions from the last successful evaluate()
    const std::vector<Point>& positions() const { return positions_; }

    //!@}
    //! \name Texture channels
    //!@{

    //! number of texture coordinate channels
    size_t n_channels() const { return channels_.size(); }

    //! number of source values of channel \p c
    size_t channel_value_count(size_t c) const { return channels_[c].n_source_values; }

    //! number of refined values of channel \p c
    size_t n_channel_vertices(size_t c) const { return channels_[c].n_vertices; }

    //! refined per-corner value indices of channel \p c: four per face,
    //! aligned with faces()
    const std::vector<IndexType>& channel_corners(size_t c) const
    {
        return channels_[c].corners;
    }

    //! stencil mapping source values to refined values of channel \p c
    const Stencil& channel_stencil(size_t c) const { return channels_[c].stencil; }

    //! refined values of channel \p c from the last successful evaluate()
    const std::vector<TexCoord>& channel_values(size_t c) const
    {
        return channels_[c].values;
    }

    //!@}

private:
    struct ChannelData
    {
        size_t n_source_values{0};          //!< source value table size
        size_t n_vertices{0};               //!< refined value count
        std::vector<IndexType> corners;     //!< refined per-corner indices
        Stencil stencil;                    //!< refined x source stencil
        std::vector<TexCoord> values;       //!< last evaluated values
    };

    bool built_{false};
    size_t levels_{0};

    std::vector<Vertex> source_vertices_;
    std::vector<Face> source_faces_;
    std::vector<Halfedge> source_corners_;

    size_t n_vertices_{0};
    std::vector<IndexType> face_corners_;
    std::vector<IndexType> face_sources_;
    Stencil position_stencil_;
    std::vector<Point> positions_;

    std::vector<ChannelData> channels_;
};

} // namespace pmp
