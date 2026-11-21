// Plane partition example: build two hexahedra sharing a face, split the
// left one with a plane, write the resulting mesh to an OVM file given on
// the command line and read it back for verification.
//
// Usage: plane_partition <output.ovm>

#include <OpenVolumeMesh/Unstable/Topology/PlanePartition.hh>
#include <OpenVolumeMesh/FileManager/FileManager.hh>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace OpenVolumeMesh;
using Vec = Geometry::Vec3d;

namespace {

// Face vertex loops of a hexahedron; the halfface pointing into the cell
// is chosen geometrically, so winding of these loops is not critical.
const int kHexFaces[6][4] = {
    {3, 2, 1, 0}, {7, 6, 5, 4}, {1, 2, 6, 7},
    {4, 5, 3, 0}, {1, 7, 4, 0}, {2, 3, 5, 6},
};

HalfFaceHandle inward_halfface(GeometricPolyhedralMeshV3d& _mesh,
                               FaceHandle _fh, const Vec& _center) {
    const auto vs = _mesh.get_halfface_vertices(_fh.h0());
    Vec face_center(0.0, 0.0, 0.0);
    for (VertexHandle vh : vs) {
        face_center += _mesh.vertex(vh);
    }
    face_center *= 1.0 / static_cast<double>(vs.size());
    const Vec winding =
        (_mesh.vertex(vs[1]) - _mesh.vertex(vs[0]))
            .cross(_mesh.vertex(vs[2]) - _mesh.vertex(vs[1]));
    return winding.dot(_center - face_center) > 0.0 ? _fh.h0() : _fh.h1();
}

CellHandle add_hexahedron(GeometricPolyhedralMeshV3d& _mesh,
                          const std::vector<VertexHandle>& _v) {
    Vec center(0.0, 0.0, 0.0);
    for (VertexHandle vh : _v) {
        center += _mesh.vertex(vh);
    }
    center *= 0.125;

    std::vector<HalfFaceHandle> halffaces;
    halffaces.reserve(6);
    for (const auto& f : kHexFaces) {
        const std::vector<VertexHandle> loop{
            _v[f[0]], _v[f[1]], _v[f[2]], _v[f[3]]};
        const HalfFaceHandle found = _mesh.find_halfface(loop);
        const FaceHandle fh =
            found.is_valid() ? found.face_handle() : _mesh.add_face(loop);
        halffaces.push_back(inward_halfface(_mesh, fh, center));
    }
    return _mesh.add_cell(halffaces);
}

double polyhedron_volume(GeometricPolyhedralMeshV3d& _mesh, CellHandle _ch) {
    double volume = 0.0;
    for (HalfFaceHandle hfh : _mesh.cell_halffaces(_ch)) {
        const auto vs = _mesh.get_halfface_vertices(hfh);
        for (size_t i = 1; i + 1 < vs.size(); ++i) {
            const Vec& a = _mesh.vertex(vs[0]);
            const Vec& b = _mesh.vertex(vs[i]);
            const Vec& c = _mesh.vertex(vs[i + 1]);
            volume -= a.dot(b.cross(c)) / 6.0; // inward winding
        }
    }
    return volume;
}

} // namespace

int main(int _argc, char** _argv) {
    if (_argc != 2) {
        std::cerr << "usage: " << _argv[0] << " <output.ovm>\n";
        return 2;
    }
    const std::string output_path = _argv[1];

    GeometricPolyhedralMeshV3d mesh;
    mesh.enable_bottom_up_incidences(true);
    mesh.enable_deferred_deletion(true);

    // 12 vertices of two unit cubes [0,1]^3 and [1,2]x[0,1]x[0,1].
    for (int x = 0; x <= 2; ++x) {
        for (int y = 0; y <= 1; ++y) {
            for (int z = 0; z <= 1; ++z) {
                mesh.add_vertex(Vec(static_cast<double>(x),
                                    static_cast<double>(y),
                                    static_cast<double>(z)));
            }
        }
    }
    auto vidx = [](int x, int y, int z) {
        return VertexHandle(x * 4 + y * 2 + z);
    };
    std::vector<VertexHandle> left(8), right(8);
    for (int i = 0; i < 8; ++i) {
        const int x = (i == 1 || i == 2 || i == 6 || i == 7) ? 1 : 0;
        const int y = (i == 2 || i == 3 || i == 5 || i == 6) ? 1 : 0;
        const int z = (i >= 4) ? 1 : 0;
        left[i] = vidx(x, y, z);
        right[i] = vidx(x + 1, y, z);
    }

    const CellHandle left_cell = add_hexahedron(mesh, left);
    const CellHandle right_cell = add_hexahedron(mesh, right);

    auto material = mesh.request_cell_property<int>("c:material", 0);
    material[left_cell] = 7;
    material[right_cell] = 9;
    mesh.set_persistent(material);

    const double volume_before = polyhedron_volume(mesh, left_cell);

    // Plane x = 0.5, i.e. normal (1,0,0), offset -0.5 (non-unit normal on
    // purpose to exercise normalization).
    const std::vector<CellPartition> records =
        partition_cells(mesh, {left_cell}, Vec(3.0, 0.0, 0.0), -1.5);
    if (records.size() != 1) {
        throw std::runtime_error("expected exactly one partition record");
    }
    const CellPartition& record = records.front();
    if (record.negative != left_cell) {
        throw std::runtime_error("negative cell must keep the parent handle");
    }
    if (mesh.face_cells(record.cap)[0] != record.positive ||
        mesh.face_cells(record.cap)[1] != record.negative) {
        throw std::runtime_error("cap face is not shared by the two children");
    }

    const double volume_neg = polyhedron_volume(mesh, record.negative);
    const double volume_pos = polyhedron_volume(mesh, record.positive);
    if (std::abs(volume_neg - 0.5 * volume_before) > 1e-10 ||
        std::abs(volume_pos - 0.5 * volume_before) > 1e-10) {
        throw std::runtime_error("partition produced wrong child volumes");
    }

    IO::FileManager io;
    if (!io.writeFile(output_path, mesh)) {
        throw std::runtime_error("failed to write OVM file");
    }

    GeometricPolyhedralMeshV3d reread;
    if (!io.readFile(output_path, reread)) {
        throw std::runtime_error("failed to read OVM file back");
    }
    auto material2 = reread.request_cell_property<int>("c:material", 0);
    if (reread.n_cells() != 3 || reread.n_logical_cells() != 3) {
        throw std::runtime_error("reread mesh has wrong cell count");
    }
    if (material2[record.negative] != 7 ||
        material2[record.positive] != 7 ||
        material2[right_cell] != 9) {
        throw std::runtime_error("persistent cell property survived incorrectly");
    }

    std::cout << "plane_partition: cells=" << reread.n_cells()
              << " faces=" << reread.n_faces()
              << " vertices=" << reread.n_vertices()
              << " child volumes=(" << volume_neg << "," << volume_pos << ")"
              << " roundtrip=ok\n";
    return 0;
}
