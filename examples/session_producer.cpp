//
//   Copyright 2021 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

//
//  session_producer: runs a TessellationSession over a self-generated,
//  curved, regular Catmull-Clark cage, chains a script of genuinely local
//  edits, and writes every resulting MeshDelta (plus the initial 0 -> 1
//  bootstrap delta) into the given directory. A stream.list file in that
//  directory names the delta files, relative to the directory, in order.
//

#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/bfr/tessellationSession.h>
#include <opensubdiv/bfr/meshDelta.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace OpenSubdiv;
using Vec3 = std::array<double, 3>;

namespace {

//  A curved torus: a closed, orientable, all-quad regular control cage
//  with no boundary, exercising the periodic case end to end.
struct Cage {
    int rings = 0;
    int segments = 0;
    std::vector<Vec3> positions;
    std::vector<int> faceVerts;
};

Cage makeCurvedTorus(int rings, int segments) {
    Cage cage;
    cage.rings = rings;
    cage.segments = segments;
    const double majorRadius = 2.0;
    const double tubeRadius = 0.6;
    const double pi = std::acos(-1.0);

    for (int j = 0; j < rings; ++j) {
        double v = 2.0 * pi * j / rings;
        for (int i = 0; i < segments; ++i) {
            double u = 2.0 * pi * i / segments;
            double radius = majorRadius + tubeRadius * std::cos(v);
            cage.positions.push_back({
                radius * std::cos(u),
                radius * std::sin(u),
                tubeRadius * std::sin(v) });
        }
    }
    for (int j = 0; j < rings; ++j) {
        for (int i = 0; i < segments; ++i) {
            cage.faceVerts.insert(cage.faceVerts.end(), {
                j * segments + i,
                j * segments + (i + 1) % segments,
                ((j + 1) % rings) * segments + (i + 1) % segments,
                ((j + 1) % rings) * segments + i });
        }
    }
    return cage;
}

std::unique_ptr<Far::TopologyRefiner>
makeRefiner(Cage const & cage) {
    std::vector<int> counts(
        static_cast<size_t>(cage.rings) * cage.segments, 4);
    Far::TopologyDescriptor desc;
    desc.numVertices = cage.rings * cage.segments;
    desc.numFaces = cage.rings * cage.segments;
    desc.numVertsPerFace = counts.data();
    desc.vertIndicesPerFace = cage.faceVerts.data();

    auto options = Far::TopologyRefinerFactory<Far::TopologyDescriptor>::
        Options(Sdc::SCHEME_CATMARK);
    options.schemeOptions.SetVtxBoundaryInterpolation(
        Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
    return std::unique_ptr<Far::TopologyRefiner>(
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
            desc, options));
}

std::string deltaName(int sequence) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "delta_%04d.osdm", sequence);
    return std::string(buffer);
}

} // unnamed namespace

int main(int argc, char ** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 2;
    }
    std::filesystem::path directory = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create directory: %s\n",
                     ec.message().c_str());
        return 2;
    }

    Cage cage = makeCurvedTorus(6, 8);
    std::unique_ptr<Far::TopologyRefiner> refiner = makeRefiner(cage);
    std::vector<int> rates(
        static_cast<size_t>(cage.rings) * cage.segments, 2);

    Bfr::TessellationSession session(*refiner, cage.positions.data(),
                                     cage.positions.size(),
                                     rates.data(), rates.size());

    std::vector<std::string> streamList;
    int sequence = 0;

    auto publish = [&](Bfr::MeshDelta const & delta) {
        std::string name = deltaName(sequence++);
        Bfr::WriteMeshDeltaFile((directory / name).string(), delta);
        streamList.push_back(name);
    };

    //  0 -> 1 bootstrap from the empty mirror.
    publish(session.InitialDelta());

    //  Local control point edit: move one coarse vertex absolutely.
    {
        auto edited = cage.positions;
        edited[0][2] += 0.35;
        publish(session.Update(
            std::vector<Bfr::PositionEdit>{Bfr::PositionEdit{0, edited[0]}},
            {}));
    }

    //  Local density change 2 -> 4 on one base face; neighbor outer rates
    //  propagate without changing their own inner rates.
    {
        const int focusFace = 11;
        publish(session.Update(
            {},
            std::vector<Bfr::RateEdit>{Bfr::RateEdit{focusFace, 4}}));

        //  Return to 2: edge rational stations keep their identity.
        publish(session.Update(
            {},
            std::vector<Bfr::RateEdit>{Bfr::RateEdit{focusFace, 2}}));
    }

    //  Simultaneous point and rate edit in a single transaction.
    {
        auto edited = cage.positions;
        edited[7][0] -= 0.22;
        edited[7][1] += 0.18;
        publish(session.Update(
            std::vector<Bfr::PositionEdit>{Bfr::PositionEdit{7, edited[7]}},
            std::vector<Bfr::RateEdit>{Bfr::RateEdit{20, 3}}));
    }

    //  A second control point edit elsewhere, leaving the rate alone.
    {
        auto edited = cage.positions;
        edited[13][2] += 0.41;
        publish(session.Update(
            std::vector<Bfr::PositionEdit>{
                Bfr::PositionEdit{13, edited[13]}},
            {}));
    }

    //  A no-op transaction: absolute values already current, so the
    //  revision does not advance and an empty, same-revision delta is
    //  written (the mirror accepts it legally).
    {
        auto same = cage.positions;
        same[13][2] += 0.41;  // identical to the value just committed
        publish(session.Update(
            std::vector<Bfr::PositionEdit>{
                Bfr::PositionEdit{13, same[13]}},
            {}));
    }

    std::ofstream list(directory / "stream.list",
                       std::ios::binary | std::ios::trunc);
    if (!list) {
        std::fprintf(stderr, "cannot write stream.list\n");
        return 2;
    }
    for (std::string const & name : streamList) {
        list << name << '\n';
    }
    if (!list) {
        std::fprintf(stderr, "failed writing stream.list\n");
        return 2;
    }

    std::printf("wrote %zu delta(s) to %s at revision %llu\n",
                streamList.size(), directory.string().c_str(),
                static_cast<unsigned long long>(session.Revision()));
    return 0;
}
