/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

/**
 * @file examples/fair_hole.cpp
 * @brief Minimal, dependency-free demonstration of fill_fair_hole and the
 * attributed OBJ channels.
 *
 * The program builds a planar annulus with an open inner boundary, assigns
 * per-corner texture coordinates and explicit normals outside the hole,
 * fills the hole fairly with two refinement rounds and writes the result
 * (including the generated patch attributes) as a Wavefront OBJ.
 *
 * Interface summary:
 *  - HMesh::AttributedManifold bundles a Manifold with halfedge indexed
 *    texcoords/normals and unsigned char validity masks.
 *  - HMesh::fill_fair_hole(am, boundary_halfedge, rounds) caps and fairly
 *    smooths one simple convex hole and returns the new surviving vertices
 *    and faces together with the achieved energy and gradient residual.
 *  - HMesh::obj_save/obj_load overloads accept an AttributedManifold and
 *    preserve independent per-corner v/vt/vn indices and masks.
 *
 * Build through task_support/Makefile; no windowing or GPU is used.
 */

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include <GEL/HMesh/HolePatch.h>
#include <GEL/HMesh/cleanup.h>
#include <GEL/HMesh/obj_load.h>
#include <GEL/HMesh/obj_save.h>

using namespace HMesh;
using CGLA::Vec2d;
using CGLA::Vec3d;

namespace
{
    // Three concentric rings; the inner ring bounds the hole, the two outer
    // rings form the existing surface. All faces are triangles.
    AttributedManifold build_annulus(int n)
    {
        std::vector<Vec3d> verts;
        for (int ring = 0; ring < 3; ++ring) {
            const double rad = 1.0 + ring;
            for (int i = 0; i < n; ++i) {
                const double a = 2.0 * M_PI * i / n;
                verts.emplace_back(rad * std::cos(a), rad * std::sin(a),
                                   0.2 * std::sin(2.0 * a) * (ring == 0 ? 1.0 : 0.3));
            }
        }
        auto id = [&](int r, int i) { return r * n + (i % n); };
        std::vector<std::array<int, 3>> tris;
        for (int r = 0; r < 2; ++r)
            for (int i = 0; i < n; ++i) {
                tris.push_back({id(r, i), id(r + 1, i), id(r, i + 1)});
                tris.push_back({id(r, i + 1), id(r + 1, i), id(r + 1, i + 1)});
            }

        AttributedManifold am;
        VertexAttributeVector<int> cluster_id;
        for (const auto& t : tris) {
            FaceID f = am.mesh.add_face({verts[t[0]], verts[t[1]], verts[t[2]]});
            int j = 0;
            circulate_face_ccw(am.mesh, f, [&](VertexID v) { cluster_id[v] = t[j++]; });
        }
        stitch_mesh(am.mesh, cluster_id);
        IDRemap remap;
        am.mesh.cleanup(remap);

        // Per-corner UVs and explicit normals on every existing corner.
        for (FaceID f : am.mesh.faces()) {
            for (Walker w = am.mesh.walker(f); !w.full_circle(); w = w.circulate_face_ccw()) {
                const Vec3d& p = am.mesh.pos(w.vertex());
                HalfEdgeID h = w.halfedge();
                am.texcoords[h] = Vec2d(0.5 + 0.25 * p[0], 0.5 + 0.25 * p[1]);
                am.has_texcoord[h] = 1;
                Vec3d nn = am.mesh.normal(f);
                am.normals[h] = nn;
                am.has_normal[h] = 1;
            }
        }
        return am;
    }
}

int main(int argc, char** argv)
{
    const std::string out_path = argc > 1 ? argv[1] : ".task-build/fair_hole_example.obj";

    try {
        AttributedManifold am = build_annulus(12);

        HalfEdgeID hole = InvalidHalfEdgeID;
        for (HalfEdgeID h : am.mesh.halfedges())
            if (am.mesh.walker(h).face() == InvalidFaceID) { hole = h; break; }
        if (hole == InvalidHalfEdgeID)
            throw std::runtime_error("test mesh unexpectedly has no hole");

        const unsigned rounds = 2;
        HolePatchResult result = fill_fair_hole(am, hole, rounds);

        std::cout << "fill_fair_hole: new_vertices=" << result.vertices.size()
                  << " new_faces=" << result.faces.size()
                  << " energy=" << result.energy
                  << " residual=" << result.residual << "\n";
        if (result.residual > 1e-8)
            throw std::runtime_error("gradient residual above 1e-8");

        am.cleanup();
        if (!obj_save(out_path, am))
            throw std::runtime_error("obj_save failed");

        AttributedManifold reloaded;
        if (!obj_load(out_path, reloaded))
            throw std::runtime_error("obj_load of the written file failed");
        std::cout << "wrote and reloaded " << out_path
                  << " vertices=" << reloaded.mesh.no_vertices()
                  << " faces=" << reloaded.mesh.no_faces() << "\n";
        std::cout << "FAIR_HOLE_EXAMPLE_OK\n";
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "example failed: " << e.what() << "\n";
        return 1;
    }
}
