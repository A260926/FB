//
//  SoftwareRenderer
//

#ifndef SR_PREPARED_MESH_HPP
#define SR_PREPARED_MESH_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>
#include "BlendState.hpp"
#include "Color.hpp"
#include "Constants.hpp"
#include "DepthState.hpp"
#include "Matrix.hpp"
#include "Rect.hpp"
#include "Sampler.hpp"
#include "Shader.hpp"
#include "Texture.hpp"
#include "Vector.hpp"
#include "Vertex.hpp"

namespace sr
{
    // Reusable mesh preparation result.
    //
    // Preparation runs the vertex shader once per vertex, clips every
    // triangle in homogeneous (clip) space against -w <= x, y <= w and
    // 0 <= z <= w, triangulates the clipped polygons as fans and keeps the
    // original face index on every produced triangle. The object owns all
    // prepared data: after construction the caller may modify or release
    // the inputs and still draw the mesh (drawTriangles overload below) or
    // export its visible edges (see HiddenLines.hpp) any number of times,
    // with any viewport.
    //
    // Construction / prepare() throw std::invalid_argument on illegal
    // indices (count not a multiple of 3, out of range, degenerate
    // triangles), non-manifold edges (more than two faces share an edge)
    // and non-finite numbers (vertex positions, matrix, shader output).
    // prepare() provides the strong guarantee: on failure the previously
    // prepared content is left untouched.
    class PreparedMesh final
    {
    public:
        // Marker for "no adjacent face" in Edge::face0 / Edge::face1.
        static constexpr std::size_t npos = ~std::size_t{0};

        // One triangle of the clipped mesh. Vertex positions are in clip
        // space; the other attributes are linearly interpolated in clip
        // space, so perspective-correct interpolation still works when
        // rasterizing. faceIndex is the original triangle index.
        struct ClippedTriangle final
        {
            std::array<VertexShaderOutput, 3> vertices;
            std::size_t faceIndex = 0;
        };

        // The same clipped geometry in double precision. Hidden-line
        // removal uses these so that faces clipped independently still
        // share bit-identical cut vertices along their common edges and
        // occluding intervals meet exactly.
        struct PreciseClippedTriangle final
        {
            std::array<std::array<double, 4>, 3> positions;
            std::size_t faceIndex = 0;
        };

        // Undirected adjacency edge built from the original indices, in
        // order of first appearance in the index buffer. v0/v1 are original
        // vertex indices oriented as first encountered. face1 == npos marks
        // a boundary edge.
        struct Edge final
        {
            std::size_t v0 = 0;
            std::size_t v1 = 0;
            std::size_t face0 = npos;
            std::size_t face1 = npos;
        };

        PreparedMesh() = default;

        PreparedMesh(VertexShader vertexShader,
                     const Matrix<float, 4>& modelViewProjection,
                     const std::vector<Vertex>& vertices,
                     const std::vector<std::size_t>& indices)
        {
            prepare(vertexShader, modelViewProjection, vertices, indices);
        }

        // Replaces the prepared content. On std::invalid_argument the
        // previous content is preserved.
        void prepare(VertexShader vertexShader,
                     const Matrix<float, 4>& modelViewProjection,
                     const std::vector<Vertex>& vertices,
                     const std::vector<std::size_t>& indices)
        {
            if (!vertexShader)
                throw std::invalid_argument{"Vertex shader must not be null"};

            if (indices.size() % 3 != 0)
                throw std::invalid_argument{"Index count must be a multiple of 3"};

            for (const float c : modelViewProjection.m)
                if (!std::isfinite(c))
                    throw std::invalid_argument{"Model-view-projection matrix must be finite"};

            for (const Vertex& vertex : vertices)
                for (const float c : vertex.position.v)
                    if (!std::isfinite(c))
                        throw std::invalid_argument{"Vertex positions must be finite"};

            for (const std::size_t index : indices)
                if (index >= vertices.size())
                    throw std::invalid_argument{"Vertex index out of range"};

            for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
                if (indices[i] == indices[i + 1] || indices[i + 1] == indices[i + 2] ||
                    indices[i] == indices[i + 2])
                    throw std::invalid_argument{"Degenerate triangle"};

            // vertex shading, once per vertex
            std::vector<VertexShaderOutput> vertexOutputs(vertices.size());
            for (std::size_t i = 0; i < vertices.size(); ++i)
            {
                vertexOutputs[i] = vertexShader(modelViewProjection, vertices[i]);
                if (!isFinite(vertexOutputs[i]))
                    throw std::invalid_argument{"Vertex shader output must be finite"};
            }

            // undirected adjacency from the original indices
            std::vector<Edge> edges;
            std::map<std::pair<std::size_t, std::size_t>, std::size_t> edgeMap;
            for (std::size_t f = 0; f < indices.size() / 3; ++f)
            {
                for (std::size_t k = 0; k < 3; ++k)
                {
                    const std::size_t a = indices[f * 3 + k];
                    const std::size_t b = indices[f * 3 + (k + 1) % 3];
                    const auto key = std::minmax(a, b);
                    const auto it = edgeMap.find(key);
                    if (it == edgeMap.end())
                    {
                        edgeMap.emplace(key, edges.size());
                        edges.push_back(Edge{a, b, f, npos});
                    }
                    else
                    {
                        Edge& edge = edges[it->second];
                        if (edge.face1 != npos)
                            throw std::invalid_argument{"Non-manifold edge"};
                        edge.face1 = f;
                    }
                }
            }

            // homogeneous clipping, original face identity is preserved
            std::vector<ClippedTriangle> triangles;
            std::vector<PreciseClippedTriangle> preciseTriangles;
            for (std::size_t f = 0; f < indices.size() / 3; ++f)
            {
                const std::array<VertexShaderOutput, 3> triangle{
                    vertexOutputs[indices[f * 3 + 0]],
                    vertexOutputs[indices[f * 3 + 1]],
                    vertexOutputs[indices[f * 3 + 2]]
                };
                clipTriangle(triangle, f, triangles, preciseTriangles);
            }

            // commit only after everything succeeded
            indices_ = indices;
            vertexOutputs_ = std::move(vertexOutputs);
            triangles_ = std::move(triangles);
            preciseTriangles_ = std::move(preciseTriangles);
            edges_ = std::move(edges);
            vertexCount_ = vertices.size();
        }

        [[nodiscard]] bool empty() const noexcept { return indices_.empty(); }
        [[nodiscard]] std::size_t getVertexCount() const noexcept { return vertexCount_; }
        [[nodiscard]] const std::vector<std::size_t>& getIndices() const noexcept { return indices_; }
        [[nodiscard]] const std::vector<VertexShaderOutput>& getVertexOutputs() const noexcept { return vertexOutputs_; }
        [[nodiscard]] const std::vector<ClippedTriangle>& getTriangles() const noexcept { return triangles_; }
        [[nodiscard]] const std::vector<PreciseClippedTriangle>& getPreciseTriangles() const noexcept { return preciseTriangles_; }
        [[nodiscard]] const std::vector<Edge>& getEdges() const noexcept { return edges_; }

    private:
        [[nodiscard]] static bool isFinite(const VertexShaderOutput& output) noexcept
        {
            for (const float c : output.position.v)
                if (!std::isfinite(c)) return false;
            if (!std::isfinite(output.color.r) || !std::isfinite(output.color.g) ||
                !std::isfinite(output.color.b) || !std::isfinite(output.color.a))
                return false;
            for (const auto& texCoord : output.texCoords)
                for (const float c : texCoord.v)
                    if (!std::isfinite(c)) return false;
            for (const float c : output.normal.v)
                if (!std::isfinite(c)) return false;
            return true;
        }

        // Signed distance to one of the clip planes -w <= x, y <= w, 0 <= z <= w.
        [[nodiscard]] static double planeDistance(const int plane,
                                                  const std::array<double, 4>& p) noexcept
        {
            switch (plane)
            {
                case 0: return p[0] + p[3]; // x >= -w
                case 1: return p[3] - p[0]; // x <= w
                case 2: return p[1] + p[3]; // y >= -w
                case 3: return p[3] - p[1]; // y <= w
                case 4: return p[2];        // z >= 0
                default: return p[3] - p[2]; // z <= w
            }
        }

        // A vertex of the polygon being clipped: the position is kept in
        // double precision so that independently clipped neighbours share
        // bit-identical cut points; the shading attributes stay in float.
        struct ClipVertex final
        {
            std::array<double, 4> position;
            VertexShaderOutput attribs;
        };

        // Cut of an edge whose endpoints have the given signed distances to
        // the clip plane. The position uses the symmetric form
        // (d0 * b - d1 * a) / (d0 - d1) which is invariant (bit-identical)
        // under reversing the edge direction, so two faces clipped
        // independently produce exactly the same vertex on their shared
        // edge.
        [[nodiscard]] static ClipVertex clipVertexAt(const ClipVertex& a, const double da,
                                                     const ClipVertex& b, const double db) noexcept
        {
            ClipVertex result;
            const double denominator = da - db;
            for (std::size_t k = 0; k < 4; ++k)
                result.position[k] = (da * b.position[k] - db * a.position[k]) / denominator;
            const float t = static_cast<float>(da / denominator);
            result.attribs.position = Vector<float, 4>{
                static_cast<float>(result.position[0]),
                static_cast<float>(result.position[1]),
                static_cast<float>(result.position[2]),
                static_cast<float>(result.position[3])
            };
            result.attribs.color = Color{
                a.attribs.color.r + (b.attribs.color.r - a.attribs.color.r) * t,
                a.attribs.color.g + (b.attribs.color.g - a.attribs.color.g) * t,
                a.attribs.color.b + (b.attribs.color.b - a.attribs.color.b) * t,
                a.attribs.color.a + (b.attribs.color.a - a.attribs.color.a) * t
            };
            for (std::size_t i = 0; i < 2; ++i)
                result.attribs.texCoords[i] = a.attribs.texCoords[i] +
                    (b.attribs.texCoords[i] - a.attribs.texCoords[i]) * t;
            result.attribs.normal = a.attribs.normal + (b.attribs.normal - a.attribs.normal) * t;
            return result;
        }

        // Sutherland-Hodgman against the six clip planes, then fan
        // triangulation. Only original edges of the polygon are kept; the
        // fan diagonals are internal to the clipped face.
        static void clipTriangle(const std::array<VertexShaderOutput, 3>& triangle,
                                 const std::size_t faceIndex,
                                 std::vector<ClippedTriangle>& output,
                                 std::vector<PreciseClippedTriangle>& preciseOutput)
        {
            std::vector<ClipVertex> polygon;
            polygon.reserve(9);
            for (const VertexShaderOutput& vertex : triangle)
            {
                ClipVertex clipVertex;
                for (std::size_t k = 0; k < 4; ++k)
                    clipVertex.position[k] = vertex.position.v[k];
                clipVertex.attribs = vertex;
                polygon.push_back(clipVertex);
            }

            for (int plane = 0; plane < 6 && polygon.size() >= 3; ++plane)
            {
                std::vector<ClipVertex> clipped;
                for (std::size_t i = 0; i < polygon.size(); ++i)
                {
                    const ClipVertex& current = polygon[i];
                    const ClipVertex& previous = polygon[(i + polygon.size() - 1) % polygon.size()];
                    const double currentDistance = planeDistance(plane, current.position);
                    const double previousDistance = planeDistance(plane, previous.position);
                    const bool currentInside = currentDistance >= 0.0;
                    const bool previousInside = previousDistance >= 0.0;

                    if (currentInside)
                    {
                        if (!previousInside)
                            clipped.push_back(clipVertexAt(previous, previousDistance,
                                                           current, currentDistance));
                        clipped.push_back(current);
                    }
                    else if (previousInside)
                        clipped.push_back(clipVertexAt(previous, previousDistance,
                                                       current, currentDistance));
                }
                polygon = std::move(clipped);
            }

            for (std::size_t i = 1; i + 1 < polygon.size(); ++i)
            {
                ClippedTriangle clippedTriangle{{polygon[0].attribs, polygon[i].attribs,
                                                 polygon[i + 1].attribs}, faceIndex};
                PreciseClippedTriangle preciseTriangle{{polygon[0].position, polygon[i].position,
                                                        polygon[i + 1].position}, faceIndex};
                output.push_back(clippedTriangle);
                preciseOutput.push_back(preciseTriangle);
            }
        }

        std::vector<std::size_t> indices_;
        std::vector<VertexShaderOutput> vertexOutputs_;
        std::vector<ClippedTriangle> triangles_;
        std::vector<PreciseClippedTriangle> preciseTriangles_;
        std::vector<Edge> edges_;
        std::size_t vertexCount_ = 0;
    };

    // Face drawing entry for prepared meshes. Uses the existing fragment
    // shader, samplers, textures, depth and blend state. The triangles were
    // clipped during preparation against -w <= x, y <= w, 0 <= z <= w.
    // Projected z/w is interpolated affinely in screen space (no extra
    // perspective weighting for depth); all other attributes are
    // perspective-correct.
    inline void drawTriangles(Texture& frameBuffer,
                              Texture& depthBuffer,
                              FragmentShader fragmentShader,
                              const std::array<const Sampler*, 2>& samplers,
                              const std::array<const Texture*, 2>& textures,
                              const Rect<float>& viewport,
                              const Rect<float>& scissorRect,
                              const BlendState& blendState,
                              const DepthState& depthState,
                              const PreparedMesh& mesh)
    {
        const auto frameBufferData = reinterpret_cast<std::uint32_t*>(frameBuffer.getData().data());
        const auto depthBufferData = reinterpret_cast<float*>(depthBuffer.getData().data());
        const auto width = frameBuffer.getWidth();
        const auto height = frameBuffer.getHeight();

        // scissor rectangle in pixel coordinates
        const auto scissorX0 = static_cast<int>(std::ceil(
            std::clamp(scissorRect.position.v[0], 0.0F, 1.0F) * static_cast<float>(width)));
        const auto scissorY0 = static_cast<int>(std::ceil(
            std::clamp(scissorRect.position.v[1], 0.0F, 1.0F) * static_cast<float>(height)));
        const auto scissorX1 = static_cast<int>(std::ceil(
            std::clamp(scissorRect.position.v[0] + scissorRect.size.v[0], 0.0F, 1.0F) *
            static_cast<float>(width))) - 1;
        const auto scissorY1 = static_cast<int>(std::ceil(
            std::clamp(scissorRect.position.v[1] + scissorRect.size.v[1], 0.0F, 1.0F) *
            static_cast<float>(height))) - 1;

        for (const PreparedMesh::ClippedTriangle& triangle : mesh.getTriangles())
        {
            const auto& tv = triangle.vertices;
            const float clipW0 = tv[0].position.v[3];
            const float clipW1 = tv[1].position.v[3];
            const float clipW2 = tv[2].position.v[3];
            if (clipW0 <= 0.0F || clipW1 <= 0.0F || clipW2 <= 0.0F)
                continue; // degenerate remainder of a clipped triangle

            const std::array<Vector<float, 4>, 3> ndcPositions{
                tv[0].position / clipW0,
                tv[1].position / clipW1,
                tv[2].position / clipW2
            };

            std::array<Vector<float, 2>, 3> viewportPositions;
            bool projectable = true;
            for (std::size_t i = 0; i < 3; ++i)
            {
                viewportPositions[i].v[0] = ndcPositions[i].v[0] * viewport.size.v[0] / 2.0F +
                    viewport.position.v[0] + viewport.size.v[0] / 2.0F;
                viewportPositions[i].v[1] = ndcPositions[i].v[1] * viewport.size.v[1] / 2.0F +
                    viewport.position.v[1] + viewport.size.v[1] / 2.0F;
                if (!std::isfinite(viewportPositions[i].v[0]) || !std::isfinite(viewportPositions[i].v[1]))
                    projectable = false;
            }
            if (!projectable)
                continue;

            Vector<float, 2> screenMin{
                std::min({viewportPositions[0].v[0], viewportPositions[1].v[0], viewportPositions[2].v[0]}),
                std::min({viewportPositions[0].v[1], viewportPositions[1].v[1], viewportPositions[2].v[1]})
            };
            Vector<float, 2> screenMax{
                std::max({viewportPositions[0].v[0], viewportPositions[1].v[0], viewportPositions[2].v[0]}),
                std::max({viewportPositions[0].v[1], viewportPositions[1].v[1], viewportPositions[2].v[1]})
            };

            const int x0 = std::max({scissorX0, 0, static_cast<int>(std::floor(screenMin.v[0]))});
            const int y0 = std::max({scissorY0, 0, static_cast<int>(std::floor(screenMin.v[1]))});
            const int x1 = std::min({scissorX1, static_cast<int>(width) - 1,
                                     static_cast<int>(std::floor(screenMax.v[0]))});
            const int y1 = std::min({scissorY1, static_cast<int>(height) - 1,
                                     static_cast<int>(std::floor(screenMax.v[1]))});
            if (x1 < x0 || y1 < y0) continue;

            const auto e0 = viewportPositions[1] - viewportPositions[0];
            const auto e1 = viewportPositions[2] - viewportPositions[0];
            const auto den = e0.v[0] * e1.v[1] - e1.v[0] * e0.v[1];
            if (den == 0.0F) continue;

            for (int screenY = y0; screenY <= y1; ++screenY)
                for (int screenX = x0; screenX <= x1; ++screenX)
                {
                    const Vector<float, 2> p{
                        static_cast<float>(screenX),
                        static_cast<float>(screenY)
                    };

                    const auto e2 = p - viewportPositions[0];

                    // screen-space barycentric coordinates
                    const auto v = (e2.v[0] * e1.v[1] - e1.v[0] * e2.v[1]) / den;
                    const auto w = (e0.v[0] * e2.v[1] - e2.v[0] * e0.v[1]) / den;

                    if (v < 0.0F || w < 0.0F || v + w > 1.0F)
                        continue;

                    const auto u = 1.0F - v - w;

                    // projected z/w interpolated affinely in screen space
                    const auto depth = ndcPositions[0].v[2] * u +
                        ndcPositions[1].v[2] * v + ndcPositions[2].v[2] * w;

                    const auto depthIndex = static_cast<std::size_t>(screenY) * depthBuffer.getWidth() +
                        static_cast<std::size_t>(screenX);

                    if (depthState.read && depthBufferData[depthIndex] < depth)
                        continue; // discard the pixel

                    if (depthState.write)
                        depthBufferData[depthIndex] = depth;

                    // perspective-correct weights for the other attributes
                    Vector<float, 3> clip{
                        u / clipW0,
                        v / clipW1,
                        w / clipW2
                    };
                    clip /= (clip.v[0] + clip.v[1] + clip.v[2]);

                    VertexShaderOutput psInput;
                    psInput.position = Vector<float, 4>{clip.v[0], clip.v[1], clip.v[2], 1.0F};
                    psInput.color = Color{
                        tv[0].color.r * clip.v[0] + tv[1].color.r * clip.v[1] + tv[2].color.r * clip.v[2],
                        tv[0].color.g * clip.v[0] + tv[1].color.g * clip.v[1] + tv[2].color.g * clip.v[2],
                        tv[0].color.b * clip.v[0] + tv[1].color.b * clip.v[1] + tv[2].color.b * clip.v[2],
                        tv[0].color.a * clip.v[0] + tv[1].color.a * clip.v[1] + tv[2].color.a * clip.v[2]
                    };

                    psInput.texCoords[0] = Vector<float, 2>{
                        tv[0].texCoords[0].v[0] * clip.v[0] + tv[1].texCoords[0].v[0] * clip.v[1] +
                            tv[2].texCoords[0].v[0] * clip.v[2],
                        tv[0].texCoords[0].v[1] * clip.v[0] + tv[1].texCoords[0].v[1] * clip.v[1] +
                            tv[2].texCoords[0].v[1] * clip.v[2]
                    };

                    psInput.texCoords[1] = Vector<float, 2>{
                        tv[0].texCoords[1].v[0] * clip.v[0] + tv[1].texCoords[1].v[0] * clip.v[1] +
                            tv[2].texCoords[1].v[0] * clip.v[2],
                        tv[0].texCoords[1].v[1] * clip.v[0] + tv[1].texCoords[1].v[1] * clip.v[1] +
                            tv[2].texCoords[1].v[1] * clip.v[2]
                    };

                    psInput.normal = tv[0].normal * clip.v[0] + tv[1].normal * clip.v[1] +
                        tv[2].normal * clip.v[2];

                    const auto srcColor = fragmentShader(psInput, samplers, textures);
                    const auto pixelIndex = static_cast<std::size_t>(screenY) * width +
                        static_cast<std::size_t>(screenX);

                    if (blendState.enabled)
                    {
                        const auto pixel = reinterpret_cast<std::uint8_t*>(&frameBufferData[pixelIndex]);
                        const Color destColor{pixel[0], pixel[1], pixel[2], pixel[3]};

                        // alpha blend
                        const Color resultColor{
                            getValue(blendState.colorOperation,
                                     srcColor.r * getValue(blendState.colorBlendSource, srcColor.r, srcColor.a, destColor.r, destColor.a, blendState.blendFactor.r),
                                     destColor.r * getValue(blendState.colorBlendDest, srcColor.r, srcColor.a, destColor.r, destColor.a, blendState.blendFactor.r)),
                            getValue(blendState.colorOperation,
                                     srcColor.g * getValue(blendState.colorBlendSource, srcColor.g, srcColor.a, destColor.g, destColor.a, blendState.blendFactor.g),
                                     destColor.g * getValue(blendState.colorBlendDest, srcColor.g, srcColor.a, destColor.g, destColor.a, blendState.blendFactor.g)),
                            getValue(blendState.colorOperation,
                                     srcColor.b * getValue(blendState.colorBlendSource, srcColor.b, srcColor.a, destColor.b, destColor.a, blendState.blendFactor.b),
                                     destColor.b * getValue(blendState.colorBlendDest, srcColor.b, srcColor.a, destColor.b, destColor.a, blendState.blendFactor.b)),
                            getValue(blendState.alphaOperation,
                                     srcColor.a * getValue(blendState.alphaBlendSource, srcColor.a, srcColor.a, destColor.a, destColor.a, blendState.blendFactor.a),
                                     destColor.a * getValue(blendState.alphaBlendDest, srcColor.a, srcColor.a, destColor.a, destColor.a, blendState.blendFactor.a))
                        };

                        frameBufferData[pixelIndex] = resultColor.getIntValueRaw();
                    }
                    else
                        frameBufferData[pixelIndex] = srcColor.getIntValueRaw();
                }
        }
    }
}

#endif
