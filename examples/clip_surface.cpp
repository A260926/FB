//
// clip_surface.cpp
//
// Example for OpenSubdiv::Bfr::ClipRegularSurfaceMesh().
//
// Builds a gently curved regular quad grid whose face-varying UVs have a
// seam, clips a block of interior limit-surface faces against a directed
// plane, and writes the resulting textured triangle mesh as OBJ (v/vt/vn/f).
//
// Usage:
//     clip_surface <output.obj>
//

#include <opensubdiv/bfr/clipSurface.h>
#include <opensubdiv/far/topologyDescriptor.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace OpenSubdiv;

int
main(int argc, char ** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output.obj>\n", argv[0]);
        return 2;
    }
    std::string outPath = argv[1];

    // ------------------------------------------------------------------
    // Curved 10x10 vertex quad grid.  Interior faces are regular bicubic
    // Catmull-Clark patches.
    // ------------------------------------------------------------------
    int const N = 10;
    std::vector<double> positions;
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            double X = x - (N - 1) * 0.5;
            double Y = y - (N - 1) * 0.5;
            double z = 0.10 * (X * X - Y * Y) + 0.15 * std::sin(0.8 * X);
            positions.insert(positions.end(), {X, Y, z});
        }
    }

    std::vector<int> faceSizes, faceVerts;
    for (int y = 0; y < N - 1; ++y) {
        for (int x = 0; x < N - 1; ++x) {
            faceSizes.push_back(4);
            faceVerts.insert(faceVerts.end(),
                             {y * N + x, y * N + x + 1,
                              (y + 1) * N + x + 1, (y + 1) * N + x});
        }
    }

    // One independent face-varying UV value per face corner: this creates UV
    // seams along every coarse edge, so shared positions carry distinct UVs.
    std::vector<double> uvs;
    std::vector<int> uvIndices;
    static double const U[4] = {0.0, 1.0, 1.0, 0.0};
    static double const VV[4] = {0.0, 0.0, 1.0, 1.0};
    int nFaces = (N - 1) * (N - 1);
    for (int f = 0; f < nFaces; ++f) {
        int gx = f % (N - 1), gy = f / (N - 1);
        for (int k = 0; k < 4; ++k) {
            uvIndices.push_back((int) uvs.size() / 2);
            uvs.push_back(gx * 0.25 + U[k]);
            uvs.push_back(gy * 0.25 + VV[k] + 0.13 * k);
        }
    }

    Far::TopologyDescriptor desc;
    desc.numVertices = N * N;
    desc.numFaces = nFaces;
    desc.numVertsPerFace = faceSizes.data();
    desc.vertIndicesPerFace = faceVerts.data();
    Far::TopologyDescriptor::FVarChannel channel;
    channel.numValues = (int) uvs.size() / 2;
    channel.valueIndices = uvIndices.data();
    desc.numFVarChannels = 1;
    desc.fvarChannels = &channel;

    Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options options(
        Sdc::SCHEME_CATMARK);
    options.schemeOptions.SetVtxBoundaryInterpolation(
        Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
    std::unique_ptr<Far::TopologyRefiner> topology(
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
            desc, options));
    if (!topology) {
        std::fprintf(stderr, "failed to build topology\n");
        return 1;
    }

    // Select a symmetric block of interior regular faces.
    std::vector<int> selected;
    for (int y = 3; y <= 6; ++y)
        for (int x = 3; x <= 6; ++x)
            selected.push_back(y * (N - 1) + x);

    Bfr::ClipPlane plane{{0.25, -0.35, 1.0}, 0.15};
    Bfr::ClipOptions clipOptions{1e-3, 1e-8, 18};

    Bfr::ClippedMesh mesh;
    std::string error;
    if (!Bfr::ClipRegularSurfaceMesh(*topology, positions, uvs, selected,
                                     plane, clipOptions, mesh, error)) {
        std::fprintf(stderr, "clip failed: %s\n", error.c_str());
        return 1;
    }
    if (mesh.positions.empty() || mesh.triangles.empty() ||
        mesh.corners.size() != 3 * mesh.triangles.size()) {
        std::fprintf(stderr, "clip produced an empty/invalid mesh\n");
        return 1;
    }

    std::ofstream out(outPath);
    if (!out) {
        std::fprintf(stderr, "cannot open %s for write\n", outPath.c_str());
        return 1;
    }

    out << "# OpenSubdiv Bfr clipped Catmull-Clark limit surface\n";
    for (auto const & p : mesh.positions)
        out << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';

    // One vt and one vn per triangle corner, both indexed in corner order.
    for (auto const & c : mesh.corners)
        out << "vt " << c.uv[0] << ' ' << c.uv[1] << '\n';
    for (auto const & c : mesh.corners)
        out << "vn " << c.normal[0] << ' ' << c.normal[1] << ' '
            << c.normal[2] << '\n';

    for (size_t t = 0; t < mesh.triangles.size(); ++t) {
        out << "f";
        for (int k = 0; k < 3; ++k) {
            size_t corner = 3 * t + k;
            int posIndex = (int) mesh.triangles[t][k] + 1;
            int attrIndex = (int) corner + 1;
            out << ' ' << posIndex << '/' << attrIndex << '/' << attrIndex;
        }
        out << '\n';
    }

    std::printf("wrote %s: %zu v, %zu vt/vn, %zu f\n", outPath.c_str(),
                mesh.positions.size(), mesh.corners.size(),
                mesh.triangles.size());
    return 0;
}
