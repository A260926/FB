//
//   Copyright 2021 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

//
//  mesh_consumer: the topology-free half of the production path. It knows
//  nothing about control cages, subdivision or Bfr -- it only reads the
//  delta stream named by stream.list through MeshMirror, then writes an
//  OBJ file. It is compiled against meshDelta.cpp alone.
//

#include <opensubdiv/bfr/meshDelta.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace OpenSubdiv;

namespace {

void writeObj(Bfr::MeshMirror const & mirror, std::string const & objPath) {
    Bfr::MeshSnapshot snapshot = mirror.Snapshot();

    //  1-based OBJ vertex index for every live position id.
    std::map<uint64_t, int> vertexIndex;
    std::ofstream out(objPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::invalid_argument("cannot open OBJ output: " + objPath);
    }
    out.setf(std::ios::scientific);
    out.precision(17);

    out << "# mesh_consumer OBJ export revision "
        << snapshot.revision << "\n";

    //  v records in ascending live position id order.
    int vCount = 0;
    for (Bfr::DeltaPosition const & rec : snapshot.positions) {
        out << "v " << rec.position[0] << ' '
                    << rec.position[1] << ' '
                    << rec.position[2] << '\n';
        vertexIndex[rec.id] = ++vCount;
    }

    //  vt and vn keep per-corner identity: each triangle contributes three
    //  texture and three normal records, and its face line maps every
    //  corner to its own pair plus the shared position vertex.
    int vtCount = 0;
    int vnCount = 0;
    int currentFace = -1;
    for (Bfr::DeltaTriangle const & tri : snapshot.triangles) {
        if (static_cast<int>(tri.face) != currentFace) {
            currentFace = static_cast<int>(tri.face);
            out << "g base_face_" << currentFace << '\n';
        }

        std::array<int, 3> vRef{}, vtRef{}, vnRef{};
        for (int q = 0; q < 3; ++q) {
            auto it = vertexIndex.find(tri.positionIds[q]);
            if (it == vertexIndex.end()) {
                throw std::runtime_error(
                    "OBJ export: triangle references unknown position");
            }
            vRef[q] = it->second;

            out << "vt " << tri.cornerUV[q][0] << ' '
                         << tri.cornerUV[q][1] << '\n';
            vtRef[q] = ++vtCount;

            out << "vn " << tri.cornerNormal[q][0] << ' '
                         << tri.cornerNormal[q][1] << ' '
                         << tri.cornerNormal[q][2] << '\n';
            vnRef[q] = ++vnCount;
        }

        out << "f "
            << vRef[0] << '/' << vtRef[0] << '/' << vnRef[0] << ' '
            << vRef[1] << '/' << vtRef[1] << '/' << vnRef[1] << ' '
            << vRef[2] << '/' << vtRef[2] << '/' << vnRef[2] << '\n';
    }

    if (!out) {
        throw std::runtime_error("failed writing OBJ: " + objPath);
    }
}

} // unnamed namespace

int main(int argc, char ** argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: %s <stream-directory> <output-obj-path>\n",
                     argv[0]);
        return 2;
    }
    std::filesystem::path directory = argv[1];
    std::string objPath = argv[2];

    std::ifstream list(directory / "stream.list", std::ios::binary);
    if (!list) {
        std::fprintf(stderr, "cannot open stream.list in %s\n",
                     directory.string().c_str());
        return 2;
    }

    Bfr::MeshMirror mirror;
    std::string relativeName;
    while (std::getline(list, relativeName)) {
        if (relativeName.empty()) continue;
        std::filesystem::path deltaPath = directory / relativeName;
        mirror.ApplyFile(deltaPath.string());
    }
    if (!list.eof() && list.fail()) {
        std::fprintf(stderr, "error reading stream.list\n");
        return 2;
    }

    try {
        writeObj(mirror, objPath);
    } catch (std::exception const & ex) {
        std::fprintf(stderr, "%s\n", ex.what());
        return 1;
    }

    std::printf("applied stream to revision %llu: %zu vertices, "
                "%zu triangles -> %s\n",
                static_cast<unsigned long long>(mirror.Revision()),
                mirror.NumPositions(), mirror.NumTriangles(),
                objPath.c_str());
    return 0;
}
