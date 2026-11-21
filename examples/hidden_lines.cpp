//
//  SoftwareRenderer - hidden-line example
//
//  Builds one mechanical-part scene (a bent bracket with a crease, two
//  overlapping boxes that occlude it, one of them crossing the near
//  plane), prepares it once, then writes:
//    <dir>/faces.ppm - the scene rasterized through the prepared mesh
//    <dir>/lines.svg - the visible edges of the same prepared mesh
//
//  Usage: hidden_lines <output-directory>
//

#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "sr.hpp"

namespace
{
    constexpr std::size_t imageWidth = 640;
    constexpr std::size_t imageHeight = 480;

    sr::VertexShaderOutput exampleVertexShader(const sr::Matrix<float, 4>& m, const sr::Vertex& v)
    {
        sr::VertexShaderOutput o{};
        o.position = m * v.position;
        o.color = v.color;
        o.texCoords[0] = v.texCoords[0];
        o.texCoords[1] = v.texCoords[1];
        o.normal = v.normal;
        return o;
    }

    sr::Color exampleFragmentShader(const sr::VertexShaderOutput& v,
                                    const std::array<const sr::Sampler*, 2>&,
                                    const std::array<const sr::Texture*, 2>&)
    {
        auto normal = v.normal;
        normal.normalize();
        const sr::Vector<float, 3> light{0.3F, -0.6F, -0.75F};
        const auto direction = light.normalized();
        const float diffuse = 0.25F + 0.75F * std::fabs(normal.dot(direction));
        return sr::Color{v.color.r * diffuse, v.color.g * diffuse, v.color.b * diffuse, 1.0F};
    }

    struct Scene
    {
        std::vector<sr::Vertex> vertices;
        std::vector<std::size_t> indices;
        std::vector<sr::Vector<float, 3>> viewPositions;
    };

    void addVertex(Scene& scene, float x, float y, float z, const sr::Color& color)
    {
        scene.vertices.push_back(sr::Vertex{sr::Vector<float, 4>{x, y, z, 1.0F}, color,
                                            sr::Vector<float, 2>{0.0F, 0.0F},
                                            sr::Vector<float, 3>{0.0F, 0.0F, 0.0F}});
        scene.viewPositions.push_back(sr::Vector<float, 3>{x, y, z});
    }

    // an L-shaped bracket: two plates joined along a bend (a crease edge),
    // open everywhere else (boundary edges)
    void addBracket(Scene& scene, const sr::Color& color)
    {
        const std::size_t b0 = scene.vertices.size();
        addVertex(scene, -3.0F, 0.2F, 6.0F, color); // bend edge
        const std::size_t b1 = scene.vertices.size();
        addVertex(scene, 3.0F, 0.2F, 6.0F, color);
        const std::size_t a0 = scene.vertices.size();
        addVertex(scene, -3.0F, 0.2F, 8.2F, color); // horizontal arm
        const std::size_t a1 = scene.vertices.size();
        addVertex(scene, 3.0F, 0.2F, 8.2F, color);
        const std::size_t c0 = scene.vertices.size();
        addVertex(scene, -3.0F, 2.0F, 6.0F, color); // vertical arm
        const std::size_t c1 = scene.vertices.size();
        addVertex(scene, 3.0F, 2.0F, 6.0F, color);

        for (const std::size_t i : {b0, b1, a1, b0, a1, a0, b1, b0, c0, b1, c0, c1})
            scene.indices.push_back(i);
    }

    void addBox(Scene& scene,
                const float x0, const float y0, const float z0,
                const float x1, const float y1, const float z1,
                const sr::Color& color)
    {
        const std::size_t base = scene.vertices.size();
        addVertex(scene, x0, y0, z0, color); // 0
        addVertex(scene, x1, y0, z0, color); // 1
        addVertex(scene, x1, y1, z0, color); // 2
        addVertex(scene, x0, y1, z0, color); // 3
        addVertex(scene, x0, y0, z1, color); // 4
        addVertex(scene, x1, y0, z1, color); // 5
        addVertex(scene, x1, y1, z1, color); // 6
        addVertex(scene, x0, y1, z1, color); // 7
        for (const std::size_t i : {0u, 2u, 1u, 0u, 3u, 2u, // back
                                    4u, 5u, 6u, 4u, 6u, 7u, // front
                                    0u, 7u, 3u, 0u, 4u, 7u, // left
                                    1u, 2u, 6u, 1u, 6u, 5u, // right
                                    0u, 1u, 5u, 0u, 5u, 4u, // top
                                    3u, 7u, 6u, 3u, 6u, 2u}) // bottom
            scene.indices.push_back(base + i);
    }

    // area-weighted vertex normals for the lambert term
    void computeNormals(Scene& scene)
    {
        for (std::size_t i = 0; i + 2 < scene.indices.size(); i += 3)
        {
            const auto& p0 = scene.vertices[scene.indices[i + 0]].position;
            const auto& p1 = scene.vertices[scene.indices[i + 1]].position;
            const auto& p2 = scene.vertices[scene.indices[i + 2]].position;
            const sr::Vector<float, 3> u{p1.v[0] - p0.v[0], p1.v[1] - p0.v[1], p1.v[2] - p0.v[2]};
            const sr::Vector<float, 3> v{p2.v[0] - p0.v[0], p2.v[1] - p0.v[1], p2.v[2] - p0.v[2]};
            const auto normal = u.cross(v);
            for (const std::size_t k : {i, i + 1, i + 2})
                scene.vertices[scene.indices[k]].normal += normal;
        }
        for (auto& vertex : scene.vertices)
            vertex.normal.normalize();
    }

    bool writePpm(const std::string& path, const sr::Texture& frameBuffer)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        out << "P6\n" << frameBuffer.getWidth() << " " << frameBuffer.getHeight() << "\n255\n";
        const auto& data = frameBuffer.getData();
        for (std::size_t i = 0; i < frameBuffer.getWidth() * frameBuffer.getHeight(); ++i)
            out.put(static_cast<char>(data[i * 4 + 0]))
               .put(static_cast<char>(data[i * 4 + 1]))
               .put(static_cast<char>(data[i * 4 + 2]));
        return static_cast<bool>(out);
    }

    bool writeSvg(const std::string& path, const std::vector<sr::VisibleEdgeSegment>& segments,
                  const sr::Rect<float>& viewport)
    {
        std::ofstream out(path);
        if (!out) return false;
        out.precision(3);
        out << std::fixed;
        out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << viewport.size.v[0]
            << "\" height=\"" << viewport.size.v[1] << "\" viewBox=\"" << viewport.position.v[0]
            << " " << viewport.position.v[1] << " " << viewport.size.v[0] << " "
            << viewport.size.v[1] << "\">\n";
        out << "<rect x=\"" << viewport.position.v[0] << "\" y=\"" << viewport.position.v[1]
            << "\" width=\"" << viewport.size.v[0] << "\" height=\"" << viewport.size.v[1]
            << "\" fill=\"white\"/>\n";
        out << "<g stroke=\"black\" stroke-width=\"1\" stroke-linecap=\"round\">\n";
        for (const auto& segment : segments)
            out << "<line x1=\"" << segment.p0.v[0] << "\" y1=\"" << segment.p0.v[1]
                << "\" x2=\"" << segment.p1.v[0] << "\" y2=\"" << segment.p1.v[1] << "\"/>\n";
        out << "</g>\n</svg>\n";
        return static_cast<bool>(out);
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0] << " <output-directory>\n";
        return 1;
    }
    const std::string directory = argv[1];

    // the scene: a bent bracket plus two overlapping boxes in front of it;
    // the nearer box crosses the near plane
    Scene scene;
    addBracket(scene, sr::Color{0.75F, 0.75F, 0.80F, 1.0F});
    addBox(scene, -2.0F, -0.4F, 0.6F, -0.8F, 1.2F, 2.6F, sr::Color{0.85F, 0.55F, 0.30F, 1.0F});
    addBox(scene, -1.8F, -1.0F, 2.4F, -0.2F, 0.6F, 3.8F, sr::Color{0.35F, 0.60F, 0.85F, 1.0F});
    computeNormals(scene);

    sr::Matrix<float, 4> projection;
    projection.setPerspective(sr::pi<float> / 3.0F,
                              static_cast<float>(imageWidth) / static_cast<float>(imageHeight),
                              1.0F, 30.0F);

    // prepare once, reuse for faces and lines
    const sr::PreparedMesh mesh(exampleVertexShader, projection, scene.vertices, scene.indices);

    // faces
    sr::Texture frameBuffer(sr::PixelFormat::rgba8, imageWidth, imageHeight);
    sr::Texture depthBuffer(sr::PixelFormat::float32, imageWidth, imageHeight);
    sr::clear(frameBuffer, sr::Color{0.07F, 0.08F, 0.10F, 1.0F});
    sr::clear(depthBuffer, 1.0F);
    const std::array<const sr::Sampler*, 2> samplers{};
    const std::array<const sr::Texture*, 2> textures{};
    const sr::Rect<float> viewport{0.0F, 0.0F, static_cast<float>(imageWidth),
                                   static_cast<float>(imageHeight)};
    sr::DepthState depthState;
    depthState.read = true;
    depthState.write = true;
    sr::drawTriangles(frameBuffer, depthBuffer, exampleFragmentShader, samplers, textures,
                      viewport, sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                      sr::BlendState{}, depthState, mesh);

    // lines: the same prepared mesh, exported with the same viewport
    const auto segments = sr::visibleEdges(mesh, scene.viewPositions, viewport, 0.5F);

    const std::string ppmPath = directory + "/faces.ppm";
    const std::string svgPath = directory + "/lines.svg";
    if (!writePpm(ppmPath, frameBuffer))
    {
        std::cerr << "failed to write " << ppmPath << "\n";
        return 1;
    }
    if (!writeSvg(svgPath, segments, viewport))
    {
        std::cerr << "failed to write " << svgPath << "\n";
        return 1;
    }

    std::cout << "wrote " << ppmPath << " and " << svgPath
              << " (" << segments.size() << " visible edge segments)\n";
    return 0;
}
