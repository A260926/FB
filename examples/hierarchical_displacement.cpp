//
//   Copyright 2024 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

//  Headless example for Far::HierarchicalDisplacement: builds a two-layer
//  sculpted quad cage, writes the resulting mesh to an OBJ file and prints
//  the evaluated limit samples together with the nonzero pullback gradient
//  norms.  Only the C++ standard library and the supplied CPU OpenSubdiv
//  sources are used.

#include "opensubdiv/far/hierarchicalDisplacement.h"
#include "opensubdiv/far/topologyDescriptor.h"
#include "opensubdiv/far/topologyRefinerFactory.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

using namespace OpenSubdiv;

namespace {

std::unique_ptr<Far::TopologyRefiner>
createCubeRefiner() {
    // Asymmetric closed cube cage.
    static int const vertsPerFace[6] = {4, 4, 4, 4, 4, 4};
    static int const faceVerts[24] = {
        0, 3, 2, 1,
        4, 5, 6, 7,
        0, 1, 5, 4,
        1, 2, 6, 5,
        2, 3, 7, 6,
        3, 0, 4, 7
    };
    static double const xyz[8][3] = {
        {-1.0, -1.0, -1.0},
        { 1.0, -1.0, -1.0},
        { 1.0,  1.0, -1.0},
        {-1.0,  1.0, -1.0},
        {-1.0, -1.0,  1.2},
        { 1.0, -1.0,  1.3},
        { 1.0,  1.0,  1.5},
        {-1.0,  1.0,  1.1}
    };
    (void)xyz;

    Far::TopologyDescriptor desc;
    desc.numVertices = 8;
    desc.numFaces = 6;
    desc.numVertsPerFace = vertsPerFace;
    desc.vertIndicesPerFace = faceVerts;

    Sdc::Options options;
    options.SetVtxBoundaryInterpolation(
        Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
    Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options opts(
        Sdc::SCHEME_CATMARK, options);
    return std::unique_ptr<Far::TopologyRefiner>(
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
            desc, opts));
}

} // anonymous namespace

int main(int argc, char ** argv) {
    std::string objPath = (argc > 1) ? argv[1] : "sculpt.obj";

    std::unique_ptr<Far::TopologyRefiner> refiner = createCubeRefiner();

    const int numLayers = 2;
    std::vector<Far::HierarchicalDisplacementSample> samples;
    samples.push_back({0, 0.31, 0.43});
    samples.push_back({2, 0.62, 0.27});
    samples.push_back({5, 0.47, 0.71});

    Far::HierarchicalDisplacement plan(
        *refiner, numLayers, samples);

    std::vector<double> coarseXYZ = {
        -1.0, -1.0, -1.0,
         1.0, -1.0, -1.0,
         1.0,  1.0, -1.0,
        -1.0,  1.0, -1.0,
        -1.0, -1.0,  1.2,
         1.0, -1.0,  1.3,
         1.0,  1.0,  1.5,
        -1.0,  1.0,  1.1
    };

    // Deterministic, sign-changing scalar displacements.
    auto makeDisplacements = [](int count, int salt) {
        std::vector<double> data(count);
        for (int i = 0; i < count; ++i) {
            double v = std::sin(0.7 * (i + 1) + 0.5 * salt) * 0.35;
            if ((i + salt) % 7 == 0) {
                v = 0.0; // zero entries remain legal
            }
            data[i] = v;
        }
        return data;
    };
    std::vector<double> disp1 =
        makeDisplacements(plan.GetNumLayerVertices(1), 1);
    std::vector<double> disp2 =
        makeDisplacements(plan.GetNumLayerVertices(2), 2);
    std::vector<std::vector<double> > displacements = {disp1, disp2};

    Far::HierarchicalDisplacementResult result;
    std::shared_ptr<Far::HierarchicalDisplacement::Tape> tape =
        plan.Forward(coarseXYZ, displacements, result);

    // Write the final sculpted layer as an OBJ mesh.
    std::ofstream obj(objPath);
    if (!obj) {
        std::cerr << "failed to open " << objPath << " for writing\n";
        return 1;
    }
    obj << std::setprecision(10);
    int numFinalVerts = plan.GetNumLayerVertices(numLayers);
    for (int v = 0; v < numFinalVerts; ++v) {
        obj << "v "
            << result.finalLayerVertices[3 * v]     << ' '
            << result.finalLayerVertices[3 * v + 1] << ' '
            << result.finalLayerVertices[3 * v + 2] << '\n';
    }
    int faceSize = plan.GetFinalFaceSize();
    std::vector<int> const & faceIndices =
        plan.GetFinalFaceVertexIndices();
    for (std::size_t f = 0; f < faceIndices.size();
         f += (std::size_t)faceSize) {
        obj << 'f';
        for (int k = 0; k < faceSize; ++k) {
            obj << ' ' << faceIndices[f + k] + 1;
        }
        obj << '\n';
    }
    obj.close();

    std::cout << std::setprecision(8);
    std::cout << "OBJ " << objPath
              << " vertices=" << numFinalVerts
              << " faces=" << plan.GetFinalNumFaces() << '\n';

    for (int i = 0; i < plan.GetNumSamples(); ++i) {
        std::cout << "sample " << i
                  << " position=("
                  << result.samplePositions[3 * i]     << ','
                  << result.samplePositions[3 * i + 1] << ','
                  << result.samplePositions[3 * i + 2] << ") normal=("
                  << result.sampleNormals[3 * i]     << ','
                  << result.sampleNormals[3 * i + 1] << ','
                  << result.sampleNormals[3 * i + 2] << ")\n";
    }

    // An arbitrary mixed position/normal objective, propagated analytically.
    std::vector<double> posAdjoint(3 * plan.GetNumSamples());
    std::vector<double> nrmAdjoint(3 * plan.GetNumSamples());
    for (int i = 0; i < plan.GetNumSamples(); ++i) {
        posAdjoint[3 * i]     = 0.6 + 0.1 * i;
        posAdjoint[3 * i + 1] = -0.4;
        posAdjoint[3 * i + 2] = 0.25;
        nrmAdjoint[3 * i]     = 0.3;
        nrmAdjoint[3 * i + 1] = 0.5 - 0.2 * i;
        nrmAdjoint[3 * i + 2] = -0.7;
    }
    std::vector<double> coarseGrad(3 * plan.GetNumCoarseVertices(), 0.0);
    std::vector<double> grad1(plan.GetNumLayerVertices(1), 0.0);
    std::vector<double> grad2(plan.GetNumLayerVertices(2), 0.0);
    std::vector<std::vector<double> *> dispGradients = {
        &grad1, &grad2
    };
    tape->Pullback(posAdjoint, nrmAdjoint,
                   coarseGrad, dispGradients);

    auto norm = [](std::vector<double> const & v) {
        double s = 0.0;
        for (double x : v) {
            s += x * x;
        }
        return std::sqrt(s);
    };
    std::cout << "gradient_norm coarse_xyz=" << norm(coarseGrad)
              << " displacement_layer1=" << norm(grad1)
              << " displacement_layer2=" << norm(grad2) << '\n';

    int nonzero1 = 0, nonzero2 = 0;
    for (double g : grad1) {
        if (g != 0.0) {
            ++nonzero1;
        }
    }
    for (double g : grad2) {
        if (g != 0.0) {
            ++nonzero2;
        }
    }
    std::cout << "nonzero_gradients layer1=" << nonzero1
              << " layer2=" << nonzero2 << '\n';

    return 0;
}
