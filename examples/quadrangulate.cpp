// Quadrangulation example: merge triangles into quads, write the result to
// an OBJ file, read it back and verify that every directed corner kept its
// position, texture coordinate and corner normal.
#include <OpenGP/SurfaceMesh/SurfaceMesh.h>
#include <OpenGP/SurfaceMesh/Quadrangulate.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace OpenGP;

namespace {

// directed corner: position, texture coordinate, corner normal
struct Corner
{
    Vec3 p, t, n;
};

std::vector<Corner> collect_corners(const SurfaceMesh& mesh)
{
    auto tex = mesh.get_halfedge_property<Vec3>("h:texcoord");
    auto nrm = mesh.get_halfedge_property<Vec3>("h:normal");
    std::vector<Corner> out;
    for (auto f : mesh.faces())
    {
        auto hit = mesh.halfedges(f), hend = hit;
        do
        {
            Corner c;
            c.p = mesh.position(mesh.to_vertex(*hit));
            c.t = tex ? tex[*hit] : Vec3(0, 0, 0);
            c.n = nrm ? nrm[*hit] : Vec3(0, 0, 0);
            out.push_back(c);
        } while (++hit != hend);
    }
    return out;
}

bool same_corners(const std::vector<Corner>& a, const std::vector<Corner>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        for (int k = 0; k < 3; ++k)
        {
            if (std::abs(a[i].p[k] - b[i].p[k]) > 1e-6f) return false;
            if (std::abs(a[i].t[k] - b[i].t[k]) > 1e-6f) return false;
            if (std::abs(a[i].n[k] - b[i].n[k]) > 1e-6f) return false;
        }
    return true;
}

} // anonymous namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " output.obj\n";
        return 1;
    }
    const std::string out_path = argv[1];

    SurfaceMesh mesh;

    // two unit squares side by side, each split into two triangles
    auto v0 = mesh.add_vertex(Vec3(0, 0, 0));
    auto v1 = mesh.add_vertex(Vec3(1, 0, 0));
    auto v2 = mesh.add_vertex(Vec3(2, 0, 0));
    auto v3 = mesh.add_vertex(Vec3(0, 1, 0));
    auto v4 = mesh.add_vertex(Vec3(1, 1, 0));
    auto v5 = mesh.add_vertex(Vec3(2, 1, 0));

    auto t0 = mesh.add_triangle(v0, v1, v4);
    auto t1 = mesh.add_triangle(v0, v4, v3);
    auto t2 = mesh.add_triangle(v1, v2, v5);
    auto t3 = mesh.add_triangle(v1, v5, v4);

    // per-corner UVs and hard normals (continuous across the diagonals,
    // so the merges are allowed)
    auto tex = mesh.halfedge_property<Vec3>("h:texcoord");
    auto nrm = mesh.halfedge_property<Vec3>("h:normal");
    auto mat = mesh.face_property<int>("f:material", 0);
    (void)mat;

    auto set_corner = [&](SurfaceMesh::Face f, SurfaceMesh::Vertex v,
                          const Vec3& t, const Vec3& n)
    {
        for (auto h : mesh.halfedges(f))
            if (mesh.to_vertex(h) == v)
            {
                tex[h] = t;
                nrm[h] = n;
            }
    };

    const Vec3 up(0, 0, 1);
    set_corner(t0, v0, Vec3(0, 0, 1), up);
    set_corner(t0, v1, Vec3(1, 0, 1), up);
    set_corner(t0, v4, Vec3(1, 1, 1), up);
    set_corner(t1, v0, Vec3(0, 0, 1), up);
    set_corner(t1, v4, Vec3(1, 1, 1), up);
    set_corner(t1, v3, Vec3(0, 1, 1), up);
    set_corner(t2, v1, Vec3(0, 0, 1), up);
    set_corner(t2, v2, Vec3(1, 0, 1), up);
    set_corner(t2, v5, Vec3(1, 1, 1), up);
    set_corner(t3, v1, Vec3(0, 0, 1), up);
    set_corner(t3, v5, Vec3(1, 1, 1), up);
    set_corner(t3, v4, Vec3(0, 1, 1), up);

    std::vector<SurfaceMesh::Face> region = { t0, t1, t2, t3 };

    // run the globally optimal pairing
    std::vector<SurfaceMesh::Edge> removed = quadrangulate(mesh, region);
    if (removed.size() != 2)
    {
        std::cerr << "expected 2 merges, got " << removed.size() << "\n";
        return 1;
    }
    if (removed[0].idx() > removed[1].idx())
    {
        std::cerr << "returned edges are not sorted\n";
        return 1;
    }

    unsigned int quads = 0, tris = 0;
    for (auto f : mesh.faces())
    {
        if (mesh.valence(f) == 4) ++quads;
        if (mesh.valence(f) == 3) ++tris;
    }
    if (quads != 2 || tris != 0 || mesh.n_faces() != 2)
    {
        std::cerr << "unexpected face layout after merge\n";
        return 1;
    }

    // normals must still be computable on the new quads
    mesh.update_face_normals();
    for (auto f : mesh.faces())
        if (mesh.compute_face_normal(f).dot(up) < 0.999f)
        {
            std::cerr << "quad normal wrong\n";
            return 1;
        }

    const std::vector<Corner> before = collect_corners(mesh);

    // write and read back through the independent-index OBJ path
    if (!mesh.write(out_path))
    {
        std::cerr << "failed to write " << out_path << "\n";
        return 1;
    }

    SurfaceMesh readback;
    if (!readback.read(out_path))
    {
        std::cerr << "failed to read " << out_path << "\n";
        return 1;
    }

    const std::vector<Corner> after = collect_corners(readback);
    if (!same_corners(before, after))
    {
        std::cerr << "directed corners changed across OBJ roundtrip\n";
        return 1;
    }
    if (readback.n_faces() != 2 || !readback.is_quad_mesh())
    {
        std::cerr << "roundtrip mesh lost its quad layout\n";
        return 1;
    }

    std::cout << "quadrangulate example: " << removed.size()
              << " pairs merged, " << mesh.n_faces()
              << " quads, " << before.size()
              << " directed corners verified across OBJ roundtrip\n";
    return 0;
}
