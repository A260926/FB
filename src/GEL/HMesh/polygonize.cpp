//
//  polygonize.cpp
//  PointReconstruction
//
//  Created by J. Andreas Bærentzen on 16/03/13.
//  Copyright (c) 2013 J. Andreas Bærentzen. All rights reserved.
//

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

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

    /** Add the cube face separating the inside voxel pi from the outside
        neighbour reached through N6i[nbr_idx], and record the isosurface
        intersection point used for vertex placement. This is the single
        place where a face is generated, shared by the dense and the sparse
        entry points. */
    inline void emit_face(Manifold& mani,
                          std::vector<Vec3d>& edge_intersections,
                          const Vec3i& pi,
                          int nbr_idx,
                          float t)
    {
        Vec3d p(pi);
        Vec3i pni = pi + N6i[nbr_idx];
        edge_intersections.push_back(p * (1-t) + Vec3d(pni) * t);
        std::array<Vec3d, 4> quad_vertices;
        for(int n=0;n<4;++n)
            quad_vertices[n] = p + hex_faces[nbr_idx][3-n];
        mani.add_face(quad_vertices);
    }

    /** Shared post-processing: stitch the quads, place the vertices on the
        isosurface (dual connectivity or marching cubes connectivity), apply
        the inverse transform, and optionally triangulate. Both the dense and
        the sparse entry point use this exact code so they cannot diverge in
        their definition of the surface. */
    Manifold finish_polygonization(Manifold& mani,
                                   std::vector<Vec3d>& edge_intersections,
                                   const XForm& xform,
                                   bool make_triangles,
                                   bool dual_connectivity)
    {
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
        Manifold mani;

        auto is_inside = [&](const Vec3i& pi) {
            float val = grid[pi];
            return !std::isnan(val) && (high_is_inside == (val > tau));
        };

        auto is_outside = [&](const Vec3i& pi) {
            if (grid.in_domain(pi)) {
                float val = grid[pi];
                return !std::isnan(val) && (high_is_inside == (val <= tau));
            }
            return true;

        };

        vector<Vec3d> edge_intersections;
        for(Vec3i pi: Range3D(grid.get_dims()))
            if(is_inside(pi)) {
                for (int nbr_idx = 0; nbr_idx < 6 ; ++ nbr_idx) {
                    Vec3i pni = pi + N6i[nbr_idx];
                    if(is_outside(pni)) {
                        float t = 0;
                        if (grid.in_domain(pni)) {
                            float va = grid[pi];
                            float vb = grid[pni];
                            t = (tau-va)/(vb - va);
                        }
                        emit_face(mani, edge_intersections, pi, nbr_idx, t);
                    }
                }
            }

        return finish_polygonization(mani, edge_intersections, xform,
                                     make_triangles, dual_connectivity);
    }

    namespace {
        /** Value accessor for the sparse grid. It counts the scalar samples
            actually read. For a coalesced (uniform) brick the single
            representative value is read once and then cached, so it is
            counted once rather than once per voxel it represents. */
        class HGridSampler
        {
            const Geometry::HGrid<float>& grid;
            SparsePolygonizeStats* stats;
            const Vec3i top_dims;
            const int bd;
            struct CellCache
            {
                bool cached = false;
                float value = 0;
            };
            std::vector<CellCache> cache;

            int top_index(int tx, int ty, int tz) const
            {
                return (tz*top_dims[1]+ty)*top_dims[0]+tx;
            }

        public:
            HGridSampler(const Geometry::HGrid<float>& _grid,
                         SparsePolygonizeStats* _stats)
                : grid(_grid), stats(_stats),
                  top_dims(_grid.get_top_dims()), bd(_grid.get_bottom_dim()),
                  cache(static_cast<size_t>(top_dims[0])*top_dims[1]*top_dims[2]) {}

            /// Read the scalar sample at p (p must be in the grid domain).
            float read(const Vec3i& p)
            {
                const int tx = p[0]/bd;
                const int ty = p[1]/bd;
                const int tz = p[2]/bd;
                const int ti = top_index(tx, ty, tz);
                const auto& cell = grid.get_cell(ti);
                if (cell.is_coalesced()) {
                    CellCache& c = cache[ti];
                    if (!c.cached) {
                        c.value = cell[Vec3i(0)];
                        if (stats) ++stats->sample_reads;
                        c.cached = true;
                    }
                    return c.value;
                }
                if (stats) ++stats->sample_reads;
                return cell[p];
            }

            /// Representative value of a top level cell.
            float read_cell(int tx, int ty, int tz)
            {
                const Vec3i p(tx*bd, ty*bd, tz*bd);
                return read(p);
            }

            /// Is the top level cell coalesced?
            bool cell_is_coalesced(int tx, int ty, int tz) const
            {
                return grid.get_cell(top_index(tx, ty, tz)).is_coalesced();
            }
        };
    }

    HMesh::Manifold volume_polygonize(const XForm& xform, const Geometry::HGrid<float>& grid,
                                      float tau, bool make_triangles, bool high_is_inside,
                                      bool dual_connectivity, SparsePolygonizeStats* stats)
    {
        if (stats)
            *stats = SparsePolygonizeStats();

        Manifold mani;
        std::vector<Vec3d> edge_intersections;

        HGridSampler sampler(grid, stats);

        const Vec3i dims = grid.get_dims();
        const Vec3i top_dims = grid.get_top_dims();
        const int bd = grid.get_bottom_dim();

        auto is_inside_value = [&](float val) {
            return !std::isnan(val) && (high_is_inside == (val > tau));
        };
        auto is_outside_value = [&](float val) {
            return !std::isnan(val) && (high_is_inside == (val <= tau));
        };

        /// The per-voxel face determination. This is the same test as in the
        /// dense entry point above, except that values are obtained through
        /// the counting sampler.
        auto visit = [&](const Vec3i& pi) {
            if (stats) ++stats->visited_voxels;
            float va = sampler.read(pi);
            if (!is_inside_value(va))
                return;
            for (int nbr_idx = 0; nbr_idx < 6; ++nbr_idx) {
                Vec3i pni = pi + N6i[nbr_idx];
                bool in_domain = grid.in_domain(pni);
                float vb = 0;
                if (in_domain)
                    vb = sampler.read(pni);
                bool outside = in_domain ? is_outside_value(vb) : true;
                if (outside) {
                    float t = 0;
                    if (in_domain)
                        t = (tau-va)/(vb - va);
                    emit_face(mani, edge_intersections, pi, nbr_idx, t);
                }
            }
        };

        // Marks the boundary voxels of a coalesced brick that must be
        // examined because the neighbouring brick can have a different
        // classification. Only these voxels are visited; the interior of a
        // uniform brick is never scanned.
        std::vector<unsigned char> seen(static_cast<size_t>(bd)*bd*bd, 0);

        // The voxels that need a per-voxel face determination. They are
        // collected from the bricks and then sorted into the same
        // lexicographic order (x fastest, then y, then z) that the dense
        // traversal uses, so that both entry points emit their faces in the
        // same order. The subsequent stitching and vertex projection are
        // order sensitive; matching the order makes the two surfaces
        // identical, not merely equivalent.
        std::vector<Vec3i> voxels;

        for (int tz = 0; tz < top_dims[2]; ++tz)
            for (int ty = 0; ty < top_dims[1]; ++ty)
                for (int tx = 0; tx < top_dims[0]; ++tx) {
                    const Vec3i toff(tx*bd, ty*bd, tz*bd);
                    const Vec3i ext = CGLA::v_min(Vec3i(bd), dims - toff);

                    if (!sampler.cell_is_coalesced(tx, ty, tz)) {
                        // A modified brick: examine all its in-domain voxels.
                        for (int z = 0; z < ext[2]; ++z)
                            for (int y = 0; y < ext[1]; ++y)
                                for (int x = 0; x < ext[0]; ++x)
                                    voxels.push_back(toff + Vec3i(x, y, z));
                        continue;
                    }

                    const float cell_val = sampler.read_cell(tx, ty, tz);
                    // If the whole uniform brick is not inside, no voxel in
                    // it can generate a face.
                    if (!is_inside_value(cell_val))
                        continue;

                    // Find the sides through which a transition to a
                    // differently valued brick (or out of the domain) can
                    // occur.
                    bool relevant[6];
                    for (int d = 0; d < 6; ++d) {
                        const int axis = d/2;
                        Vec3i nt(tx, ty, tz);
                        nt[axis] += (d%2 ? 1 : -1);
                        if (nt[0] < 0 || nt[1] < 0 || nt[2] < 0 ||
                            nt[0] >= top_dims[0] || nt[1] >= top_dims[1] ||
                            nt[2] >= top_dims[2]) {
                            // Beyond the top level grid: outside the domain.
                            relevant[d] = true;
                        }
                        else if (sampler.cell_is_coalesced(nt[0], nt[1], nt[2])) {
                            const float nval = sampler.read_cell(nt[0], nt[1], nt[2]);
                            relevant[d] = !(nval == cell_val);
                        }
                        else {
                            // Neighbouring brick has been edited.
                            relevant[d] = true;
                        }
                    }

                    // Collect the (deduplicated) boundary voxels on the
                    // relevant sides.
                    std::fill(seen.begin(), seen.end(), 0);
                    for (int d = 0; d < 6; ++d) {
                        if (!relevant[d])
                            continue;
                        const int axis = d/2;
                        const int fixed = (d%2 ? ext[axis]-1 : 0);
                        const int a1 = (axis+1)%3;
                        const int a2 = (axis+2)%3;
                        Vec3i l(0);
                        l[axis] = fixed;
                        for (int u = 0; u < ext[a1]; ++u)
                            for (int v = 0; v < ext[a2]; ++v) {
                                l[a1] = u;
                                l[a2] = v;
                                const size_t li = (static_cast<size_t>(l[2])*bd + l[1])*bd + l[0];
                                if (!seen[li]) {
                                    seen[li] = 1;
                                    voxels.push_back(toff + l);
                                }
                            }
                    }
                }

        std::sort(voxels.begin(), voxels.end(), [](const Vec3i& a, const Vec3i& b) {
            if (a[2] != b[2]) return a[2] < b[2];
            if (a[1] != b[1]) return a[1] < b[1];
            return a[0] < b[0];
        });

        for (const Vec3i& p: voxels)
            visit(p);

        return finish_polygonization(mani, edge_intersections, xform,
                                     make_triangles, dual_connectivity);
    }

}
