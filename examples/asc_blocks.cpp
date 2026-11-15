// David Eberly, Geometric Tools, Redmond WA 98052
// Copyright (c) 1998-2026
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt
// https://www.geometrictools.com/License/Boost/LICENSE_1_0.txt
// File Version: 8.0.2026.11.16

// Multi-block adaptive skeleton climbing example.
//
// The example builds a synthetic 2x2x2 arrangement of blocks that share a
// corner, assigns a different depth to each block, and writes the extracted
// surface as a Wavefront OBJ file.  The block that produced a triangle is
// recorded in an OBJ comment group.  The first command-line argument is the
// output path; it is required.
//
//   asc_blocks output.obj
//
// The sample field is a sphere centred at the corner shared by all eight
// blocks, so the surface crosses every shared face and the shared corner.
// Each block is sampled from the same analytic function, so the blocks agree
// sample for sample on their shared features.

#include <Mathematics/ASCBlockMesh.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    // A smooth, globally defined test field.  The surface is a sphere.
    int32_t Sample(double x, double y, double z)
    {
        double dx = x - 4.0;
        double dy = y - 4.0;
        double dz = z - 4.0;
        double value = 100.0 * (dx * dx + dy * dy + dz * dz - 6.0);
        return static_cast<int32_t>(std::llround(value));
    }

    std::vector<gte::ASCBlockMeshBlock> BuildBlocks(int32_t N)
    {
        int32_t const size = 1 << N;
        size_t const samplesPerBlock =
            static_cast<size_t>(size + 1) * static_cast<size_t>(size + 1) *
            static_cast<size_t>(size + 1);

        std::vector<gte::ASCBlockMeshBlock> blocks;
        // Eight blocks meeting at the corner (size, size, size), each with a
        // different depth so the shared features have to be negotiated.
        int32_t depth = 0;
        for (int32_t z = 0; z < 2; ++z)
        {
            for (int32_t y = 0; y < 2; ++y)
            {
                for (int32_t x = 0; x < 2; ++x)
                {
                    gte::ASCBlockMeshBlock block;
                    block.origin = { static_cast<int64_t>(x) * size,
                        static_cast<int64_t>(y) * size,
                        static_cast<int64_t>(z) * size };
                    block.depth = depth;
                    block.samples.resize(samplesPerBlock);
                    for (int32_t lz = 0; lz <= size; ++lz)
                    {
                        for (int32_t ly = 0; ly <= size; ++ly)
                        {
                            for (int32_t lx = 0; lx <= size; ++lx)
                            {
                                size_t index = static_cast<size_t>(lx) +
                                    static_cast<size_t>(size + 1) *
                                    (static_cast<size_t>(ly) +
                                        static_cast<size_t>(size + 1) *
                                        static_cast<size_t>(lz));
                                block.samples[index] = Sample(
                                    static_cast<double>(lx) + block.origin[0],
                                    static_cast<double>(ly) + block.origin[1],
                                    static_cast<double>(lz) + block.origin[2]);
                            }
                        }
                    }
                    blocks.push_back(std::move(block));
                    depth = (depth + 1) % (N + 1);
                }
            }
        }
        return blocks;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: asc_blocks output.obj\n";
        return EXIT_FAILURE;
    }

    try
    {
        int32_t const N = 2;
        double const level = 0.5;
        std::vector<gte::ASCBlockMeshBlock> blocks = BuildBlocks(N);
        gte::ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, level,
            blocks);

        std::ofstream out(argv[1]);
        if (!out)
        {
            std::cerr << "cannot open " << argv[1] << " for writing\n";
            return EXIT_FAILURE;
        }

        out << "# multi-block adaptive skeleton climbing\n";
        out << "# blocks: " << blocks.size() << "\n";
        out << "# vertices: " << mesh.vertices.size() << "\n";
        out << "# triangles: " << mesh.triangles.size() << "\n";
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            out << "# block " << i << ": origin " << blocks[i].origin[0]
                << " " << blocks[i].origin[1] << " " << blocks[i].origin[2]
                << ", depth " << blocks[i].depth << "\n";
        }

        for (auto const& vertex : mesh.vertices)
        {
            out << "v " << vertex[0] << " " << vertex[1] << " " << vertex[2]
                << "\n";
        }

        // Group the triangles by the block that produced them.  OBJ indices
        // are one-based.
        std::map<int32_t, std::vector<size_t>> byBlock;
        for (size_t t = 0; t < mesh.triangles.size(); ++t)
        {
            byBlock[mesh.triangleBlock[t]].push_back(t);
        }
        for (auto const& group : byBlock)
        {
            int32_t block = group.first;
            auto const& origin = mesh.blockOrigins[block];
            out << "g block_" << block << "\n";
            out << "# source origin " << origin[0] << " " << origin[1] << " "
                << origin[2] << "\n";
            for (size_t t : group.second)
            {
                auto const& triangle = mesh.triangles[t];
                out << "f " << triangle[0] + 1 << " " << triangle[1] + 1
                    << " " << triangle[2] + 1 << "\n";
            }
        }

        out.flush();
        if (!out)
        {
            std::cerr << "error while writing " << argv[1] << "\n";
            return EXIT_FAILURE;
        }
        std::cout << "wrote " << mesh.vertices.size() << " vertices and "
            << mesh.triangles.size() << " triangles to " << argv[1] << "\n";
    }
    catch (std::exception const& exception)
    {
        std::cerr << "error: " << exception.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
