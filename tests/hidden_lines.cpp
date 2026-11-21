#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
#include "task_support/check.h"
#include "sr.hpp"

namespace
{
    sr::VertexShaderOutput testVertexShader(const sr::Matrix<float, 4>& m, const sr::Vertex& v)
    {
        sr::VertexShaderOutput o{};
        o.position = m * v.position;
        o.color = v.color;
        o.texCoords[0] = v.texCoords[0];
        o.texCoords[1] = v.texCoords[1];
        o.normal = v.normal;
        return o;
    }

    sr::Color testFragmentShader(const sr::VertexShaderOutput& v,
                                 const std::array<const sr::Sampler*, 2>&,
                                 const std::array<const sr::Texture*, 2>&)
    {
        return v.color;
    }

    sr::Vertex makeVertex(const float x, const float y, const float z, const float w,
                          const sr::Color color = sr::Color{1.0F, 1.0F, 1.0F, 1.0F})
    {
        return sr::Vertex{sr::Vector<float, 4>{x, y, z, w}, color,
                          sr::Vector<float, 2>{0.0F, 0.0F}, sr::Vector<float, 3>{0.0F, 0.0F, 1.0F}};
    }

    const sr::Matrix<float, 4> identity = sr::Matrix<float, 4>::identity();
    const sr::Rect<float> unitViewport{0.0F, 0.0F, 100.0F, 100.0F};
    const sr::Rect<float> fullScissor{0.0F, 0.0F, 1.0F, 1.0F};

    // view positions on a front-facing plane; used when only boundary
    // classification matters
    std::vector<sr::Vector<float, 3>> flatViewPositions(const std::vector<sr::Vertex>& vertices,
                                                        const float z = 5.0F)
    {
        std::vector<sr::Vector<float, 3>> result;
        result.reserve(vertices.size());
        for (const auto& v : vertices)
            result.push_back(sr::Vector<float, 3>{v.position.v[0], v.position.v[1], z});
        return result;
    }

    std::vector<sr::Vector<float, 3>> viewPositionsOf(const std::vector<sr::Vertex>& vertices)
    {
        std::vector<sr::Vector<float, 3>> result;
        result.reserve(vertices.size());
        for (const auto& v : vertices)
            result.push_back(sr::Vector<float, 3>{v.position.v[0], v.position.v[1], v.position.v[2]});
        return result;
    }

    template <typename F> bool throwsInvalidArgument(F&& f)
    {
        try { static_cast<void>(f()); }
        catch (const std::invalid_argument&) { return true; }
        catch (...) { return false; }
        return false;
    }

    bool nearValue(const double a, const double b, const double tol)
    {
        return std::fabs(a - b) <= tol;
    }

    bool nearPoint(const sr::Vector<float, 2>& p, const double x, const double y, const double tol)
    {
        return nearValue(p.v[0], x, tol) && nearValue(p.v[1], y, tol);
    }

    std::vector<sr::VisibleEdgeSegment> segmentsOf(const std::vector<sr::VisibleEdgeSegment>& segments,
                                                   const std::size_t edge)
    {
        std::vector<sr::VisibleEdgeSegment> result;
        for (const auto& s : segments)
            if (s.edgeIndex == edge) result.push_back(s);
        return result;
    }

    bool sameSegments(const std::vector<sr::VisibleEdgeSegment>& a,
                      const std::vector<sr::VisibleEdgeSegment>& b)
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].edgeIndex != b[i].edgeIndex || a[i].index0 != b[i].index0 ||
                a[i].index1 != b[i].index1 || a[i].t0 != b[i].t0 || a[i].t1 != b[i].t1 ||
                a[i].p0 != b[i].p0 || a[i].p1 != b[i].p1)
                return false;
        return true;
    }

    void fillDepth(sr::Texture& depth, const float value = 1.0F)
    {
        auto* data = reinterpret_cast<float*>(depth.getData().data());
        std::fill(data, data + depth.getWidth() * depth.getHeight(), value);
    }

    const std::uint8_t* pixelAt(const sr::Texture& color, const std::size_t x, const std::size_t y)
    {
        return &color.getData()[(y * color.getWidth() + x) * 4];
    }

    float depthAt(const sr::Texture& depth, const std::size_t x, const std::size_t y)
    {
        return reinterpret_cast<const float*>(depth.getData().data())[y * depth.getWidth() + x];
    }

    sr::DepthState depthOn()
    {
        sr::DepthState state;
        state.read = true;
        state.write = true;
        return state;
    }

    void drawPrepared(const sr::PreparedMesh& mesh, sr::Texture& color, sr::Texture& depth,
                      const sr::Rect<float>& viewport, const sr::Rect<float>& scissor,
                      const sr::BlendState& blend, const sr::DepthState& depthState)
    {
        const std::array<const sr::Sampler*, 2> samplers{};
        const std::array<const sr::Texture*, 2> textures{};
        sr::drawTriangles(color, depth, testFragmentShader, samplers, textures,
                          viewport, scissor, blend, depthState, mesh);
    }

    // ---------------------------------------------------------------- geometry

    int caseClipIdentity()
    {
        // first triangle crosses the near plane, second is fully outside
        const std::vector<sr::Vertex> vertices{
            makeVertex(-0.5F, -0.5F, -0.5F, 1.0F),
            makeVertex(0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(-0.5F, 0.5F, 0.5F, 1.0F),
            makeVertex(0.0F, 0.0F, -0.5F, 1.0F),
            makeVertex(0.1F, 0.0F, -0.5F, 1.0F),
            makeVertex(0.0F, 0.1F, -0.5F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2, 3, 4, 5};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);

        TASK_CHECK(mesh.getVertexCount() == 6);
        TASK_CHECK(mesh.getIndices() == indices);
        TASK_CHECK(mesh.getTriangles().size() == 2); // clipped quad fan, nothing from triangle 1
        TASK_CHECK(mesh.getEdges().size() == 6);

        bool midpointFound = false;
        for (const auto& triangle : mesh.getTriangles())
        {
            TASK_CHECK(triangle.faceIndex == 0);
            for (const auto& v : triangle.vertices)
            {
                const auto& p = v.position.v;
                TASK_CHECK(p[0] >= -p[3] - 1.0E-5F && p[0] <= p[3] + 1.0E-5F);
                TASK_CHECK(p[1] >= -p[3] - 1.0E-5F && p[1] <= p[3] + 1.0E-5F);
                TASK_CHECK(p[2] >= -1.0E-5F && p[2] <= p[3] + 1.0E-5F);
                if (std::fabs(p[0]) < 1.0E-5F && std::fabs(p[1] + 0.5F) < 1.0E-5F &&
                    std::fabs(p[2]) < 1.0E-5F)
                    midpointFound = true; // the z = 0 clip midpoint (0, -0.5, 0, 1)
            }
        }
        TASK_CHECK(midpointFound);
        return 0;
    }

    int caseEdgesAdjacency()
    {
        const std::vector<sr::Vertex> quad{
            makeVertex(-0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.5F, 0.5F, 0.5F, 1.0F),
            makeVertex(-0.5F, 0.5F, 0.5F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2, 0, 2, 3};
        sr::PreparedMesh mesh(testVertexShader, identity, quad, indices);

        const auto& edges = mesh.getEdges();
        TASK_CHECK(edges.size() == 5);
        TASK_CHECK(edges[0].v0 == 0 && edges[0].v1 == 1);
        TASK_CHECK(edges[0].face0 == 0 && edges[0].face1 == sr::PreparedMesh::npos);
        TASK_CHECK(edges[1].v0 == 1 && edges[1].v1 == 2 && edges[1].face1 == sr::PreparedMesh::npos);
        // diagonal, oriented as first encountered, shared by both faces
        TASK_CHECK(edges[2].v0 == 2 && edges[2].v1 == 0);
        TASK_CHECK(edges[2].face0 == 0 && edges[2].face1 == 1);
        TASK_CHECK(edges[3].v0 == 2 && edges[3].v1 == 3 && edges[3].face0 == 1);
        TASK_CHECK(edges[4].v0 == 3 && edges[4].v1 == 0 && edges[4].face0 == 1);

        TASK_CHECK(mesh.getTriangles().size() == 2);
        TASK_CHECK(mesh.getTriangles()[0].faceIndex == 0);
        TASK_CHECK(mesh.getTriangles()[1].faceIndex == 1);
        return 0;
    }

    int caseInvalidInputs()
    {
        const std::vector<sr::Vertex> triangle{
            makeVertex(-0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.0F, 0.5F, 0.5F, 1.0F)
        };
        const std::vector<std::size_t> triIndices{0, 1, 2};
        const float nan = std::numeric_limits<float>::quiet_NaN();

        // index count not a multiple of 3
        TASK_CHECK(throwsInvalidArgument([&] {
            sr::PreparedMesh m(testVertexShader, identity, triangle, std::vector<std::size_t>{0, 1});
        }));
        // index out of range
        TASK_CHECK(throwsInvalidArgument([&] {
            sr::PreparedMesh m(testVertexShader, identity, triangle, std::vector<std::size_t>{0, 1, 3});
        }));
        // degenerate triangle
        TASK_CHECK(throwsInvalidArgument([&] {
            sr::PreparedMesh m(testVertexShader, identity, triangle, std::vector<std::size_t>{0, 1, 1});
        }));
        // non-finite vertex position
        TASK_CHECK(throwsInvalidArgument([&] {
            auto bad = triangle;
            bad[1].position.v[0] = nan;
            sr::PreparedMesh m(testVertexShader, identity, bad, triIndices);
        }));
        // non-finite matrix
        TASK_CHECK(throwsInvalidArgument([&] {
            auto bad = identity;
            bad.m[5] = nan;
            sr::PreparedMesh m(testVertexShader, bad, triangle, triIndices);
        }));
        // non-manifold edge: three faces share edge (0, 1)
        TASK_CHECK(throwsInvalidArgument([&] {
            const std::vector<sr::Vertex> fan{
                makeVertex(0.0F, 0.0F, 0.5F, 1.0F), makeVertex(1.0F, 0.0F, 0.5F, 1.0F),
                makeVertex(0.0F, 1.0F, 0.5F, 1.0F), makeVertex(1.0F, 1.0F, 0.5F, 1.0F),
                makeVertex(-1.0F, -1.0F, 0.5F, 1.0F)
            };
            sr::PreparedMesh m(testVertexShader, identity, fan,
                               std::vector<std::size_t>{0, 1, 2, 0, 1, 3, 0, 1, 4});
        }));
        // null vertex shader
        TASK_CHECK(throwsInvalidArgument([&] {
            sr::PreparedMesh m(nullptr, identity, triangle, triIndices);
        }));

        // a valid mesh for the visibleEdges validation below
        sr::PreparedMesh mesh(testVertexShader, identity, triangle, triIndices);
        const auto viewPositions = flatViewPositions(triangle);

        TASK_CHECK(throwsInvalidArgument([&] {
            return sr::visibleEdges(mesh, viewPositions, sr::Rect<float>{0.0F, 0.0F, 0.0F, 100.0F}, 0.5F);
        }));
        TASK_CHECK(throwsInvalidArgument([&] {
            return sr::visibleEdges(mesh, viewPositions, sr::Rect<float>{0.0F, 0.0F, 100.0F, -1.0F}, 0.5F);
        }));
        TASK_CHECK(throwsInvalidArgument([&] {
            return sr::visibleEdges(mesh, viewPositions, sr::Rect<float>{nan, 0.0F, 100.0F, 100.0F}, 0.5F);
        }));
        TASK_CHECK(throwsInvalidArgument([&] {
            return sr::visibleEdges(mesh, std::vector<sr::Vector<float, 3>>{}, unitViewport, 0.5F);
        }));
        TASK_CHECK(throwsInvalidArgument([&] {
            auto bad = viewPositions;
            bad[1].v[2] = nan;
            return sr::visibleEdges(mesh, bad, unitViewport, 0.5F);
        }));
        TASK_CHECK(throwsInvalidArgument([&] {
            return sr::visibleEdges(mesh, viewPositions, unitViewport, nan);
        }));

        // the valid call still works after all the failures
        TASK_CHECK(sr::visibleEdges(mesh, viewPositions, unitViewport, 0.5F).size() == 3);
        return 0;
    }

    int caseBoundaryProjection()
    {
        const std::vector<sr::Vertex> vertices{
            makeVertex(-0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.0F, 0.5F, 0.5F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);

        // non-zero origin, non-square viewport
        const sr::Rect<float> viewport{10.0F, 20.0F, 100.0F, 50.0F};
        const auto segments = sr::visibleEdges(mesh, flatViewPositions(vertices), viewport, 0.5F);

        TASK_CHECK(segments.size() == 3);
        // screen x = 50 * ndc_x + 60, screen y = 25 * ndc_y + 45
        TASK_CHECK(segments[0].edgeIndex == 0 && segments[0].index0 == 0 && segments[0].index1 == 1);
        TASK_CHECK(nearValue(segments[0].t0, 0.0, 1.0E-9) && nearValue(segments[0].t1, 1.0, 1.0E-9));
        TASK_CHECK(nearPoint(segments[0].p0, 35.0, 32.5, 1.0E-3));
        TASK_CHECK(nearPoint(segments[0].p1, 85.0, 32.5, 1.0E-3));

        TASK_CHECK(segments[1].edgeIndex == 1 && segments[1].index0 == 1 && segments[1].index1 == 2);
        TASK_CHECK(nearPoint(segments[1].p0, 85.0, 32.5, 1.0E-3));
        TASK_CHECK(nearPoint(segments[1].p1, 60.0, 57.5, 1.0E-3));

        TASK_CHECK(segments[2].edgeIndex == 2 && segments[2].index0 == 2 && segments[2].index1 == 0);
        TASK_CHECK(nearPoint(segments[2].p0, 60.0, 57.5, 1.0E-3));
        TASK_CHECK(nearPoint(segments[2].p1, 35.0, 32.5, 1.0E-3));
        return 0;
    }

    int caseEdgeNearClip()
    {
        const std::vector<sr::Vertex> vertices{
            makeVertex(-1.0F, 0.0F, -0.5F, 1.0F), // behind the near plane
            makeVertex(1.0F, 0.0F, 0.5F, 1.0F),
            makeVertex(0.0F, 1.0F, 0.25F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);

        const sr::Rect<float> viewport{10.0F, 20.0F, 100.0F, 50.0F};
        const auto segments = sr::visibleEdges(mesh, flatViewPositions(vertices), viewport, 0.5F);

        // edge (0, 1) is clipped to t in [0.5, 1] by the near plane
        const auto e0 = segmentsOf(segments, 0);
        TASK_CHECK(e0.size() == 1);
        TASK_CHECK(nearValue(e0[0].t0, 0.5, 1.0E-9) && nearValue(e0[0].t1, 1.0, 1.0E-9));
        TASK_CHECK(nearPoint(e0[0].p0, 60.0, 45.0, 1.0E-3));
        TASK_CHECK(nearPoint(e0[0].p1, 110.0, 45.0, 1.0E-3));

        // edge (2, 0) is clipped to t in [0, 1/3]
        const auto e2 = segmentsOf(segments, 2);
        TASK_CHECK(e2.size() == 1);
        TASK_CHECK(nearValue(e2[0].t0, 0.0, 1.0E-9) && nearValue(e2[0].t1, 1.0 / 3.0, 1.0E-9));
        TASK_CHECK(nearPoint(e2[0].p0, 60.0, 70.0, 1.0E-3));
        TASK_CHECK(nearPoint(e2[0].p1, 43.333333, 61.666667, 1.0E-3));
        return 0;
    }

    int caseClassification()
    {
        sr::Matrix<float, 4> perspective;
        perspective.setPerspective(sr::pi<float> / 3.0F, 1.0F, 1.0F, 20.0F);

        // tent: both faces front, 90 degree dihedral
        const std::vector<sr::Vertex> tentVertices{
            makeVertex(-1.0F, 0.0F, 5.0F, 1.0F), makeVertex(1.0F, 0.0F, 5.0F, 1.0F),
            makeVertex(0.0F, -1.0F, 4.0F, 1.0F), makeVertex(0.0F, 1.0F, 4.0F, 1.0F)
        };
        const std::vector<std::size_t> tentIndices{0, 1, 2, 1, 0, 3};
        sr::PreparedMesh tent(testVertexShader, perspective, tentVertices, tentIndices);
        const auto tentView = viewPositionsOf(tentVertices);

        // shared edge is a crease for a small threshold
        auto segments = sr::visibleEdges(tent, tentView, unitViewport, 0.5F);
        auto shared = segmentsOf(segments, 0);
        TASK_CHECK(shared.size() == 1);
        TASK_CHECK(nearValue(shared[0].t0, 0.0, 1.0E-9) && nearValue(shared[0].t1, 1.0, 1.0E-9));
        // ... but not for a threshold above 90 degrees
        segments = sr::visibleEdges(tent, tentView, unitViewport, 1.6F);
        TASK_CHECK(segmentsOf(segments, 0).empty());

        // coplanar quad: the diagonal is never emitted
        const std::vector<sr::Vertex> quadVertices{
            makeVertex(-1.0F, -1.0F, 5.0F, 1.0F), makeVertex(1.0F, -1.0F, 5.0F, 1.0F),
            makeVertex(1.0F, 1.0F, 5.0F, 1.0F), makeVertex(-1.0F, 1.0F, 5.0F, 1.0F)
        };
        const std::vector<std::size_t> quadIndices{0, 1, 2, 0, 2, 3};
        sr::PreparedMesh quad(testVertexShader, perspective, quadVertices, quadIndices);
        segments = sr::visibleEdges(quad, viewPositionsOf(quadVertices), unitViewport, 0.5F);
        TASK_CHECK(segmentsOf(segments, 2).empty());
        TASK_CHECK(segments.size() == 4); // boundary edges only

        // fold: one front face and one back face meet at the shared edge
        const std::vector<sr::Vertex> foldVertices{
            makeVertex(-1.0F, 0.0F, 5.0F, 1.0F), makeVertex(1.0F, 0.0F, 5.0F, 1.0F),
            makeVertex(0.0F, -1.0F, 4.0F, 1.0F), makeVertex(0.0F, -0.5F, 6.0F, 1.0F)
        };
        const std::vector<std::size_t> foldIndices{0, 1, 2, 1, 0, 3};
        sr::PreparedMesh fold(testVertexShader, perspective, foldVertices, foldIndices);
        // silhouette edge even with a threshold above the dihedral angle
        segments = sr::visibleEdges(fold, viewPositionsOf(foldVertices), unitViewport, 1.6F);
        TASK_CHECK(segmentsOf(segments, 0).size() == 1);

        // a back-facing triangle still emits its boundary edges
        const std::vector<sr::Vertex> backVertices{
            makeVertex(0.0F, 0.0F, 5.0F, 1.0F), makeVertex(1.0F, 0.0F, 5.0F, 1.0F),
            makeVertex(0.0F, 1.0F, 5.0F, 1.0F)
        };
        sr::PreparedMesh back(testVertexShader, perspective, backVertices,
                              std::vector<std::size_t>{0, 1, 2});
        segments = sr::visibleEdges(back, viewPositionsOf(backVertices), unitViewport, 0.5F);
        TASK_CHECK(segments.size() == 3);
        return 0;
    }

    int caseViewportSwitch()
    {
        const std::vector<sr::Vertex> vertices{
            makeVertex(-0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.0F, 0.5F, 0.5F, 1.0F)
        };
        sr::PreparedMesh mesh(testVertexShader, identity, vertices,
                              std::vector<std::size_t>{0, 1, 2});
        const auto viewPositions = flatViewPositions(vertices);

        const auto first = segmentsOf(
            sr::visibleEdges(mesh, viewPositions, sr::Rect<float>{0.0F, 0.0F, 100.0F, 100.0F}, 0.5F), 0);
        TASK_CHECK(first.size() == 1);
        TASK_CHECK(nearPoint(first[0].p0, 25.0, 25.0, 1.0E-3));
        TASK_CHECK(nearPoint(first[0].p1, 75.0, 25.0, 1.0E-3));

        // same prepared mesh, different viewport with non-zero origin
        const auto second = segmentsOf(
            sr::visibleEdges(mesh, viewPositions, sr::Rect<float>{10.0F, 20.0F, 200.0F, 50.0F}, 0.5F), 0);
        TASK_CHECK(second.size() == 1);
        TASK_CHECK(nearPoint(second[0].p0, 60.0, 32.5, 1.0E-3));
        TASK_CHECK(nearPoint(second[0].p1, 160.0, 32.5, 1.0E-3));
        TASK_CHECK(nearValue(second[0].t0, 0.0, 1.0E-9) && nearValue(second[0].t1, 1.0, 1.0E-9));
        return 0;
    }

    // --------------------------------------------------------------- visibility

    int caseOccluderMiddle()
    {
        const std::vector<sr::Vertex> vertices{
            makeVertex(-1.0F, 0.0F, 0.5F, 1.0F),  // 0: edge start
            makeVertex(1.0F, 0.0F, 0.5F, 1.0F),   // 1: edge end
            makeVertex(0.0F, 0.8F, 0.5F, 1.0F),   // 2: carrier third vertex
            makeVertex(-0.4F, -0.5F, 0.25F, 1.0F), // 3..6: occluding quad
            makeVertex(0.4F, -0.5F, 0.25F, 1.0F),
            makeVertex(0.4F, 0.5F, 0.25F, 1.0F),
            makeVertex(-0.4F, 0.5F, 0.25F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2, 3, 4, 5, 3, 5, 6};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);

        const auto segments = segmentsOf(
            sr::visibleEdges(mesh, flatViewPositions(vertices), unitViewport, 0.5F), 0);
        TASK_CHECK(segments.size() == 2);
        TASK_CHECK(nearValue(segments[0].t0, 0.0, 1.0E-6) && nearValue(segments[0].t1, 0.3, 1.0E-6));
        TASK_CHECK(nearValue(segments[1].t0, 0.7, 1.0E-6) && nearValue(segments[1].t1, 1.0, 1.0E-6));
        TASK_CHECK(nearPoint(segments[0].p0, 0.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[0].p1, 30.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[1].p0, 70.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[1].p1, 100.0, 50.0, 1.0E-3));
        TASK_CHECK(segments[0].index0 == 0 && segments[0].index1 == 1);
        return 0;
    }

    int caseWVarying()
    {
        // endpoints have different w: the screen parameter and the original
        // edge parameter differ and the output must restore the latter
        const std::vector<sr::Vertex> vertices{
            makeVertex(-1.0F, 0.0F, 0.5F, 1.0F), // 0: edge start, w = 1
            makeVertex(1.0F, 0.0F, 1.5F, 3.0F),  // 1: edge end, w = 3
            makeVertex(0.0F, 1.0F, 1.0F, 2.0F),  // 2: carrier third vertex
            makeVertex(-0.2F, -0.5F, 0.25F, 1.0F), // 3..6: occluding quad
            makeVertex(0.2F, -0.5F, 0.25F, 1.0F),
            makeVertex(0.2F, 0.5F, 0.25F, 1.0F),
            makeVertex(-0.2F, 0.5F, 0.25F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2, 3, 4, 5, 3, 5, 6};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);

        const sr::Rect<float> viewport{0.0F, 0.0F, 200.0F, 100.0F};
        const auto segments = segmentsOf(
            sr::visibleEdges(mesh, flatViewPositions(vertices), viewport, 0.5F), 0);

        // the occluder covers screen u in [0.6, 0.9] which maps back to
        // the original edge parameter t in [1/3, 0.75]
        TASK_CHECK(segments.size() == 2);
        TASK_CHECK(nearValue(segments[0].t0, 0.0, 1.0E-6));
        TASK_CHECK(nearValue(segments[0].t1, 1.0 / 3.0, 1.0E-6));
        TASK_CHECK(nearValue(segments[1].t0, 0.75, 1.0E-6));
        TASK_CHECK(nearValue(segments[1].t1, 1.0, 1.0E-6));
        TASK_CHECK(nearPoint(segments[0].p0, 0.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[0].p1, 80.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[1].p0, 120.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[1].p1, 133.333333, 50.0, 1.0E-3));
        return 0;
    }

    int caseDepthReversal()
    {
        // the depth order between the edge and the occluder reverses inside
        // the overlap: only the second half of the overlap is hidden
        const std::vector<sr::Vertex> vertices{
            makeVertex(0.0F, 0.0F, 0.2F, 1.0F),  // 0: edge start
            makeVertex(1.0F, 0.0F, 0.8F, 1.0F),  // 1: edge end
            makeVertex(0.5F, 0.8F, 0.5F, 1.0F),  // 2: carrier third vertex
            makeVertex(0.2F, -0.5F, 0.68F, 1.0F), // 3..6: tilted occluding quad
            makeVertex(0.8F, -0.5F, 0.32F, 1.0F),
            makeVertex(0.8F, 0.5F, 0.32F, 1.0F),
            makeVertex(0.2F, 0.5F, 0.68F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2, 3, 4, 5, 3, 5, 6};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);

        const auto segments = segmentsOf(
            sr::visibleEdges(mesh, flatViewPositions(vertices), unitViewport, 0.5F), 0);
        // occluder covers x in [0.2, 0.8] but is closer only for x > 0.5
        TASK_CHECK(segments.size() == 2);
        TASK_CHECK(nearValue(segments[0].t0, 0.0, 1.0E-6) && nearValue(segments[0].t1, 0.5, 1.0E-6));
        TASK_CHECK(nearValue(segments[1].t0, 0.8, 1.0E-6) && nearValue(segments[1].t1, 1.0, 1.0E-6));
        TASK_CHECK(nearPoint(segments[0].p0, 50.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[0].p1, 75.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[1].p0, 90.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[1].p1, 100.0, 50.0, 1.0E-3));
        return 0;
    }

    int caseEqualDepth()
    {
        // a coplanar occluder never hides the edge: equal depth does not occlude
        const std::vector<sr::Vertex> vertices{
            makeVertex(-1.0F, 0.0F, 0.5F, 1.0F),
            makeVertex(1.0F, 0.0F, 0.5F, 1.0F),
            makeVertex(0.0F, 0.8F, 0.5F, 1.0F),
            makeVertex(-0.4F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.4F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.4F, 0.5F, 0.5F, 1.0F),
            makeVertex(-0.4F, 0.5F, 0.5F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2, 3, 4, 5, 3, 5, 6};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);

        const auto segments = segmentsOf(
            sr::visibleEdges(mesh, flatViewPositions(vertices), unitViewport, 0.5F), 0);
        TASK_CHECK(segments.size() == 1);
        TASK_CHECK(nearValue(segments[0].t0, 0.0, 1.0E-9) && nearValue(segments[0].t1, 1.0, 1.0E-9));
        TASK_CHECK(nearPoint(segments[0].p0, 0.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[0].p1, 100.0, 50.0, 1.0E-3));
        return 0;
    }

    int caseOverlappingOccluders()
    {
        // two occluders whose hidden intervals overlap must be united
        const std::vector<sr::Vertex> vertices{
            makeVertex(-1.0F, 0.0F, 0.5F, 1.0F),
            makeVertex(1.0F, 0.0F, 0.5F, 1.0F),
            makeVertex(0.0F, 0.8F, 0.5F, 1.0F),
            makeVertex(-0.8F, -0.5F, 0.3F, 1.0F), // 3..6: first occluder
            makeVertex(0.2F, -0.5F, 0.3F, 1.0F),
            makeVertex(0.2F, 0.5F, 0.3F, 1.0F),
            makeVertex(-0.8F, 0.5F, 0.3F, 1.0F),
            makeVertex(-0.2F, -0.5F, 0.4F, 1.0F), // 7..10: second occluder
            makeVertex(0.8F, -0.5F, 0.4F, 1.0F),
            makeVertex(0.8F, 0.5F, 0.4F, 1.0F),
            makeVertex(-0.2F, 0.5F, 0.4F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2, 3, 4, 5, 3, 5, 6, 7, 8, 9, 7, 9, 10};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);

        const auto segments = segmentsOf(
            sr::visibleEdges(mesh, flatViewPositions(vertices), unitViewport, 0.5F), 0);
        TASK_CHECK(segments.size() == 2);
        TASK_CHECK(nearValue(segments[0].t0, 0.0, 1.0E-6) && nearValue(segments[0].t1, 0.1, 1.0E-6));
        TASK_CHECK(nearValue(segments[1].t0, 0.9, 1.0E-6) && nearValue(segments[1].t1, 1.0, 1.0E-6));
        TASK_CHECK(nearPoint(segments[0].p0, 0.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[0].p1, 10.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[1].p0, 90.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[1].p1, 100.0, 50.0, 1.0E-3));
        return 0;
    }

    int caseNearPlaneBoth()
    {
        // the near plane clips both the edge and the occluder
        const std::vector<sr::Vertex> vertices{
            makeVertex(-0.8F, 0.0F, -0.4F, 1.0F), // 0: edge start, behind the near plane
            makeVertex(0.8F, 0.0F, 0.6F, 1.0F),   // 1: edge end
            makeVertex(0.5F, 0.8F, 0.5F, 1.0F),   // 2: carrier third vertex
            makeVertex(0.0F, -0.5F, -0.2F, 1.0F), // 3..6: occluder crossing the near plane
            makeVertex(0.6F, -0.5F, 0.4F, 1.0F),
            makeVertex(0.6F, 0.5F, 0.4F, 1.0F),
            makeVertex(0.0F, 0.5F, -0.2F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2, 3, 4, 5, 3, 5, 6};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);

        const auto segments = segmentsOf(
            sr::visibleEdges(mesh, flatViewPositions(vertices), unitViewport, 0.5F), 0);
        // edge clipped to t in [0.4, 1]; clipped occluder hides t in [0.625, 0.875]
        TASK_CHECK(segments.size() == 2);
        TASK_CHECK(nearValue(segments[0].t0, 0.4, 1.0E-5) && nearValue(segments[0].t1, 0.625, 1.0E-5));
        TASK_CHECK(nearValue(segments[1].t0, 0.875, 1.0E-5) && nearValue(segments[1].t1, 1.0, 1.0E-5));
        TASK_CHECK(nearPoint(segments[0].p0, 42.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[0].p1, 60.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[1].p0, 80.0, 50.0, 1.0E-3));
        TASK_CHECK(nearPoint(segments[1].p1, 90.0, 50.0, 1.0E-3));
        return 0;
    }

    int caseBackfaceOccluder()
    {
        // same scene as occluderMiddle but the occluder is wound backwards;
        // occluding faces are double-sided
        const std::vector<sr::Vertex> vertices{
            makeVertex(-1.0F, 0.0F, 0.5F, 1.0F),
            makeVertex(1.0F, 0.0F, 0.5F, 1.0F),
            makeVertex(0.0F, 0.8F, 0.5F, 1.0F),
            makeVertex(-0.4F, -0.5F, 0.25F, 1.0F),
            makeVertex(0.4F, -0.5F, 0.25F, 1.0F),
            makeVertex(0.4F, 0.5F, 0.25F, 1.0F),
            makeVertex(-0.4F, 0.5F, 0.25F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2, 3, 5, 4, 3, 6, 5};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);

        const auto segments = segmentsOf(
            sr::visibleEdges(mesh, flatViewPositions(vertices), unitViewport, 0.5F), 0);
        TASK_CHECK(segments.size() == 2);
        TASK_CHECK(nearValue(segments[0].t0, 0.0, 1.0E-6) && nearValue(segments[0].t1, 0.3, 1.0E-6));
        TASK_CHECK(nearValue(segments[1].t0, 0.7, 1.0E-6) && nearValue(segments[1].t1, 1.0, 1.0E-6));
        return 0;
    }

    int caseOccluderBehind()
    {
        // an occluder behind the edge hides nothing
        const std::vector<sr::Vertex> vertices{
            makeVertex(-1.0F, 0.0F, 0.5F, 1.0F),
            makeVertex(1.0F, 0.0F, 0.5F, 1.0F),
            makeVertex(0.0F, 0.8F, 0.5F, 1.0F),
            makeVertex(-0.4F, -0.5F, 0.75F, 1.0F),
            makeVertex(0.4F, -0.5F, 0.75F, 1.0F),
            makeVertex(0.4F, 0.5F, 0.75F, 1.0F),
            makeVertex(-0.4F, 0.5F, 0.75F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2, 3, 4, 5, 3, 5, 6};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);

        const auto segments = segmentsOf(
            sr::visibleEdges(mesh, flatViewPositions(vertices), unitViewport, 0.5F), 0);
        TASK_CHECK(segments.size() == 1);
        TASK_CHECK(nearValue(segments[0].t0, 0.0, 1.0E-9) && nearValue(segments[0].t1, 1.0, 1.0E-9));
        return 0;
    }

    int caseDrawClipping()
    {
        // a triangle crossing the near plane is clipped before rasterization
        const std::vector<sr::Vertex> vertices{
            makeVertex(-0.5F, -0.5F, -0.5F, 1.0F),
            makeVertex(0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(-0.5F, 0.5F, 0.5F, 1.0F)
        };
        sr::PreparedMesh mesh(testVertexShader, identity, vertices,
                              std::vector<std::size_t>{0, 1, 2});

        sr::Texture color(sr::PixelFormat::rgba8, 8, 8);
        sr::Texture depth(sr::PixelFormat::float32, 8, 8);
        fillDepth(depth);
        drawPrepared(mesh, color, depth, sr::Rect<float>{0.0F, 0.0F, 8.0F, 8.0F},
                     fullScissor, sr::BlendState{}, depthOn());

        const auto* inside = pixelAt(color, 5, 2);
        TASK_CHECK(inside[0] == 255 && inside[3] == 255);
        const auto* clippedAway = pixelAt(color, 2, 2); // inside the original, outside the clipped triangle
        TASK_CHECK(clippedAway[0] == 0 && clippedAway[3] == 0);
        const auto* outside = pixelAt(color, 1, 1);
        TASK_CHECK(outside[0] == 0 && outside[3] == 0);
        const auto* onEdge = pixelAt(color, 4, 4);
        TASK_CHECK(onEdge[0] == 255 && onEdge[3] == 255);
        TASK_CHECK(nearValue(depthAt(depth, 4, 4), 0.5, 1.0E-5));
        TASK_CHECK(nearValue(depthAt(depth, 5, 2), 0.25, 1.0E-5));
        return 0;
    }

    int caseDrawDepthAttributes()
    {
        // varying w: depth must be the affine screen-space interpolation of
        // z/w (0.3 here, not the perspective-correct 0.2375) while color
        // uses perspective-correct weights
        const sr::Color red{1.0F, 0.0F, 0.0F, 1.0F};
        const sr::Color green{0.0F, 1.0F, 0.0F, 1.0F};
        const sr::Color blue{0.0F, 0.0F, 1.0F, 1.0F};
        const std::vector<sr::Vertex> vertices{
            makeVertex(-1.0F, -1.0F, 0.2F, 1.0F, red),
            makeVertex(1.5F, -3.0F, 2.4F, 3.0F, green), // ndc (0.5, -1, 0.8)
            makeVertex(-1.0F, 1.0F, 0.2F, 1.0F, blue)
        };
        sr::PreparedMesh mesh(testVertexShader, identity, vertices,
                              std::vector<std::size_t>{0, 1, 2});

        sr::Texture color(sr::PixelFormat::rgba8, 8, 8);
        sr::Texture depth(sr::PixelFormat::float32, 8, 8);
        fillDepth(depth);
        drawPrepared(mesh, color, depth, sr::Rect<float>{0.0F, 0.0F, 8.0F, 8.0F},
                     fullScissor, sr::BlendState{}, depthOn());

        TASK_CHECK(nearValue(depthAt(depth, 1, 1), 0.3, 1.0E-5));
        const auto* center = pixelAt(color, 1, 1);
        TASK_CHECK(std::abs(static_cast<int>(center[0]) - 203) <= 1); // 0.796875 * 255
        TASK_CHECK(std::abs(static_cast<int>(center[1]) - 15) <= 1);  // 0.0625 * 255
        TASK_CHECK(std::abs(static_cast<int>(center[2]) - 35) <= 1);  // 0.140625 * 255

        TASK_CHECK(nearValue(depthAt(depth, 4, 0), 0.6, 1.0E-5));
        const auto* edge = pixelAt(color, 4, 0);
        TASK_CHECK(std::abs(static_cast<int>(edge[0]) - 153) <= 1); // 0.6 * 255
        TASK_CHECK(std::abs(static_cast<int>(edge[1]) - 102) <= 1); // 0.4 * 255
        TASK_CHECK(edge[2] <= 1);
        return 0;
    }

    int caseDrawDepthBlendScissor()
    {
        const sr::Color red{1.0F, 0.0F, 0.0F, 1.0F};
        const sr::Color blue{0.0F, 0.0F, 1.0F, 1.0F};

        // depth order: the nearer triangle wins regardless of submission order
        const std::vector<sr::Vertex> nearFar{
            makeVertex(-0.9F, -0.9F, 0.3F, 1.0F, red),
            makeVertex(0.9F, -0.9F, 0.3F, 1.0F, red),
            makeVertex(0.0F, 0.9F, 0.3F, 1.0F, red),
            makeVertex(-0.9F, -0.9F, 0.6F, 1.0F, blue),
            makeVertex(0.9F, -0.9F, 0.6F, 1.0F, blue),
            makeVertex(0.0F, 0.9F, 0.6F, 1.0F, blue)
        };
        sr::PreparedMesh nearFirst(testVertexShader, identity, nearFar,
                                   std::vector<std::size_t>{0, 1, 2, 3, 4, 5});
        sr::PreparedMesh farFirst(testVertexShader, identity, nearFar,
                                  std::vector<std::size_t>{3, 4, 5, 0, 1, 2});
        for (const sr::PreparedMesh* mesh : {&nearFirst, &farFirst})
        {
            sr::Texture color(sr::PixelFormat::rgba8, 8, 8);
            sr::Texture depth(sr::PixelFormat::float32, 8, 8);
            fillDepth(depth);
            drawPrepared(*mesh, color, depth, sr::Rect<float>{0.0F, 0.0F, 8.0F, 8.0F},
                         fullScissor, sr::BlendState{}, depthOn());
            const auto* pixel = pixelAt(color, 4, 4);
            TASK_CHECK(pixel[0] == 255 && pixel[2] == 0);
            TASK_CHECK(nearValue(depthAt(depth, 4, 4), 0.3, 1.0E-6));
        }

        // blending: semi-transparent red over black
        {
            const sr::Color glass{1.0F, 0.0F, 0.0F, 0.5F};
            const std::vector<sr::Vertex> vertices{
                makeVertex(-1.0F, -1.0F, 0.5F, 1.0F, glass),
                makeVertex(1.0F, -1.0F, 0.5F, 1.0F, glass),
                makeVertex(0.0F, 1.0F, 0.5F, 1.0F, glass)
            };
            sr::PreparedMesh mesh(testVertexShader, identity, vertices,
                                  std::vector<std::size_t>{0, 1, 2});
            sr::BlendState blend;
            blend.enabled = true;
            blend.colorBlendSource = sr::BlendState::Factor::srcAlpha;
            blend.colorBlendDest = sr::BlendState::Factor::invSrcAlpha;

            sr::Texture color(sr::PixelFormat::rgba8, 8, 8);
            sr::Texture depth(sr::PixelFormat::float32, 8, 8);
            fillDepth(depth);
            drawPrepared(mesh, color, depth, sr::Rect<float>{0.0F, 0.0F, 8.0F, 8.0F},
                         fullScissor, blend, depthOn());
            const auto* pixel = pixelAt(color, 4, 4);
            TASK_CHECK(std::abs(static_cast<int>(pixel[0]) - 127) <= 1);
            TASK_CHECK(pixel[1] == 0 && pixel[2] == 0);
        }

        // scissor: only the right half is rasterized
        {
            const std::vector<sr::Vertex> vertices{
                makeVertex(-1.0F, -1.0F, 0.5F, 1.0F),
                makeVertex(1.0F, -1.0F, 0.5F, 1.0F),
                makeVertex(0.0F, 1.0F, 0.5F, 1.0F)
            };
            sr::PreparedMesh mesh(testVertexShader, identity, vertices,
                                  std::vector<std::size_t>{0, 1, 2});
            sr::Texture color(sr::PixelFormat::rgba8, 8, 8);
            sr::Texture depth(sr::PixelFormat::float32, 8, 8);
            fillDepth(depth);
            drawPrepared(mesh, color, depth, sr::Rect<float>{0.0F, 0.0F, 8.0F, 8.0F},
                         sr::Rect<float>{0.5F, 0.0F, 0.5F, 1.0F}, sr::BlendState{}, depthOn());
            TASK_CHECK(pixelAt(color, 2, 2)[3] == 0);
            TASK_CHECK(pixelAt(color, 5, 3)[0] == 255);
        }
        return 0;
    }

    int caseOldEntry()
    {
        // the original drawTriangles entry keeps working
        sr::Texture color(sr::PixelFormat::rgba8, 8, 8);
        sr::Texture depth(sr::PixelFormat::float32, 8, 8);
        fillDepth(depth);

        std::vector<sr::Vertex> vertices{
            makeVertex(-0.75F, -0.75F, 0.25F, 1.0F, sr::Color{1.0F, 0.0F, 0.0F, 1.0F}),
            makeVertex(0.5F, -0.75F, 0.25F, 1.0F, sr::Color{1.0F, 0.0F, 0.0F, 1.0F}),
            makeVertex(-0.75F, 0.5F, 0.25F, 1.0F, sr::Color{1.0F, 0.0F, 0.0F, 1.0F})
        };
        const std::vector<std::size_t> indices{0, 1, 2};
        const std::array<const sr::Sampler*, 2> samplers{};
        const std::array<const sr::Texture*, 2> textures{};
        sr::BlendState blend;

        sr::drawTriangles(color, depth, testVertexShader, testFragmentShader, samplers, textures,
                          sr::Rect<float>{0.0F, 0.0F, 8.0F, 8.0F}, fullScissor, blend, depthOn(),
                          indices, vertices, identity);
        TASK_CHECK(pixelAt(color, 2, 2)[0] == 255);
        TASK_CHECK(nearValue(depthAt(depth, 2, 2), 0.25, 1.0E-6));

        // a farther triangle does not overwrite
        for (auto& v : vertices)
        {
            v.position.v[2] = 0.75F;
            v.color = sr::Color{0.0F, 0.0F, 1.0F, 1.0F};
        }
        sr::drawTriangles(color, depth, testVertexShader, testFragmentShader, samplers, textures,
                          sr::Rect<float>{0.0F, 0.0F, 8.0F, 8.0F}, fullScissor, blend, depthOn(),
                          indices, vertices, identity);
        TASK_CHECK(pixelAt(color, 2, 2)[0] == 255 && pixelAt(color, 2, 2)[2] == 0);
        TASK_CHECK(nearValue(depthAt(depth, 2, 2), 0.25, 1.0E-6));
        return 0;
    }

    // -------------------------------------------------------------------- reuse

    int caseDrawAfterInputChange()
    {
        std::vector<sr::Vertex> vertices{
            makeVertex(-0.75F, -0.75F, 0.25F, 1.0F, sr::Color{1.0F, 0.0F, 0.0F, 1.0F}),
            makeVertex(0.5F, -0.75F, 0.25F, 1.0F, sr::Color{0.0F, 1.0F, 0.0F, 1.0F}),
            makeVertex(-0.75F, 0.5F, 0.25F, 1.0F, sr::Color{0.0F, 0.0F, 1.0F, 1.0F})
        };
        std::vector<std::size_t> indices{0, 1, 2};
        sr::Matrix<float, 4> transform = identity;
        sr::PreparedMesh mesh(testVertexShader, transform, vertices, indices);
        const auto viewPositions = flatViewPositions(vertices);
        const sr::Rect<float> viewport{0.0F, 0.0F, 8.0F, 8.0F};

        sr::Texture color1(sr::PixelFormat::rgba8, 8, 8);
        sr::Texture depth1(sr::PixelFormat::float32, 8, 8);
        fillDepth(depth1);
        drawPrepared(mesh, color1, depth1, viewport, fullScissor, sr::BlendState{}, depthOn());
        const auto segments1 = sr::visibleEdges(mesh, viewPositions, unitViewport, 0.5F);
        TASK_CHECK(!segments1.empty());

        // modify and release every input
        vertices.clear();
        vertices.shrink_to_fit();
        indices.clear();
        indices.shrink_to_fit();
        transform.m[0] = 42.0F;

        sr::Texture color2(sr::PixelFormat::rgba8, 8, 8);
        sr::Texture depth2(sr::PixelFormat::float32, 8, 8);
        fillDepth(depth2);
        drawPrepared(mesh, color2, depth2, viewport, fullScissor, sr::BlendState{}, depthOn());
        const auto segments2 = sr::visibleEdges(mesh, viewPositions, unitViewport, 0.5F);

        TASK_CHECK(color1.getData() == color2.getData());
        TASK_CHECK(depth1.getData() == depth2.getData());
        TASK_CHECK(sameSegments(segments1, segments2));
        return 0;
    }

    int caseTwoMeshesInterleaved()
    {
        const std::vector<sr::Vertex> verticesA{
            makeVertex(-0.9F, -0.9F, 0.3F, 1.0F, sr::Color{1.0F, 0.0F, 0.0F, 1.0F}),
            makeVertex(0.9F, -0.9F, 0.3F, 1.0F, sr::Color{1.0F, 0.0F, 0.0F, 1.0F}),
            makeVertex(0.0F, 0.9F, 0.3F, 1.0F, sr::Color{1.0F, 0.0F, 0.0F, 1.0F})
        };
        const std::vector<sr::Vertex> verticesB{
            makeVertex(-0.4F, -0.4F, 0.6F, 1.0F, sr::Color{0.0F, 1.0F, 0.0F, 1.0F}),
            makeVertex(0.4F, -0.4F, 0.6F, 1.0F, sr::Color{0.0F, 1.0F, 0.0F, 1.0F}),
            makeVertex(0.0F, 0.4F, 0.6F, 1.0F, sr::Color{0.0F, 1.0F, 0.0F, 1.0F})
        };
        sr::PreparedMesh meshA(testVertexShader, identity, verticesA, std::vector<std::size_t>{0, 1, 2});
        sr::PreparedMesh meshB(testVertexShader, identity, verticesB, std::vector<std::size_t>{0, 1, 2});
        const sr::Rect<float> viewport{0.0F, 0.0F, 8.0F, 8.0F};

        sr::Texture colorA1(sr::PixelFormat::rgba8, 8, 8), colorB1(sr::PixelFormat::rgba8, 8, 8);
        sr::Texture colorA2(sr::PixelFormat::rgba8, 8, 8), colorB2(sr::PixelFormat::rgba8, 8, 8);
        sr::Texture depthA1(sr::PixelFormat::float32, 8, 8), depthB1(sr::PixelFormat::float32, 8, 8);
        sr::Texture depthA2(sr::PixelFormat::float32, 8, 8), depthB2(sr::PixelFormat::float32, 8, 8);
        fillDepth(depthA1);
        fillDepth(depthB1);
        fillDepth(depthA2);
        fillDepth(depthB2);

        // interleave draws of the two prepared meshes
        drawPrepared(meshA, colorA1, depthA1, viewport, fullScissor, sr::BlendState{}, depthOn());
        drawPrepared(meshB, colorB1, depthB1, viewport, fullScissor, sr::BlendState{}, depthOn());
        drawPrepared(meshA, colorA2, depthA2, viewport, fullScissor, sr::BlendState{}, depthOn());
        drawPrepared(meshB, colorB2, depthB2, viewport, fullScissor, sr::BlendState{}, depthOn());

        TASK_CHECK(colorA1.getData() == colorA2.getData());
        TASK_CHECK(depthA1.getData() == depthA2.getData());
        TASK_CHECK(colorB1.getData() == colorB2.getData());
        TASK_CHECK(depthB1.getData() == depthB2.getData());
        TASK_CHECK(colorA1.getData() != colorB1.getData());

        // interleave edge extraction as well
        const auto viewA = flatViewPositions(verticesA);
        const auto viewB = flatViewPositions(verticesB);
        const auto segmentsA1 = sr::visibleEdges(meshA, viewA, unitViewport, 0.5F);
        const auto segmentsB = sr::visibleEdges(meshB, viewB, unitViewport, 0.5F);
        const auto segmentsA2 = sr::visibleEdges(meshA, viewA, unitViewport, 0.5F);
        TASK_CHECK(!segmentsA1.empty() && !segmentsB.empty());
        TASK_CHECK(sameSegments(segmentsA1, segmentsA2));
        TASK_CHECK(!sameSegments(segmentsA1, segmentsB));
        return 0;
    }

    int caseFailedPreparePreserves()
    {
        const std::vector<sr::Vertex> vertices{
            makeVertex(-0.75F, -0.75F, 0.25F, 1.0F),
            makeVertex(0.5F, -0.75F, 0.25F, 1.0F),
            makeVertex(-0.75F, 0.5F, 0.25F, 1.0F)
        };
        const std::vector<std::size_t> indices{0, 1, 2};
        sr::PreparedMesh mesh(testVertexShader, identity, vertices, indices);
        const auto viewPositions = flatViewPositions(vertices);
        const sr::Rect<float> viewport{0.0F, 0.0F, 8.0F, 8.0F};

        sr::Texture color1(sr::PixelFormat::rgba8, 8, 8);
        sr::Texture depth1(sr::PixelFormat::float32, 8, 8);
        fillDepth(depth1);
        drawPrepared(mesh, color1, depth1, viewport, fullScissor, sr::BlendState{}, depthOn());
        const auto segments1 = sr::visibleEdges(mesh, viewPositions, unitViewport, 0.5F);

        // a failed re-prepare keeps the previous content
        TASK_CHECK(throwsInvalidArgument([&] {
            mesh.prepare(testVertexShader, identity, vertices, std::vector<std::size_t>{0, 1, 99});
        }));
        // a failed construction throws as well
        TASK_CHECK(throwsInvalidArgument([&] {
            sr::PreparedMesh bad(testVertexShader, identity, vertices,
                                 std::vector<std::size_t>{0, 1, 99});
        }));

        sr::Texture color2(sr::PixelFormat::rgba8, 8, 8);
        sr::Texture depth2(sr::PixelFormat::float32, 8, 8);
        fillDepth(depth2);
        drawPrepared(mesh, color2, depth2, viewport, fullScissor, sr::BlendState{}, depthOn());
        const auto segments2 = sr::visibleEdges(mesh, viewPositions, unitViewport, 0.5F);

        TASK_CHECK(color1.getData() == color2.getData());
        TASK_CHECK(depth1.getData() == depth2.getData());
        TASK_CHECK(sameSegments(segments1, segments2));
        return 0;
    }

    int caseRepeatedDeterministic()
    {
        const std::vector<sr::Vertex> vertices{
            makeVertex(-0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.5F, -0.5F, 0.5F, 1.0F),
            makeVertex(0.0F, 0.5F, 0.5F, 1.0F)
        };
        sr::PreparedMesh mesh(testVertexShader, identity, vertices,
                              std::vector<std::size_t>{0, 1, 2});
        const auto viewPositions = flatViewPositions(vertices);

        // repeated extraction with the same viewport is identical
        const auto first = sr::visibleEdges(mesh, viewPositions, unitViewport, 0.5F);
        const auto second = sr::visibleEdges(mesh, viewPositions, unitViewport, 0.5F);
        TASK_CHECK(!first.empty());
        TASK_CHECK(sameSegments(first, second));

        // repeated drawing is identical
        sr::Texture color1(sr::PixelFormat::rgba8, 8, 8), color2(sr::PixelFormat::rgba8, 8, 8);
        sr::Texture depth1(sr::PixelFormat::float32, 8, 8), depth2(sr::PixelFormat::float32, 8, 8);
        fillDepth(depth1);
        fillDepth(depth2);
        const sr::Rect<float> viewport{0.0F, 0.0F, 8.0F, 8.0F};
        drawPrepared(mesh, color1, depth1, viewport, fullScissor, sr::BlendState{}, depthOn());
        drawPrepared(mesh, color2, depth2, viewport, fullScissor, sr::BlendState{}, depthOn());
        TASK_CHECK(color1.getData() == color2.getData());
        TASK_CHECK(depth1.getData() == depth2.getData());

        // a smaller offset viewport only touches its own region
        sr::Texture color3(sr::PixelFormat::rgba8, 8, 8);
        sr::Texture depth3(sr::PixelFormat::float32, 8, 8);
        fillDepth(depth3);
        drawPrepared(mesh, color3, depth3, sr::Rect<float>{4.0F, 4.0F, 4.0F, 4.0F},
                     fullScissor, sr::BlendState{}, depthOn());
        TASK_CHECK(color1.getData() != color3.getData());
        TASK_CHECK(pixelAt(color3, 0, 0)[3] == 0);
        return 0;
    }
}

const task_case task_cases[] = {
    {"geometry", "clip_identity", caseClipIdentity},
    {"geometry", "edges_adjacency", caseEdgesAdjacency},
    {"geometry", "invalid_inputs", caseInvalidInputs},
    {"geometry", "boundary_projection", caseBoundaryProjection},
    {"geometry", "edge_near_clip", caseEdgeNearClip},
    {"geometry", "classification", caseClassification},
    {"geometry", "viewport_switch", caseViewportSwitch},
    {"visibility", "occluder_middle", caseOccluderMiddle},
    {"visibility", "w_varying", caseWVarying},
    {"visibility", "depth_reversal", caseDepthReversal},
    {"visibility", "equal_depth", caseEqualDepth},
    {"visibility", "overlapping_occluders", caseOverlappingOccluders},
    {"visibility", "near_plane_both", caseNearPlaneBoth},
    {"visibility", "backface_occluder", caseBackfaceOccluder},
    {"visibility", "occluder_behind", caseOccluderBehind},
    {"visibility", "draw_clipping", caseDrawClipping},
    {"visibility", "draw_depth_attributes", caseDrawDepthAttributes},
    {"visibility", "draw_depth_blend_scissor", caseDrawDepthBlendScissor},
    {"visibility", "old_entry", caseOldEntry},
    {"reuse", "draw_after_input_change", caseDrawAfterInputChange},
    {"reuse", "two_meshes_interleaved", caseTwoMeshesInterleaved},
    {"reuse", "failed_prepare_preserves", caseFailedPreparePreserves},
    {"reuse", "repeated_deterministic", caseRepeatedDeterministic}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
