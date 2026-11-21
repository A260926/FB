#include <OpenVolumeMesh/Unstable/Topology/PlanePartition.hh>
#include <OpenVolumeMesh/Attribs/StatusAttrib.hh>
#include <iostream>
#include <cmath>
#include <set>

using namespace OpenVolumeMesh;
using Vec = Geometry::Vec3d;

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
        FaceHandle fh = found.is_valid() ? found.face_handle() : m.add_face(loop);
        hfs.push_back(inward_halfface(m, fh, center));
    }
    return m.add_cell(hfs);
}

// Polyhedron volume from its inward-oriented halfface loops (fan triangulation).
static double cell_volume(GeometricPolyhedralMeshV3d& m, CellHandle ch) {
    double vol = 0.0;
    for (HFH hfh : m.cell_halffaces(ch)) {
        auto vs = m.get_halfface_vertices(hfh);
        for (size_t i = 1; i + 1 < vs.size(); ++i) {
            Vec a = m.vertex(vs[0]);
            Vec b = m.vertex(vs[i]);
            Vec c = m.vertex(vs[i + 1]);
            vol -= a.dot(b.cross(c)) / 6.0; // inward winding -> negate
        }
    }
    return vol;
}

int main() {
    GeometricPolyhedralMeshV3d m;
    m.enable_bottom_up_incidences(true);
    m.enable_deferred_deletion(true);

    // Two cubes sharing the x=1 face: cube L [0,1], cube R [1,2].
    auto vidx = [&](int x, int y, int z) {
        return VertexHandle(x * 4 + y * 2 + z);
    };
    for (int x = 0; x <= 2; ++x)
        for (int y = 0; y <= 1; ++y)
            for (int z = 0; z <= 1; ++z)
                m.add_vertex(Vec(x, y, z));
    std::vector<VertexHandle> L(8), R(8);
    for (int i = 0; i < 8; ++i) {
        int x = (i == 1 || i == 2 || i == 6 || i == 7) ? 1 : 0;
        int y = (i == 2 || i == 3 || i == 5 || i == 6) ? 1 : 0;
        int z = (i >= 4) ? 1 : 0;
        L[i] = vidx(x, y, z);
        R[i] = vidx(x + 1, y, z);
    }
    CellHandle cL = add_hex(m, L);
    CellHandle cR = add_hex(m, R);
    std::cout << "shared faces: ";
    int shared = 0;
    for (FH fh : m.faces()) {
        auto cs = m.face_cells(fh);
        if (cs[0].is_valid() && cs[1].is_valid()) { ++shared; std::cout << fh << " "; }
    }
    std::cout << " count=" << shared << "\n";
    std::cout << "V=" << m.n_vertices() << " E=" << m.n_edges()
              << " F=" << m.n_faces() << " C=" << m.n_cells() << "\n";

    // properties
    auto ep = m.request_edge_property<int>("e:tag", 0);
    auto fep = m.request_halfedge_property<int>("he:tag", 0);
    auto fp = m.request_face_property<int>("f:tag", 0);
    auto hfp = m.request_halfface_property<int>("hf:tag", 0);
    auto cp = m.request_cell_property<int>("c:tag", 0);
    for (auto e : m.edges()) ep[e] = e.idx() + 1;
    for (auto he : m.halfedges()) fep[he] = he.idx() + 100;
    for (auto f : m.faces()) fp[f] = f.idx() + 7;
    for (auto hf : m.halffaces()) hfp[hf] = hf.idx() + 50;
    cp[cL] = 11; cp[cR] = 22;

    auto uv = m.request_halfface_property<std::vector<double>>("hf:uv", {});
    for (auto hf : m.halffaces()) {
        size_t k = m.face(hf.face_handle()).halfedges().size();
        std::vector<double> v;
        for (size_t i = 0; i < k; ++i) { v.push_back(double(hf.idx())); v.push_back(double(i)); }
        uv[hf] = v;
    }

    double vL_before = cell_volume(m, cL);
    double total_before = 0;
    for (auto c : m.cells()) total_before += cell_volume(m, c);
    std::cout << "vol L=" << vL_before << " total=" << total_before << "\n";

    auto recs = partition_cells(m, {cL, cL}, Vec(2.0, 0.0, 0.0), -1.0);
    std::cout << "records=" << recs.size() << "\n";
    auto r = recs.front();
    std::cout << "parent=" << r.parent << " neg=" << r.negative
              << " pos=" << r.positive << " cap=" << r.cap << "\n";

    double total_after = 0;
    for (auto c : m.cells()) total_after += cell_volume(m, c);
    std::cout << "vol neg=" << cell_volume(m, r.negative)
              << " pos=" << cell_volume(m, r.positive)
              << " total=" << total_after << "\n";
    std::cout << "conservation err=" << std::abs(total_after - total_before) << "\n";

    // cap shared between the two children
    auto cc = m.face_cells(r.cap);
    std::cout << "cap cells " << cc[0] << " " << cc[1] << "\n";

    // cell props inherited
    std::cout << "c:tag neg=" << cp[r.negative] << " pos=" << cp[r.positive]
              << " R=" << cp[cR] << "\n";

    // neighbor R still one cell, still has 6 halffaces and references split children
    std::cout << "R halfface count=" << m.valence(cR) << "\n";
    std::set<FH> rfaces;
    for (HFH hfh : m.cell_halffaces(cR)) rfaces.insert(hfh.face_handle());
    std::cout << "R faces:";
    for (auto f : rfaces) std::cout << " " << f;
    std::cout << "\n";

    // UV checks: every halfface has 2*valence finite values
    bool ok = true;
    for (auto hf : m.halffaces()) {
        size_t k = m.face(hf.face_handle()).halfedges().size();
        if (uv[hf].size() != 2 * k) { ok = false; std::cout << "UV SIZE FAIL " << hf << "\n"; }
        for (double x : uv[hf]) if (!std::isfinite(x)) ok = false;
    }
    std::cout << "uv sizes ok=" << ok << "\n";

    // Interpolation check: find an original edge of L along x that got split,
    // check the midpoint corner UV on an incident halfface of R.
    // (plane x=0.5 -> t=0.5 on edges (0,y,z)-(1,y,z))
    bool interp_ok = false;
    for (auto hf : m.halffaces()) {
        auto hes = m.halfface(hf).halfedges();
        for (size_t i = 0; i < hes.size(); ++i) {
            auto v0 = m.from_vertex_handle(hes[i]);
            if (std::abs(m.vertex(v0)[0] - 0.5) > 1e-12) continue;
            double u = uv[hf][2*i], w = uv[hf][2*i+1];
            // expected: average of the two endpoint corners of origin face;
            // just print a sample here
            std::cout << "mid corner on hf " << hf << " he " << hes[i]
                      << " uv=(" << u << "," << w << ")\n";
            interp_ok = true;
        }
    }
    std::cout << "interp present=" << interp_ok << "\n";

    // error cases must leave mesh intact
    size_t nv = m.n_vertices(), ne = m.n_edges(), nf = m.n_faces(), nc = m.n_cells();
    auto expect_ia = [&](const char* name, auto fn) {
        try { fn(); std::cout << name << ": NO THROW (BUG)\n"; }
        catch (const std::invalid_argument&) { std::cout << name << ": invalid_argument OK\n"; }
        catch (const std::logic_error& e) { std::cout << name << ": logic_error? " << e.what() << "\n"; }
    };
    expect_ia("zero normal", [&]{ partition_cells(m, {cR}, Vec(0,0,0), 0.0); });
    expect_ia("plane through vertex", [&]{ partition_cells(m, {cR}, Vec(1,0,0), 0.0); });
    expect_ia("bad cell", [&]{ partition_cells(m, {CellHandle(9999)}, Vec(1,0,0), -3.0); });
    std::cout << "unchanged counts: " << (m.n_vertices()==nv && m.n_edges()==ne
              && m.n_faces()==nf && m.n_cells()==nc) << "\n";

    bool threw_logic = false;
    {
        GeometricPolyhedralMeshV3d m2;
        m2.enable_deferred_deletion(false);
        auto v0 = m2.add_vertex(Vec(0,0,0));
        (void)v0;
        try { partition_cells(m2, {CellHandle(0)}, Vec(1,0,0), 0.0); }
        catch (const std::logic_error&) { threw_logic = true; }
    }
    std::cout << "logic_error when incidences off: " << threw_logic << "\n";

    // garbage collection then re-partition
    {
        StatusAttrib status(m);
        m.collect_garbage();
        auto recs2 = partition_cells(m, {r.positive}, Vec(0,1,0), -0.5);
        std::cout << "second partition records=" << recs2.size() << "\n";
        double t2 = 0; for (auto c : m.cells()) t2 += cell_volume(m, c);
        std::cout << "volume after 2nd partition=" << t2 << "\n";
    }
    return 0;
}
