#include <OpenVolumeMesh/Unstable/Topology/PlanePartition.hh>
#include <OpenVolumeMesh/FileManager/FileManager.hh>
#include <iostream>
#include <cmath>

using namespace OpenVolumeMesh;
using Vec = Geometry::Vec3d;

// Face vertex loops of a hexahedron (any consistent winding; the halfface
// pointing into the cell is selected geometrically).
static const int hex_faces[6][4] = {
    {3,2,1,0}, {7,6,5,4}, {1,2,6,7},
    {4,5,3,0}, {1,7,4,0}, {2,3,5,6},
};

static HalfFaceHandle inward_halfface(GeometricPolyhedralMeshV3d& m,
                                      FaceHandle fh, const Vec& center) {
    auto vs = m.get_halfface_vertices(fh.h0());
    Vec fc(0,0,0);
    for (auto v : vs) fc = fc + m.vertex(v);
    fc = fc * (1.0 / vs.size());
    Vec normal = (m.vertex(vs[1]) - m.vertex(vs[0]))
                     .cross(m.vertex(vs[2]) - m.vertex(vs[1]));
    return normal.dot(center - fc) > 0.0 ? fh.h0() : fh.h1();
}

static CellHandle add_hex(GeometricPolyhedralMeshV3d& m,
                          const std::vector<VertexHandle>& v) {
    Vec center(0,0,0);
    for (auto vh : v) center = center + m.vertex(vh);
    center = center * 0.125;
    std::vector<HalfFaceHandle> hfs;
    for (auto& f : hex_faces) {
        std::vector<VertexHandle> loop{v[f[0]], v[f[1]], v[f[2]], v[f[3]]};
        HalfFaceHandle found = m.find_halfface(loop);
        FaceHandle fh;
        if (found.is_valid()) {
            fh = found.face_handle();
        } else {
            fh = m.add_face(loop);
        }
        hfs.push_back(inward_halfface(m, fh, center));
    }
    return m.add_cell(hfs);
}

int main() {
    GeometricPolyhedralMeshV3d m;
    m.enable_bottom_up_incidences(true);
    m.enable_deferred_deletion(true);

    auto add_grid = [&](int ox) {
        std::vector<VertexHandle> v(8);
        for (int i = 0; i < 8; ++i) {
            double x = (i == 1 || i == 2 || i == 6 || i == 7) ? 1.0 : 0.0;
            double y = (i == 2 || i == 3 || i == 5 || i == 6) ? 1.0 : 0.0;
            double z = (i >= 4) ? 1.0 : 0.0;
            v[i] = m.add_vertex(Vec(x + ox, y, z));
        }
        return add_hex(m, v);
    };
    CellHandle c0 = add_grid(0);
    CellHandle c1 = add_grid(1);
    std::cout << "before: V=" << m.n_vertices() << " E=" << m.n_edges()
              << " F=" << m.n_faces() << " C=" << m.n_cells() << "\n";

    auto recs = partition_cells(m, {c0}, Vec(1.0, 0.0, 0.0), -0.5);
    std::cout << "records=" << recs.size() << "\n";
    for (auto& r : recs) {
        std::cout << " parent=" << r.parent << " neg=" << r.negative
                  << " pos=" << r.positive << " cap=" << r.cap << "\n";
    }
    std::cout << "after: V=" << m.n_vertices() << " E=" << m.n_edges()
              << " F=" << m.n_faces() << " C=" << m.n_cells() << "\n";

    // shell closure check
    for (CellHandle ch : m.cells()) {
        std::set<HEH> seen;
        size_t nhe = 0;
        for (HFH hfh : m.cell_halffaces(ch)) {
            for (HEH heh : m.halfface(hfh).halfedges()) {
                ++nhe;
                seen.insert(heh);
                seen.insert(heh.opposite_handle());
            }
        }
        size_t paired = 0;
        for (HEH h : seen) {
            size_t cnt = 0;
            for (HFH hfh : m.cell_halffaces(ch))
                for (HEH heh : m.halfface(hfh).halfedges())
                    if (heh == h) ++cnt;
            if (cnt == 1) ++paired;
        }
        std::cout << "cell " << ch << " halffaces=" << m.valence(ch)
                  << " halfedge-slots=" << nhe << " paired=" << paired
                  << " distinct=" << seen.size() << "\n";
        if (paired != seen.size()) { std::cout << "SHELL NOT CLOSED\n"; }
    }

    // incident cells of cap
    for (auto& r : recs) {
        auto cells = m.face_cells(r.cap);
        std::cout << "cap cells: " << cells[0] << " " << cells[1] << "\n";
    }

    // unselected neighbor c1: its halffaces
    std::cout << "neighbor " << c1 << " halffaces:";
    for (HFH hfh : m.cell_halffaces(c1)) std::cout << " " << hfh;
    std::cout << "\n";

    // circulator sanity
    size_t circ = 0;
    for (auto eh : m.edges()) {
        for (auto hfh : m.edge_halffaces(eh)) { (void)hfh; ++circ; }
    }
    std::cout << "edge-halfface incidences=" << circ << "\n";

    IO::FileManager io;
    if (!io.writeFile(std::string(getenv("TMPDIR")) + "/smoke.ovm", m))
        std::cout << "WRITE FAILED\n";
    GeometricPolyhedralMeshV3d n;
    if (!io.readFile(std::string(getenv("TMPDIR")) + "/smoke.ovm", n))
        std::cout << "READ FAILED\n";
    std::cout << "roundtrip C=" << n.n_cells() << " F=" << n.n_faces() << "\n";
    return 0;
}
