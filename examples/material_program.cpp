//
//  SoftwareRenderer - material node graph example
//
//  Renders an oblique quad with a distorted, analytically filtered
//  checkerboard mixed with a procedural texture, and writes the result as a
//  binary PPM. Usage: material_program <output.ppm>
//

#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

#include "sr/sr.hpp"

namespace
{
    sr::VertexShaderOutput exampleVertexShader(const sr::Matrix<float, 4>& m, const sr::Vertex& v)
    {
        sr::VertexShaderOutput output{};
        output.position = m * v.position;
        output.color = v.color;
        output.texCoords[0] = v.texCoords[0];
        output.texCoords[1] = v.texCoords[1];
        output.normal = v.normal;
        return output;
    }

    sr::Vertex makeVertex(const float ndcX, const float ndcY, const float w,
                          const float u, const float v)
    {
        sr::Vertex vertex{};
        vertex.position = sr::Vector<float, 4>{ndcX * w, ndcY * w, 0.5F * w, w};
        vertex.color = sr::Color{1.0F, 1.0F, 1.0F, 1.0F};
        vertex.texCoords[0] = sr::Vector<float, 2>{u, v};
        vertex.texCoords[1] = vertex.texCoords[0];
        return vertex;
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "usage: material_program <output.ppm>\n";
        return 1;
    }

    constexpr std::size_t width = 512;
    constexpr std::size_t height = 512;

    // procedural source texture
    sr::Texture texture(sr::PixelFormat::rgba8, 64, 64);
    {
        std::vector<std::uint8_t> data(64 * 64 * 4);
        for (std::size_t y = 0; y < 64; ++y)
            for (std::size_t x = 0; x < 64; ++x)
            {
                auto* px = &data[(y * 64 + x) * 4];
                px[0] = static_cast<std::uint8_t>(40 + x * 3);
                px[1] = static_cast<std::uint8_t>(30 + y * 3);
                px[2] = static_cast<std::uint8_t>(((x / 8 + y / 8) % 2) ? 220 : 70);
                px[3] = 255;
            }
        texture.setData(data);
    }

    sr::Sampler sampler;
    sampler.filter = sr::Sampler::Filter::linear;

    // material: distorted, filtered checkerboard mixed with the texture
    sr::MaterialGraph graph;
    const auto uv = graph.texCoord(0);
    const auto warp = graph.multiply(
        graph.sine(graph.multiply(uv, graph.constant(sr::Vector<float, 2>{2.5F, 2.5F}))),
        graph.constant(sr::Vector<float, 2>{0.35F, 0.35F}));
    const auto distorted = graph.add(uv, warp);
    const auto ck = graph.checker(graph.multiply(distorted, graph.constant(sr::Vector<float, 2>{1.5F, 1.5F})));
    const auto texel = graph.sample(graph.multiply(uv, graph.constant(sr::Vector<float, 2>{0.25F, 0.25F})),
                                    &texture, sampler);
    const auto ck4 = graph.assemble({ck, ck, ck, graph.constant(1.0F)});
    const auto inv = graph.add(
        graph.multiply(ck4, graph.constant(sr::Vector<float, 4>{-1.0F, -1.0F, -1.0F, 0.0F})),
        graph.constant(sr::Vector<float, 4>{1.0F, 1.0F, 1.0F, 1.0F}));
    const auto tint = graph.constant(sr::Vector<float, 4>{0.10F, 0.18F, 0.55F, 1.0F});
    const auto mixed = graph.add(graph.multiply(ck4, texel), graph.multiply(inv, tint));
    const auto root = graph.assemble({graph.component(mixed, 0),
                                      graph.component(mixed, 1),
                                      graph.component(mixed, 2),
                                      graph.constant(1.0F)});
    const auto program = sr::compileMaterialGraph(graph, root);

    // oblique quad receding into the distance (w grows towards the far edge)
    const std::vector<sr::Vertex> vertices{
        makeVertex(-0.95F, -0.75F, 1.0F, 0.0F, 0.0F),
        makeVertex(0.95F, -0.75F, 1.0F, 4.0F, 0.0F),
        makeVertex(0.45F, 0.60F, 2.4F, 4.0F, 4.0F),
        makeVertex(-0.55F, 0.60F, 2.4F, 0.0F, 4.0F)
    };
    const std::vector<std::size_t> indices{0, 1, 2, 0, 2, 3};

    sr::Texture frameBuffer(sr::PixelFormat::rgba8, width, height);
    sr::Texture depthBuffer(sr::PixelFormat::float32, width, height);
    sr::clear(frameBuffer, sr::Color{0.02F, 0.02F, 0.04F, 1.0F});
    sr::clear(depthBuffer, 1.0F);

    sr::Matrix<float, 4> mvp;
    mvp.setIdentity();
    sr::DepthState depthState;
    depthState.read = true;
    depthState.write = true;

    sr::drawTriangles(frameBuffer, depthBuffer, exampleVertexShader, program,
                      sr::Rect<float>{static_cast<float>(width), static_cast<float>(height)},
                      sr::Rect<float>{1.0F, 1.0F},
                      sr::BlendState{}, depthState, indices, vertices, mvp);

    std::ofstream out(argv[1], std::ios::binary);
    if (!out)
    {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 1;
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    const auto& pixels = frameBuffer.getData();
    for (std::size_t i = 0; i < width * height; ++i)
        out.write(reinterpret_cast<const char*>(&pixels[i * 4]), 3);
    out.flush();
    if (!out)
    {
        std::cerr << "failed to write " << argv[1] << "\n";
        return 1;
    }

    std::cout << "wrote " << argv[1] << " (" << width << "x" << height << ", "
              << program.getInstructionCount() << " instructions, "
              << program.getTextureCount() << " texture snapshot)\n";
    return 0;
}
