// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

// Demonstrates the corner-aware editing path: OBJ smoothing groups and
// explicit corner normals survive an edge split and an OBJ round-trip.

#include "pmp/algorithms/corner_edit.h"
#include "pmp/algorithms/normals.h"
#include "pmp/io/io_flags.h"
#include "pmp/io/read_obj.h"
#include "pmp/io/write_obj.h"
#include "pmp/surface_mesh.h"

#include <iostream>

using namespace pmp;

int main()
{
    // build a simple two-triangle mesh
    SurfaceMesh mesh;
    const Vertex v0 = mesh.add_vertex(Point(0, 0, 0));
    const Vertex v1 = mesh.add_vertex(Point(1, 0, 0));
    const Vertex v2 = mesh.add_vertex(Point(0, 1, 0));
    const Vertex v3 = mesh.add_vertex(Point(1, 1, 1));
    const Face f0 = mesh.add_triangle(v0, v1, v2);
    const Face f1 = mesh.add_triangle(v1, v3, v2);

    // corner properties produced by read_obj; create them manually here
    auto smoothing = mesh.face_property<int>("f:smoothing_group", 0);
    mesh.halfedge_property<Normal>("h:normal", Normal(0, 0, 0));
    mesh.halfedge_property<bool>("h:normal_explicit", false);
    mesh.halfedge_property<TexCoord>("h:tex", TexCoord(0, 0));

    // the two faces share a positive smoothing group, so they form one fan
    smoothing[f0] = 1;
    smoothing[f1] = 1;

    // pin one explicit corner normal, the rest stay automatic
    auto hnormal_explicit =
        mesh.get_halfedge_property<bool>("h:normal_explicit");
    auto hnormal = mesh.get_halfedge_property<Normal>("h:normal");
    for (auto h : mesh.halfedges(f0))
    {
        if (mesh.to_vertex(h) == v1)
        {
            hnormal_explicit[h] = true;
            hnormal[h] = normalize(Normal(0, 0, 1));
        }
    }

    update_corner_normals(mesh);

    // split the shared edge at a non-midpoint parameter
    const Halfedge h = mesh.find_halfedge(v1, v2);
    const Vertex vnew = split_edge_with_corners(mesh, h, Scalar(0.3));
    std::cout << "inserted vertex " << vnew << " at " << mesh.position(vnew)
              << '\n';

    // write smoothing groups and per-corner normals, then read them back
    IOFlags flags;
    flags.use_halfedge_normals = true;
    flags.use_halfedge_texcoords = true;
    write_obj(mesh, "corner_edit_example.obj", flags);

    SurfaceMesh roundtrip;
    read_obj(roundtrip, "corner_edit_example.obj");
    update_corner_normals(roundtrip);
    std::cout << "round-tripped mesh: " << roundtrip.n_vertices()
              << " vertices, " << roundtrip.n_faces() << " faces\n";
    return 0;
}
