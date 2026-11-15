// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/algorithms/corner_edit.h"

#include "pmp/algorithms/normals.h"
#include "pmp/exceptions.h"

#include <limits>
#include <vector>

namespace pmp {

namespace {

// Per-side snapshot of the three corners of the triangle adjacent to the
// edge that is about to be split.
struct SideData
{
    bool present{false};
    Face parent;
    Vertex a;             // edge start vertex
    Vertex b;             // edge end vertex
    Vertex c;             // opposite vertex
    int smoothing{0};

    bool has_tex{false};
    TexCoord tex_a, tex_b, tex_c;
    TexCoord tex_v;       // interpolated texture coordinate at the new vertex

    bool explicit_a{false};
    bool explicit_b{false};
    bool explicit_c{false};
    Normal normal_a, normal_b, normal_c;
    bool explicit_v{false};
    Normal normal_v;
};

// Find the halfedge of face \p f whose target vertex is \p v.
Halfedge corner_halfedge(const SurfaceMesh& mesh, Face f, Vertex v)
{
    for (auto h : mesh.halfedges(f))
        if (mesh.to_vertex(h) == v)
            return h;
    return {};
}

// Whether face \p f contains vertex \p v.
bool contains_vertex(const SurfaceMesh& mesh, Face f, Vertex v)
{
    for (auto vx : mesh.vertices(f))
        if (vx == v)
            return true;
    return false;
}

} // namespace

Vertex split_edge_with_corners(SurfaceMesh& mesh, Halfedge h, Scalar t)
{
    // ---- validate inputs before touching the mesh -------------------------
    if (!h.is_valid() || !mesh.is_valid(h) || mesh.is_deleted(h))
        throw InvalidInputException("split_edge_with_corners: invalid halfedge");

    if (!(t > Scalar(0.0) && t < Scalar(1.0)))
        throw InvalidInputException(
            "split_edge_with_corners: parameter t must satisfy 0 < t < 1");

    const Halfedge oh = mesh.opposite_halfedge(h);
    const Face f0 = mesh.face(h);
    const Face f1 = mesh.face(oh);

    if (f0.is_valid() && mesh.valence(f0) != 3)
        throw InvalidInputException(
            "split_edge_with_corners: adjacent face must be a triangle");
    if (f1.is_valid() && mesh.valence(f1) != 3)
        throw InvalidInputException(
            "split_edge_with_corners: adjacent face must be a triangle");
    if (!f0.is_valid() && !f1.is_valid())
        throw InvalidInputException(
            "split_edge_with_corners: halfedge is not incident to any face");

    // look up the corner properties without creating them yet, so that a
    // rejected call never changes the mesh's property set
    auto tex = mesh.get_halfedge_property<TexCoord>("h:tex");
    auto hnormal = mesh.get_halfedge_property<Normal>("h:normal");
    auto hnormal_explicit =
        mesh.get_halfedge_property<bool>("h:normal_explicit");
    auto smoothing = mesh.get_face_property<int>("f:smoothing_group");

    const Vertex va = mesh.from_vertex(h);
    const Vertex vb = mesh.to_vertex(h);

    // ---- snapshot the (up to two) incident triangle sides -----------------
    SideData sides[2];
    const Face side_face[2] = {f0, f1};

    for (int s = 0; s < 2; ++s)
    {
        const Face f = side_face[s];
        if (!f.is_valid())
            continue;

        SideData& d = sides[s];
        d.present = true;
        d.parent = f;
        d.a = va;
        d.b = vb;
        d.smoothing = smoothing ? smoothing[f] : 0;

        for (auto vx : mesh.vertices(f))
            if (vx != va && vx != vb)
                d.c = vx;

        const Halfedge ha = corner_halfedge(mesh, f, va);
        const Halfedge hb = corner_halfedge(mesh, f, vb);
        const Halfedge hc = corner_halfedge(mesh, f, d.c);

        if (tex)
        {
            d.has_tex = true;
            d.tex_a = tex[ha];
            d.tex_b = tex[hb];
            d.tex_c = tex[hc];
            d.tex_v = d.tex_a * (Scalar(1.0) - t) + d.tex_b * t;
        }

        d.explicit_a = hnormal_explicit && hnormal_explicit[ha];
        d.explicit_b = hnormal_explicit && hnormal_explicit[hb];
        d.explicit_c = hnormal_explicit && hnormal_explicit[hc];
        if (hnormal)
        {
            d.normal_a = hnormal[ha];
            d.normal_b = hnormal[hb];
            d.normal_c = hnormal[hc];
        }

        if (d.explicit_a && d.explicit_b)
        {
            const Normal interpolated =
                d.normal_a * (Scalar(1.0) - t) + d.normal_b * t;
            const Scalar len = norm(interpolated);
            if (!(len > std::numeric_limits<Scalar>::min()))
                throw InvalidInputException(
                    "split_edge_with_corners: interpolated explicit normal is "
                    "the zero vector");
            d.explicit_v = true;
            d.normal_v = interpolated / len;
        }
    }

    // all preconditions have passed: make sure the corner properties exist
    // before the topology is changed
    if (!tex)
        tex = mesh.halfedge_property<TexCoord>("h:tex");
    if (!hnormal)
        hnormal = mesh.halfedge_property<Normal>("h:normal");
    if (!hnormal_explicit)
        hnormal_explicit =
            mesh.halfedge_property<bool>("h:normal_explicit", false);

    // ---- perform the real topological split -------------------------------
    const Point p = mesh.position(va) * (Scalar(1.0) - t) +
                    mesh.position(vb) * t;
    const Vertex v = mesh.add_vertex(p);
    mesh.split(mesh.edge(h), v);

    // ---- transfer corner data onto the resulting faces --------------------
    for (auto f : mesh.faces(v))
    {
        const SideData* side = nullptr;
        for (int s = 0; s < 2; ++s)
        {
            if (sides[s].present && contains_vertex(mesh, f, sides[s].c))
            {
                side = &sides[s];
                break;
            }
        }
        if (!side)
            continue;

        if (smoothing)
            smoothing[f] = side->smoothing;

        for (auto ch : mesh.halfedges(f))
        {
            const Vertex w = mesh.to_vertex(ch);

            if (tex)
            {
                if (w == v)
                    tex[ch] = side->tex_v;
                else if (w == side->a)
                    tex[ch] = side->tex_a;
                else if (w == side->b)
                    tex[ch] = side->tex_b;
                else if (w == side->c)
                    tex[ch] = side->tex_c;
            }

            if (w == v)
            {
                hnormal_explicit[ch] = side->explicit_v;
                if (side->explicit_v)
                    hnormal[ch] = side->normal_v;
            }
            else if (w == side->a)
            {
                hnormal_explicit[ch] = side->explicit_a;
                hnormal[ch] = side->normal_a;
            }
            else if (w == side->b)
            {
                hnormal_explicit[ch] = side->explicit_b;
                hnormal[ch] = side->normal_b;
            }
            else if (w == side->c)
            {
                hnormal_explicit[ch] = side->explicit_c;
                hnormal[ch] = side->normal_c;
            }
        }
    }

    // ---- refresh the automatic corners ------------------------------------
    update_corner_normals(mesh);

    return v;
}

} // namespace pmp
