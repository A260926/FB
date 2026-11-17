/*===========================================================================*\
 *                                                                           *
 *                            OpenVolumeMesh                                 *
 *        Copyright (C) 2011 by Computer Graphics Group, RWTH Aachen         *
 *                        www.openvolumemesh.org                             *
 *                                                                           *
 *---------------------------------------------------------------------------*
 *  This file is part of OpenVolumeMesh.                                     *
 *                                                                           *
 *  OpenVolumeMesh is free software: you can redistribute it and/or modify   *
 *  it under the terms of the GNU Lesser General Public License as           *
 *  published by the Free Software Foundation, either version 3 of           *
 *  the License, or (at your option) any later version with the              *
 *  following exceptions:                                                    *
 *                                                                           *
 *  If other files instantiate templates or use macros                       *
 *  or inline functions from this file, or you compile this file and         *
 *  link it with other files to produce an executable, this file does        *
 *  not by itself cause the resulting executable to be covered by the        *
 *  GNU Lesser General Public License. This exception does not however       *
 *  invalidate any other reasons why the executable file might be            *
 *  covered by the GNU Lesser General Public License.                        *
 *                                                                           *
 *  OpenVolumeMesh is distributed in the hope that it will be useful,        *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *  GNU Lesser General Public License for more details.                      *
 *                                                                           *
 *  You should have received a copy of the GNU LesserGeneral Public          *
 *  License along with OpenVolumeMesh.  If not,                              *
 *  see <http://www.gnu.org/licenses/>.                                      *
 *                                                                           *
\*===========================================================================*/

/// \file interface_cut.cc
/// \brief Example: cut a two-cell polyhedral mesh along its shared face,
///        write it to an ASCII OVM file, read it back and verify the
///        persistent interface pairing.

#include <OpenVolumeMesh/Unstable/Topology/InterfaceCut.hh>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <OpenVolumeMesh/FileManager/FileManager.hh>
#include <OpenVolumeMesh/Mesh/PolyhedralMesh.hh>

using namespace OpenVolumeMesh;

namespace {

CellHandle add_tetrahedron(GeometricPolyhedralMeshV3d &_mesh,
                           VertexHandle _a, VertexHandle _b,
                           VertexHandle _c, VertexHandle _d)
{
    auto make_face = [&](VertexHandle _x, VertexHandle _y, VertexHandle _z) {
        std::vector<HalfEdgeHandle> halfedges;
        auto make_halfedge = [&](VertexHandle _from, VertexHandle _to) {
            HalfEdgeHandle heh = _mesh.find_halfedge(_from, _to);
            if (!heh.is_valid()) {
                heh = _mesh.add_edge(_from, _to, true).halfedge_handle(0);
            }
            if (_mesh.from_vertex_handle(heh) != _from) {
                heh = heh.opposite_handle();
            }
            return heh;
        };
        halfedges.push_back(make_halfedge(_x, _y));
        halfedges.push_back(make_halfedge(_y, _z));
        halfedges.push_back(make_halfedge(_z, _x));

        HalfFaceHandle hfh = _mesh.find_halfface(halfedges);
        if (hfh.is_valid()) {
            return hfh;
        }
        return _mesh.halfface_handle(_mesh.add_face(halfedges), 0);
    };

    std::vector<HalfFaceHandle> halffaces;
    halffaces.push_back(make_face(_a, _b, _c));
    halffaces.push_back(make_face(_a, _c, _d));
    halffaces.push_back(make_face(_a, _d, _b));
    halffaces.push_back(make_face(_b, _d, _c));
    return _mesh.add_cell(halffaces, true);
}

/// The two tetrahedra share the face (a, b, c); the second cell is built with
/// the opposite orientation of that face.
GeometricPolyhedralMeshV3d make_two_cell_mesh()
{
    GeometricPolyhedralMeshV3d mesh;

    const VertexHandle a = mesh.add_vertex(Vec3d(0.0, 0.0, 0.0));
    const VertexHandle b = mesh.add_vertex(Vec3d(1.0, 0.0, 0.0));
    const VertexHandle c = mesh.add_vertex(Vec3d(0.0, 1.0, 0.0));
    const VertexHandle d = mesh.add_vertex(Vec3d(0.0, 0.0, 1.0));
    const VertexHandle e = mesh.add_vertex(Vec3d(0.0, 0.0, -1.0));

    const CellHandle cell0 = add_tetrahedron(mesh, a, b, c, d);
    const CellHandle cell1 = add_tetrahedron(mesh, a, c, b, e);
    if (!cell0.is_valid() || !cell1.is_valid()) {
        throw std::runtime_error("could not build the example mesh");
    }

    mesh.enable_bottom_up_incidences(true);
    mesh.enable_deferred_deletion(true);
    return mesh;
}

FaceHandle find_interior_face(const GeometricPolyhedralMeshV3d &_mesh)
{
    for (const FaceHandle fh : _mesh.faces()) {
        if (_mesh.incident_cell(_mesh.halfface_handle(fh, 0)).is_valid()
                && _mesh.incident_cell(_mesh.halfface_handle(fh, 1)).is_valid()) {
            return fh;
        }
    }
    return GeometricPolyhedralMeshV3d::InvalidFaceHandle;
}

int fail(const std::string &_message)
{
    std::cerr << "interface_cut: " << _message << std::endl;
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <output.ovm>" << std::endl;
        return EXIT_FAILURE;
    }
    const std::string output_path = argv[1];

    try {
        GeometricPolyhedralMeshV3d mesh = make_two_cell_mesh();

        const FaceHandle shared = find_interior_face(mesh);
        if (!shared.is_valid()) {
            return fail("the example mesh has no interior face");
        }

        const std::size_t cells_before = mesh.n_cells();
        const std::size_t faces_before = mesh.n_faces();

        std::cout << "before: cells=" << cells_before
                  << " faces=" << faces_before
                  << " interior face=" << shared << std::endl;

        cut_interfaces(mesh, std::vector<FaceHandle>{shared});

        const std::vector<InterfacePair> pairs = interface_pairs(mesh);
        if (pairs.size() != 1) {
            return fail("expected exactly one interface pair after the cut");
        }
        if (!mesh.incident_cell(pairs[0].side0).is_valid()
                || !mesh.incident_cell(pairs[0].side1).is_valid()) {
            return fail("an interface side has no incident cell");
        }
        const CellHandle side0_cell = mesh.incident_cell(pairs[0].side0);
        const CellHandle side1_cell = mesh.incident_cell(pairs[0].side1);
        if (!(side0_cell < side1_cell)) {
            return fail("side0 does not belong to the smaller cell");
        }

        std::cout << "after:  cells=" << mesh.n_cells()
                  << " faces=" << mesh.n_faces()
                  << " interface id=" << pairs[0].id << std::endl;

        IO::FileManager file_manager;
        if (!file_manager.writeFile(output_path, mesh)) {
            return fail("could not write " + output_path);
        }

        GeometricPolyhedralMeshV3d read_back;
        if (!file_manager.readFile(output_path, read_back)) {
            return fail("could not read " + output_path);
        }

        const std::vector<InterfacePair> read_pairs = interface_pairs(read_back);
        if (read_pairs.size() != 1 || read_pairs[0].id != pairs[0].id) {
            return fail("interface pairing did not survive the file round trip");
        }
        if (read_back.n_cells() != cells_before
                || read_back.n_faces() != faces_before + 1) {
            return fail("unexpected topology after the file round trip");
        }

        std::cout << "round trip: cells=" << read_back.n_cells()
                  << " faces=" << read_back.n_faces()
                  << " interface id=" << read_pairs[0].id << std::endl;
        std::cout << "OK" << std::endl;
    } catch (const std::exception &e) {
        return fail(e.what());
    }

    return EXIT_SUCCESS;
}
