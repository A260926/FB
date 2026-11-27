//
//  polygonize.cpp
//  PointReconstruction
//
//  Created by J. Andreas Bærentzen on 16/03/13.
//  Copyright (c) 2013 J. Andreas Bærentzen. All rights reserved.
//

#include <GEL/Geometry/GridAlgorithm.h>
#include <GEL/Geometry/Implicit.h>
#include <GEL/Geometry/Neighbours.h>
#include <GEL/HMesh/polygonize.h>
#include <GEL/HMesh/mesh_optimization.h>
#include <GEL/HMesh/smooth.h>
#include <GEL/HMesh/cleanup.h>
#include <GEL/HMesh/triangulate.h>
#include <GEL/HMesh/dual.h>

using namespace std;
using namespace CGLA;
using namespace Geometry;
using namespace HMesh;

namespace {
    const Vec3d hex_faces[6][4] = {
        {Vec3d(-0.5,-0.5,-0.5), Vec3d(-0.5,0.5,-0.5),   Vec3d(-0.5,0.5,0.5),    Vec3d(-0.5,-0.5,0.5)},
        {Vec3d(0.5, 0.5,-0.5),  Vec3d(0.5,-0.5,-0.5),   Vec3d(0.5,-0.5,0.5),    Vec3d(0.5,0.5,0.5)},
        {Vec3d( 0.5,-0.5, -0.5),Vec3d(-0.5,-0.5, -0.5), Vec3d(-0.5,-0.5, 0.5),  Vec3d(0.5,-0.5, 0.5)},
        {Vec3d(-0.5,0.5, -0.5), Vec3d(0.5,0.5, -0.5),   Vec3d(0.5,0.5, 0.5),    Vec3d(-0.5,0.5, 0.5)},
        {Vec3d(-0.5,-0.5,-0.5), Vec3d(0.5,-0.5,-0.5),   Vec3d(0.5,0.5,-0.5),    Vec3d(-0.5,0.5,-0.5)},
        {Vec3d( 0.5,-0.5,0.5),  Vec3d(-0.5,-0.5,0.5),   Vec3d(-0.5,0.5,0.5),    Vec3d(0.5,0.5,0.5)}
    };

    /** Accumulates the cuberille quads produced by inside voxels bordering
        outside (or out of domain) voxels. Used by both the dense (RGrid) and
        the sparse (HGrid) traversal so that both define exactly the same
        surface. */
    struct CuberilleBuilder {
        Manifold mani;
        vector<Vec3d> edge_intersections;
        float tau = 0;
        bool high_is_inside = true;

        bool is_inside_val(float val) const {
            return !std::isnan(val) && (high_is_inside == (val > tau));
        }

        bool is_outside_val(float val) const {
            return !std::isnan(val) && (high_is_inside == (val <= tau));
        }

        /** Emit the quad that the inside voxel at pi sends across the face
            towards the neighbor in direction nbr_idx. t is the linear
            interpolation parameter of the isocontour crossing (0 if the
            neighbor is outside the grid domain). */
        void emit_quad(const Vec3i& pi, int nbr_idx, float t) {
            Vec3d p(pi);
            Vec3i pni = pi + N6i[nbr_idx];
            edge_intersections.push_back(p * (1-t) + Vec3d(pni) * t);
            std::array<Vec3d, 4> quad_vertices;
            for(int n=0;n<4;++n)
                quad_vertices[n] = p + hex_faces[nbr_idx][3-n];
            mani.add_face(quad_vertices);
        }
    };

    /** Everything that happens after the cuberille quads have been generated:
        stitching, conversion to dual or marching cubes connectivity, vertex
        projection onto the isocontour, transformation back to object
        coordinates and (optionally) triangulation. Shared by the dense and
        the sparse entry points. */
    Manifold finish_polygonize(CuberilleBuilder& builder,
                               const XForm& xform,
                               bool make_triangles,
                               bool dual_connectivity)
    {
        Manifold& mani = builder.mani;
        const vector<Vec3d>& edge_intersections = builder.edge_intersections;

        stitch_mesh(mani, 1e-10);

        if (dual_connectivity) {
            for(auto v: mani.vertices()) {
                Vec3d p(0);
                int cnt = 0;
                for (auto f: mani.incident_faces(v))
                    if (mani.in_use(f)) {
                        auto id = f.get_index();
                        p += edge_intersections[id];
                        ++cnt;
                }
                mani.pos(v) = p/cnt;
            }
        }
        else {
            Manifold mc_mesh;
            VertexAttributeVector<int> cluster_id(-1);
            for (auto v: mani.vertices()) {
                vector<Vec3d> points;
                vector<int> ids;
                ids.reserve(20);
                for (auto f: mani.incident_faces(v))
                    if (mani.in_use(f)) {
                        auto id = f.get_index();
                        points.push_back(edge_intersections[id]);
                        ids.push_back(id);
                    }
                if(points.size()>2) {
                    FaceID f_mc = mc_mesh.add_face(points);
                    int i = 0;
                    for (auto vv: mc_mesh.incident_vertices(f_mc))
                        cluster_id[vv] = ids[i++];
                }
            }
            stitch_mesh(mc_mesh, cluster_id);
            mani = mc_mesh;
        }

        for(auto v: mani.vertices())
            mani.pos(v) = xform.inverse(mani.pos(v));

        if(make_triangles) {
            triangulate(mani, SHORTEST_EDGE);
            remove_needles(mani, 0.1, true);
        }

        mani.cleanup();

        return mani;
    }
}

namespace  HMesh {

    HMesh::Manifold volume_polygonize(const XForm& xform, const Geometry::RGrid<float>& grid,
                                      float tau, bool make_triangles, bool high_is_inside,
                                      bool dual_connectivity)
    {
        CuberilleBuilder builder;
        builder.tau = tau;
        builder.high_is_inside = high_is_inside;

        for(Vec3i pi: Range3D(grid.get_dims())) {
            float va = grid[pi];
            if(builder.is_inside_val(va)) {
                for (int nbr_idx = 0; nbr_idx < 6 ; ++ nbr_idx) {
                    Vec3i pni = pi + N6i[nbr_idx];
                    bool outside;
                    float t = 0;
                    if (grid.in_domain(pni)) {
                        float vb = grid[pni];
                        outside = builder.is_outside_val(vb);
                        if (outside)
                            t = (tau-va)/(vb - va);
                    }
                    else
                        outside = true;
                    if (outside)
                        builder.emit_quad(pi, nbr_idx, t);
                }
            }
        }

        return finish_polygonize(builder, xform, make_triangles, dual_connectivity);
    }

    HMesh::Manifold volume_polygonize(const XForm& xform, const Geometry::HGrid<float>& grid,
                                      float tau, bool make_triangles, bool high_is_inside,
                                      bool dual_connectivity, SparsePolygonizeStats* stats)
    {
        if (stats) {
            stats->visited_voxels = 0;
            stats->sample_reads = 0;
        }

        CuberilleBuilder builder;
        builder.tau = tau;
        builder.high_is_inside = high_is_inside;

        const Vec3i dims = grid.get_dims();
        const int bdim = grid.get_bottom_dim();
        const Vec3i top_dims = grid.get_top_dims();

        // Reading a scalar sample: for a coalesced brick this transparently
        // yields the single representative value, which is precisely the one
        // read we account for.
        auto sample = [&](const Vec3i& p) -> float {
            if (stats)
                ++stats->sample_reads;
            return grid[p];
        };

        // The brick-wise traversal discovers quads in a different order than
        // the dense voxel scan. To make the surface literally identical to
        // the dense entry point (and not just geometrically equal), the
        // quads are collected and replayed in dense emission order: voxel
        // linear index (x fastest), then neighbor direction.
        struct PendingQuad {
            size_t key;
            Vec3i pi;
            int nbr_idx;
            float t;
        };
        vector<PendingQuad> pending;
        auto emit = [&](const Vec3i& pi, int nbr_idx, float t) {
            size_t lin = (size_t(pi[2])*dims[1] + pi[1])*dims[0] + pi[0];
            pending.push_back({lin*6 + size_t(nbr_idx), pi, nbr_idx, t});
        };

        for(Vec3i bt: Range3D(top_dims)) {
            const Vec3i org = bt * bdim;
            // Valid voxels of this brick: [org, valid_hi). The padding of
            // the final bricks is not part of the voxel domain.
            const Vec3i valid_hi = v_min(org + Vec3i(bdim), dims);
            const auto& cell = grid.get_cell(bt);

            if (cell.is_coalesced()) {
                // Uniform brick: a single representative read classifies
                // every voxel inside it.
                float va = sample(org);
                if (!builder.is_inside_val(va))
                    continue; // Uniformly outside (or NaN): no faces at all.
                // Only voxels on the brick boundary can neighbor a different
                // value, so only the six brick faces are examined.
                for (int nbr_idx = 0; nbr_idx < 6; ++nbr_idx) {
                    const int axis = nbr_idx >> 1;
                    const bool neg = (nbr_idx & 1) == 0;
                    Vec3i face_lo = org, face_hi = valid_hi;
                    face_lo[axis] = neg ? org[axis] : valid_hi[axis]-1;
                    face_hi[axis] = face_lo[axis]+1;
                    const int ncoord = face_lo[axis] + (neg ? -1 : 1);
                    if (ncoord < 0 || ncoord >= dims[axis]) {
                        // The whole face borders the outside of the domain.
                        for(Vec3i pi: Range3D(face_lo, face_hi)) {
                            if (stats)
                                ++stats->visited_voxels;
                            emit(pi, nbr_idx, 0);
                        }
                        continue;
                    }
                    const auto& ncell = grid.get_cell(bt + N6i[nbr_idx]);
                    if (ncell.is_coalesced()) {
                        // Uniform neighbor: one representative read decides
                        // the fate of the entire face.
                        float vb = sample(face_lo + N6i[nbr_idx]);
                        if (!builder.is_outside_val(vb))
                            continue;
                        float t = (tau-va)/(vb - va);
                        for(Vec3i pi: Range3D(face_lo, face_hi)) {
                            if (stats)
                                ++stats->visited_voxels;
                            emit(pi, nbr_idx, t);
                        }
                    }
                    else {
                        // Non-uniform neighbor: per voxel determination
                        // along the face.
                        for(Vec3i pi: Range3D(face_lo, face_hi)) {
                            if (stats)
                                ++stats->visited_voxels;
                            float vb = sample(pi + N6i[nbr_idx]);
                            if (builder.is_outside_val(vb)) {
                                float t = (tau-va)/(vb - va);
                                emit(pi, nbr_idx, t);
                            }
                        }
                    }
                }
            }
            else {
                // Non-uniform brick: plain per voxel traversal, exactly as
                // in the dense version.
                for(Vec3i pi: Range3D(org, valid_hi)) {
                    if (stats)
                        ++stats->visited_voxels;
                    float va = sample(pi);
                    if(builder.is_inside_val(va)) {
                        for (int nbr_idx = 0; nbr_idx < 6 ; ++ nbr_idx) {
                            Vec3i pni = pi + N6i[nbr_idx];
                            bool outside;
                            float t = 0;
                            if (grid.in_domain(pni)) {
                                float vb = sample(pni);
                                outside = builder.is_outside_val(vb);
                                if (outside)
                                    t = (tau-va)/(vb - va);
                            }
                            else
                                outside = true;
                            if (outside)
                                emit(pi, nbr_idx, t);
                        }
                    }
                }
            }
        }

        // Replay the quads in dense emission order.
        sort(pending.begin(), pending.end(),
             [](const PendingQuad& a, const PendingQuad& b){ return a.key < b.key; });
        for (const auto& pq : pending)
            builder.emit_quad(pq.pi, pq.nbr_idx, pq.t);

        return finish_polygonize(builder, xform, make_triangles, dual_connectivity);
    }

}
