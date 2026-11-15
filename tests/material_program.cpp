#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "sr/sr.hpp"

namespace
{
    constexpr std::size_t kSize = 32;
    constexpr float kViewport = static_cast<float>(kSize);
    constexpr float kPixelTolerance = 1.0F / 255.0F + 2.0e-3F;

    // ---------------------------------------------------------------- helpers

    sr::VertexShaderOutput testVertexShader(const sr::Matrix<float, 4>& m, const sr::Vertex& v)
    {
        sr::VertexShaderOutput output{};
        output.position = m * v.position;
        output.color = v.color;
        output.texCoords[0] = v.texCoords[0];
        output.texCoords[1] = v.texCoords[1];
        output.normal = v.normal;
        return output;
    }

    // Reference vertex: clip-space position plus attributes.
    // attr = {u, v, r, g, b, a, unused, unused}
    struct RefVert
    {
        float clip[2];
        float w;
        float attr[8];
    };

    struct RefInterp
    {
        bool inside = false;
        float value[8]{};
        float dx[8]{};
        float dy[8]{};
    };

    // Independent scalar model of the rasterizer: barycentric coverage, the
    // perspective quotient clip_i = (bary_i / w_i) / S used for interpolation,
    // and the analytic screen-space derivatives of that quotient.
    RefInterp refEval(const RefVert verts[3], const float px, const float py)
    {
        RefInterp out;

        float vp[3][2];
        for (int k = 0; k < 3; ++k)
        {
            const float nx = verts[k].clip[0] / verts[k].w;
            const float ny = verts[k].clip[1] / verts[k].w;
            vp[k][0] = nx * kViewport / 2.0F + 0.0F + kViewport / 2.0F;
            vp[k][1] = ny * kViewport / 2.0F + 0.0F + kViewport / 2.0F;
        }

        const float v0x = vp[1][0] - vp[0][0];
        const float v0y = vp[1][1] - vp[0][1];
        const float v1x = vp[2][0] - vp[0][0];
        const float v1y = vp[2][1] - vp[0][1];
        const float den = v0x * v1y - v1x * v0y;

        const float v2x = px - vp[0][0];
        const float v2y = py - vp[0][1];
        const float bv = (v2x * v1y - v1x * v2y) / den;
        const float bw = (v0x * v2y - v2x * v0y) / den;

        if (!(bv >= 0.0F && bw >= 0.0F && bv + bw <= 1.0F))
            return out;

        const float bu = 1.0F - bv - bw;
        const float bary[3] = {bu, bv, bw};
        const float invW[3] = {1.0F / verts[0].w, 1.0F / verts[1].w, 1.0F / verts[2].w};

        // the rasterizer divides by w per component; mirror it exactly
        float clip[3] = {bary[0] / verts[0].w, bary[1] / verts[1].w, bary[2] / verts[2].w};
        const float clipSum = clip[0] + clip[1] + clip[2];
        for (int k = 0; k < 3; ++k)
            clip[k] /= clipSum;

        const float dbDx[3] = {(v0y - v1y) / den, v1y / den, -v0y / den};
        const float dbDy[3] = {(v1x - v0x) / den, -v1x / den, v0x / den};
        const float dSumDx = dbDx[0] * invW[0] + dbDx[1] * invW[1] + dbDx[2] * invW[2];
        const float dSumDy = dbDy[0] * invW[0] + dbDy[1] * invW[1] + dbDy[2] * invW[2];

        float dClipDx[3];
        float dClipDy[3];
        for (int k = 0; k < 3; ++k)
        {
            dClipDx[k] = (dbDx[k] * invW[k] - clip[k] * dSumDx) / clipSum;
            dClipDy[k] = (dbDy[k] * invW[k] - clip[k] * dSumDy) / clipSum;
        }

        out.inside = true;
        for (int c = 0; c < 8; ++c)
        {
            out.value[c] = verts[0].attr[c] * clip[0] + verts[1].attr[c] * clip[1] + verts[2].attr[c] * clip[2];
            out.dx[c] = verts[0].attr[c] * dClipDx[0] + verts[1].attr[c] * dClipDx[1] + verts[2].attr[c] * dClipDx[2];
            out.dy[c] = verts[0].attr[c] * dClipDy[0] + verts[1].attr[c] * dClipDy[1] + verts[2].attr[c] * dClipDy[2];
        }
        return out;
    }

    // Exact mean of the checker square wave s(t) = (-1)^floor(t) over
    // [t - h/2, t + h/2], computed independently by walking the integer
    // boundaries of the interval (piecewise integration).
    double refWaveMean(const double t, const double h)
    {
        if (h <= 0.0)
            return (std::fmod(std::floor(t), 2.0) == 0.0) ? 1.0 : -1.0;

        const double lo = t - 0.5 * h;
        const double hi = t + 0.5 * h;
        double total = 0.0;
        double pos = lo;
        while (pos < hi)
        {
            const double next = std::min(hi, std::floor(pos) + 1.0);
            const double s = (std::fmod(std::floor(pos), 2.0) == 0.0) ? 1.0 : -1.0;
            total += s * (next - pos);
            pos = next;
        }
        return total / (hi - lo);
    }

    struct TestTriangle
    {
        std::vector<sr::Vertex> vertices;
        RefVert ref[3];
    };

    // Triangle with unequal clip-space w per vertex (perspective interpolation).
    TestTriangle makeTestTriangle(const float u0, const float v0,
                                  const float u1, const float v1,
                                  const float u2, const float v2,
                                  const sr::Color c0 = sr::Color{1.0F, 1.0F, 1.0F, 1.0F},
                                  const sr::Color c1 = sr::Color{1.0F, 1.0F, 1.0F, 1.0F},
                                  const sr::Color c2 = sr::Color{1.0F, 1.0F, 1.0F, 1.0F})
    {
        const float ndc[3][2] = {{-0.85F, -0.75F}, {0.80F, -0.65F}, {-0.60F, 0.85F}};
        const float w[3] = {1.0F, 1.8F, 1.3F};
        const float uv[3][2] = {{u0, v0}, {u1, v1}, {u2, v2}};
        const sr::Color color[3] = {c0, c1, c2};

        TestTriangle triangle;
        for (int k = 0; k < 3; ++k)
        {
            const float clipX = ndc[k][0] * w[k];
            const float clipY = ndc[k][1] * w[k];

            sr::Vertex vertex{};
            vertex.position = sr::Vector<float, 4>{clipX, clipY, 0.5F * w[k], w[k]};
            vertex.color = color[k];
            vertex.texCoords[0] = sr::Vector<float, 2>{uv[k][0], uv[k][1]};
            vertex.texCoords[1] = vertex.texCoords[0];
            triangle.vertices.push_back(vertex);

            triangle.ref[k].clip[0] = clipX;
            triangle.ref[k].clip[1] = clipY;
            triangle.ref[k].w = w[k];
            triangle.ref[k].attr[0] = uv[k][0];
            triangle.ref[k].attr[1] = uv[k][1];
            triangle.ref[k].attr[2] = color[k].r;
            triangle.ref[k].attr[3] = color[k].g;
            triangle.ref[k].attr[4] = color[k].b;
            triangle.ref[k].attr[5] = color[k].a;
            triangle.ref[k].attr[6] = 0.0F;
            triangle.ref[k].attr[7] = 0.0F;
        }
        return triangle;
    }

    void makeTargets(sr::Texture& frameBuffer, sr::Texture& depthBuffer)
    {
        frameBuffer = sr::Texture{sr::PixelFormat::rgba8, kSize, kSize};
        depthBuffer = sr::Texture{sr::PixelFormat::float32, kSize, kSize};
        sr::clear(frameBuffer, sr::Color{0.0F, 0.0F, 0.0F, 1.0F});
        sr::clear(depthBuffer, 1.0F);
    }

    void drawWithProgram(sr::Texture& frameBuffer, sr::Texture& depthBuffer,
                         const sr::MaterialProgram& program,
                         const std::vector<sr::Vertex>& vertices)
    {
        sr::Matrix<float, 4> mvp;
        mvp.setIdentity();
        sr::drawTriangles(frameBuffer, depthBuffer, testVertexShader, program,
                          sr::Rect<float>{kViewport, kViewport},
                          sr::Rect<float>{1.0F, 1.0F},
                          sr::BlendState{}, sr::DepthState{},
                          {0, 1, 2}, vertices, mvp);
    }

    bool throwsMaterialError(void (*fn)())
    {
        try
        {
            fn();
        }
        catch (const sr::MaterialError&)
        {
            return true;
        }
        catch (...)
        {
        }
        return false;
    }

    // ------------------------------------------------------------- compiler

    int caseCompileReachableOnly()
    {
        sr::MaterialGraph graph;
        const auto uv = graph.texCoord(0);
        const auto scale = graph.constant(sr::Vector<float, 2>{3.0F, 5.0F});
        const auto scaled = graph.multiply(uv, scale);
        const auto ck = graph.checker(scaled);
        const auto one = graph.constant(1.0F);
        const auto unused = graph.constant(7.0F); // unreachable
        (void)unused;

        // unreachable garbage that must not fail the compile
        const auto badMismatch = graph.add(one, scale); // float + float2
        const auto badMissing = graph.add(one, sr::MaterialGraph::invalidNode);
        const auto cycA = graph.add(one, one);
        const auto cycB = graph.add(one, one);
        graph.setInput(cycA, 0, cycB);
        graph.setInput(cycB, 0, cycA); // unreachable cycle
        const auto badSample = graph.sample(scale, nullptr, sr::Sampler{}); // no texture
        const auto badUse = graph.checker(badMismatch);
        (void)badMissing;
        (void)badSample;
        (void)badUse;

        const auto root = graph.assemble({ck, ck, ck, one});

        const auto program = sr::compileMaterialGraph(graph, root);
        TASK_CHECK(program.isValid());
        TASK_CHECK(program.getInstructionCount() == 6); // uv, scale, scaled, ck, one, root
        TASK_CHECK(program.getTextureCount() == 0);

        sr::MaterialFragment fragment;
        fragment.texCoords[0] = sr::Vector<float, 2>{0.3F, 0.7F};
        const auto color = program.evaluate(fragment);
        // u = 0.9 -> s = +1, v = 3.5 -> s = -1 -> (1 - (+1)(-1)) / 2 = 1
        TASK_CHECK(std::fabs(color.r - 1.0F) < 1e-6F);
        TASK_CHECK(std::fabs(color.g - 1.0F) < 1e-6F);
        TASK_CHECK(std::fabs(color.b - 1.0F) < 1e-6F);
        TASK_CHECK(std::fabs(color.a - 1.0F) < 1e-6F);
        TASK_CHECK(program.getLastExecutionCount() == program.getInstructionCount());
        return 0;
    }

    int caseCompileSharedSubgraph()
    {
        sr::MaterialGraph graph;
        const auto uv = graph.texCoord(0);
        const auto k = graph.constant(sr::Vector<float, 2>{2.0F, 3.0F});
        const auto shared = graph.multiply(uv, k); // shared subexpression
        const auto sum = graph.add(shared, shared); // used twice here
        const auto prod = graph.multiply(sum, shared); // and once here
        const auto x = graph.component(prod, 0);
        const auto y = graph.component(prod, 1);
        const auto root = graph.assemble({x, y, graph.constant(0.25F), graph.constant(1.0F)});

        const auto program = sr::compileMaterialGraph(graph, root);
        // unique reachable nodes: uv, k, shared, sum, prod, x, y, c0.25, c1, root
        TASK_CHECK(program.getInstructionCount() == 10);

        sr::MaterialFragment fragment;
        fragment.texCoords[0] = sr::Vector<float, 2>{0.5F, 0.25F};
        const auto color = program.evaluate(fragment);
        // shared = (1.0, 0.75); sum = (2.0, 1.5); prod = (2.0, 1.125)
        TASK_CHECK(std::fabs(color.r - 2.0F) < 1e-6F);
        TASK_CHECK(std::fabs(color.g - 1.125F) < 1e-6F);
        TASK_CHECK(std::fabs(color.b - 0.25F) < 1e-6F);
        TASK_CHECK(std::fabs(color.a - 1.0F) < 1e-6F);
        // each instruction ran exactly once although 'shared' is referenced 3 times
        TASK_CHECK(program.getLastExecutionCount() == 10);
        return 0;
    }

    int caseCompileInvalidGraphs()
    {
        // root must produce a float4
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            (void)sr::compileMaterialGraph(g, g.constant(1.0F));
        }));
        // invalid root ids
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            (void)sr::compileMaterialGraph(g, sr::MaterialGraph::invalidNode);
        }));
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            g.constant(1.0F);
            (void)sr::compileMaterialGraph(g, 12345);
        }));
        // reachable type mismatch
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            const auto bad = g.add(g.constant(1.0F), g.constant(sr::Vector<float, 2>{1.0F, 2.0F}));
            (void)sr::compileMaterialGraph(g, g.assemble({bad, bad, bad, bad}));
        }));
        // missing input
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            const auto bad = g.multiply(g.constant(1.0F), sr::MaterialGraph::invalidNode);
            (void)sr::compileMaterialGraph(g, g.assemble({bad, bad, bad, bad}));
        }));
        // reachable cycle
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            const auto one = g.constant(1.0F);
            const auto a = g.add(one, one);
            const auto b = g.add(a, one);
            g.setInput(a, 0, b); // a depends on b which depends on a
            (void)sr::compileMaterialGraph(g, g.assemble({a, a, a, a}));
        }));
        // component index out of range
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            const auto v2 = g.constant(sr::Vector<float, 2>{1.0F, 2.0F});
            const auto bad = g.component(v2, 2);
            (void)sr::compileMaterialGraph(g, g.assemble({bad, bad, bad, bad}));
        }));
        // component extraction from a scalar
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            const auto bad = g.component(g.constant(1.0F), 0);
            (void)sr::compileMaterialGraph(g, g.assemble({bad, bad, bad, bad}));
        }));
        // checker requires a float2
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            const auto bad = g.checker(g.constant(1.0F));
            (void)sr::compileMaterialGraph(g, g.assemble({bad, bad, bad, bad}));
        }));
        // sample requires a float2 coordinate
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            sr::Texture texture(sr::PixelFormat::rgba8, 2, 2);
            const auto bad = g.sample(g.constant(sr::Vector<float, 4>{0.0F, 0.0F, 0.0F, 0.0F}),
                                      &texture, sr::Sampler{});
            (void)sr::compileMaterialGraph(g, bad);
        }));
        // null texture reference
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            const auto uv = g.constant(sr::Vector<float, 2>{0.0F, 0.0F});
            (void)sr::compileMaterialGraph(g, g.sample(uv, nullptr, sr::Sampler{}));
        }));
        // empty texture reference
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            sr::Texture empty;
            const auto uv = g.constant(sr::Vector<float, 2>{0.0F, 0.0F});
            (void)sr::compileMaterialGraph(g, g.sample(uv, &empty, sr::Sampler{}));
        }));
        // assemble with a component total that is neither 2 nor 4
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph g;
            const auto one = g.constant(1.0F);
            (void)sr::compileMaterialGraph(g, g.assemble({one, one, one}));
        }));
        return 0;
    }

    // -------------------------------------------------------- differentials

    int caseCheckerMeanPiecewise()
    {
        sr::MaterialGraph graph;
        const auto uv = graph.texCoord(0);
        const auto ck = graph.checker(uv);
        const auto one = graph.constant(1.0F);
        const auto root = graph.assemble({ck, ck, ck, one});
        const auto program = sr::compileMaterialGraph(graph, root);
        TASK_CHECK(program.getInstructionCount() == 4);

        struct Case
        {
            float u;
            float a;
        };
        const Case cases[] = {
            {0.3F, 0.0F}, {1.2F, 0.0F}, {-0.5F, 0.0F}, {-1.5F, 0.0F}, {-2.0F, 0.0F},
            {0.5F, 2.0F}, {0.5F, 4.0F}, {0.25F, 1.0F}, {-0.25F, 1.0F},
            {10.3F, 7.0F}, {-7.7F, 13.0F}, {3.75F, 0.4F}, {-13.2F, 31.0F}, {123.4F, 55.0F}
        };

        for (const auto& c : cases)
        {
            sr::MaterialFragment fragment;
            // v = 0.25 with zero footprint -> M(v, 0) = s(0.25) = 1, so the
            // output isolates M(u, a): checker = (1 - M(u, a)) / 2
            fragment.texCoords[0] = sr::Vector<float, 2>{c.u, 0.25F};
            fragment.texCoordsDx[0] = sr::Vector<float, 2>{c.a, 0.0F};
            fragment.texCoordsDy[0] = sr::Vector<float, 2>{0.0F, 0.0F};
            const auto color = program.evaluate(fragment);
            const double expected = (1.0 - refWaveMean(c.u, c.a)) / 2.0;
            TASK_CHECK(std::fabs(color.r - expected) < 1e-5);
            TASK_CHECK(std::fabs(color.g - expected) < 1e-5);
            TASK_CHECK(std::fabs(color.b - expected) < 1e-5);
            TASK_CHECK(program.getLastExecutionCount() == program.getInstructionCount());
        }
        return 0;
    }

    int caseUvQuotientDraw()
    {
        auto tri = makeTestTriangle(0.1F, 0.2F, 0.9F, 0.3F, 0.2F, 0.95F);

        sr::MaterialGraph graph;
        const auto uv = graph.texCoord(0);
        const auto u = graph.component(uv, 0);
        const auto v = graph.component(uv, 1);
        const auto root = graph.assemble({u, v, graph.constant(0.5F), graph.constant(1.0F)});
        const auto program = sr::compileMaterialGraph(graph, root);

        sr::Texture frameBuffer, depthBuffer;
        makeTargets(frameBuffer, depthBuffer);
        drawWithProgram(frameBuffer, depthBuffer, program, tri.vertices);

        std::size_t covered = 0;
        for (std::size_t y = 0; y < kSize; ++y)
            for (std::size_t x = 0; x < kSize; ++x)
            {
                const auto ref = refEval(tri.ref, static_cast<float>(x), static_cast<float>(y));
                const auto pixel = frameBuffer.getPixel(x, y, 0);
                if (!ref.inside)
                {
                    TASK_CHECK(std::fabs(pixel.r - 0.0F) < 1e-6F); // clear color
                    continue;
                }
                ++covered;
                TASK_CHECK(std::fabs(pixel.r - ref.value[0]) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.g - ref.value[1]) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.b - 0.5F) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.a - 1.0F) <= kPixelTolerance);
            }
        TASK_CHECK(covered > 100);
        TASK_CHECK(program.getLastExecutionCount() == program.getInstructionCount());
        return 0;
    }

    int caseCheckerDrawQuotient()
    {
        auto tri = makeTestTriangle(0.0F, 0.0F, 1.6F, 0.2F, 0.3F, 1.9F);
        const float freq = 6.0F;

        sr::MaterialGraph graph;
        const auto uv = graph.texCoord(0);
        const auto ck = graph.checker(graph.multiply(uv, graph.constant(sr::Vector<float, 2>{freq, freq})));
        const auto root = graph.assemble({ck, ck, ck, graph.constant(1.0F)});
        const auto program = sr::compileMaterialGraph(graph, root);

        sr::Texture frameBuffer, depthBuffer;
        makeTargets(frameBuffer, depthBuffer);
        drawWithProgram(frameBuffer, depthBuffer, program, tri.vertices);

        std::size_t covered = 0;
        for (std::size_t y = 0; y < kSize; ++y)
            for (std::size_t x = 0; x < kSize; ++x)
            {
                const auto ref = refEval(tri.ref, static_cast<float>(x), static_cast<float>(y));
                if (!ref.inside) continue;
                ++covered;
                const double cu = freq * ref.value[0];
                const double cv = freq * ref.value[1];
                const double a = std::fabs(freq * ref.dx[0]) + std::fabs(freq * ref.dy[0]);
                const double b = std::fabs(freq * ref.dx[1]) + std::fabs(freq * ref.dy[1]);
                const double expected = (1.0 - refWaveMean(cu, a) * refWaveMean(cv, b)) / 2.0;
                const auto pixel = frameBuffer.getPixel(x, y, 0);
                TASK_CHECK(std::fabs(pixel.r - expected) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.g - expected) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.b - expected) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.a - 1.0F) <= kPixelTolerance);
            }
        TASK_CHECK(covered > 100);
        return 0;
    }

    int caseSinDistortionWideFootprint()
    {
        auto tri = makeTestTriangle(0.0F, 0.0F, 1.4F, 0.1F, 0.2F, 1.6F);
        const float inner = 40.0F; // fast oscillation -> wide screen-space footprint
        const float outer = 3.0F;

        sr::MaterialGraph graph;
        const auto uv = graph.texCoord(0);
        const auto s = graph.sine(graph.multiply(uv, graph.constant(sr::Vector<float, 2>{inner, inner})));
        const auto coords = graph.multiply(s, graph.constant(sr::Vector<float, 2>{outer, outer}));
        const auto ck = graph.checker(coords);
        const auto root = graph.assemble({ck, ck, ck, graph.constant(1.0F)});
        const auto program = sr::compileMaterialGraph(graph, root);

        sr::Texture frameBuffer, depthBuffer;
        makeTargets(frameBuffer, depthBuffer);
        drawWithProgram(frameBuffer, depthBuffer, program, tri.vertices);

        std::size_t covered = 0;
        double maxFootprint = 0.0;
        for (std::size_t y = 0; y < kSize; ++y)
            for (std::size_t x = 0; x < kSize; ++x)
            {
                const auto ref = refEval(tri.ref, static_cast<float>(x), static_cast<float>(y));
                if (!ref.inside) continue;
                ++covered;
                // independent scalar evaluation of checker(3 * sin(40 * uv))
                const double cu = inner * ref.value[0];
                const double cv = inner * ref.value[1];
                const double su = outer * std::sin(cu);
                const double sv = outer * std::sin(cv);
                const double dsudx = outer * (std::cos(cu) * (inner * ref.dx[0]));
                const double dsudy = outer * (std::cos(cu) * (inner * ref.dy[0]));
                const double dsvdx = outer * (std::cos(cv) * (inner * ref.dx[1]));
                const double dsvdy = outer * (std::cos(cv) * (inner * ref.dy[1]));
                const double a = std::fabs(dsudx) + std::fabs(dsudy);
                const double b = std::fabs(dsvdx) + std::fabs(dsvdy);
                maxFootprint = std::max(maxFootprint, std::max(a, b));
                const double expected = (1.0 - refWaveMean(su, a) * refWaveMean(sv, b)) / 2.0;
                const auto pixel = frameBuffer.getPixel(x, y, 0);
                TASK_CHECK(std::fabs(pixel.r - expected) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.g - expected) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.b - expected) <= kPixelTolerance);
            }
        TASK_CHECK(covered > 100);
        // the sin domain distortion must actually produce footprints that
        // average the square wave over multiple periods
        TASK_CHECK(maxFootprint > 4.0);
        return 0;
    }

    int caseNegativeCoordinates()
    {
        auto tri = makeTestTriangle(-1.5F, -0.5F, 1.0F, -1.2F, -0.8F, 2.0F);
        const float freq = 4.0F;

        sr::MaterialGraph graph;
        const auto uv = graph.texCoord(0);
        const auto ck = graph.checker(graph.multiply(uv, graph.constant(sr::Vector<float, 2>{freq, freq})));
        const auto root = graph.assemble({ck, ck, ck, graph.constant(1.0F)});
        const auto program = sr::compileMaterialGraph(graph, root);

        sr::Texture frameBuffer, depthBuffer;
        makeTargets(frameBuffer, depthBuffer);
        drawWithProgram(frameBuffer, depthBuffer, program, tri.vertices);

        std::size_t covered = 0;
        bool sawNegative = false;
        for (std::size_t y = 0; y < kSize; ++y)
            for (std::size_t x = 0; x < kSize; ++x)
            {
                const auto ref = refEval(tri.ref, static_cast<float>(x), static_cast<float>(y));
                if (!ref.inside) continue;
                ++covered;
                if (ref.value[0] < 0.0F || ref.value[1] < 0.0F) sawNegative = true;
                const double cu = freq * ref.value[0];
                const double cv = freq * ref.value[1];
                const double a = std::fabs(freq * ref.dx[0]) + std::fabs(freq * ref.dy[0]);
                const double b = std::fabs(freq * ref.dx[1]) + std::fabs(freq * ref.dy[1]);
                const double expected = (1.0 - refWaveMean(cu, a) * refWaveMean(cv, b)) / 2.0;
                const auto pixel = frameBuffer.getPixel(x, y, 0);
                TASK_CHECK(std::fabs(pixel.r - expected) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.g - expected) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.b - expected) <= kPixelTolerance);
            }
        TASK_CHECK(covered > 100);
        TASK_CHECK(sawNegative);
        return 0;
    }

    int caseVertexColorDerivatives()
    {
        auto tri = makeTestTriangle(0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, // uvs unused
                                    sr::Color{0.0F, 0.0F, 0.5F, 1.0F},
                                    sr::Color{1.5F, 0.3F, 0.2F, 1.0F},
                                    sr::Color{0.2F, 1.7F, 0.9F, 1.0F});
        const float freq = 5.0F;

        sr::MaterialGraph graph;
        const auto vc = graph.vertexColor();
        const auto r = graph.component(vc, 0);
        const auto g = graph.component(vc, 1);
        const auto coords = graph.assemble({r, g});
        const auto ck = graph.checker(graph.multiply(coords, graph.constant(sr::Vector<float, 2>{freq, freq})));
        const auto root = graph.assemble({ck, ck, ck, graph.constant(1.0F)});
        const auto program = sr::compileMaterialGraph(graph, root);

        sr::Texture frameBuffer, depthBuffer;
        makeTargets(frameBuffer, depthBuffer);
        drawWithProgram(frameBuffer, depthBuffer, program, tri.vertices);

        std::size_t covered = 0;
        for (std::size_t y = 0; y < kSize; ++y)
            for (std::size_t x = 0; x < kSize; ++x)
            {
                const auto ref = refEval(tri.ref, static_cast<float>(x), static_cast<float>(y));
                if (!ref.inside) continue;
                ++covered;
                const double cu = freq * ref.value[2]; // red channel drives u
                const double cv = freq * ref.value[3]; // green channel drives v
                const double a = std::fabs(freq * ref.dx[2]) + std::fabs(freq * ref.dy[2]);
                const double b = std::fabs(freq * ref.dx[3]) + std::fabs(freq * ref.dy[3]);
                const double expected = (1.0 - refWaveMean(cu, a) * refWaveMean(cv, b)) / 2.0;
                const auto pixel = frameBuffer.getPixel(x, y, 0);
                TASK_CHECK(std::fabs(pixel.r - expected) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.g - expected) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.b - expected) <= kPixelTolerance);
            }
        TASK_CHECK(covered > 100);
        return 0;
    }

    int caseDerivativeFiniteDifference()
    {
        auto tri = makeTestTriangle(0.1F, 0.2F, 0.9F, 0.3F, 0.2F, 0.95F);

        // the analytic derivatives of the perspective quotient must match the
        // central finite difference of the interpolated attribute
        std::size_t tested = 0;
        for (std::size_t y = 1; y + 1 < kSize; ++y)
            for (std::size_t x = 1; x + 1 < kSize; ++x)
            {
                const auto c = refEval(tri.ref, static_cast<float>(x), static_cast<float>(y));
                const auto px = refEval(tri.ref, static_cast<float>(x + 1), static_cast<float>(y));
                const auto mx = refEval(tri.ref, static_cast<float>(x - 1), static_cast<float>(y));
                const auto py = refEval(tri.ref, static_cast<float>(x), static_cast<float>(y + 1));
                const auto my = refEval(tri.ref, static_cast<float>(x), static_cast<float>(y - 1));
                if (!c.inside || !px.inside || !mx.inside || !py.inside || !my.inside) continue;
                ++tested;
                for (int attr = 0; attr < 2; ++attr)
                {
                    const double numDx = (px.value[attr] - mx.value[attr]) / 2.0;
                    const double numDy = (py.value[attr] - my.value[attr]) / 2.0;
                    TASK_CHECK(std::fabs(numDx - c.dx[attr]) <= 0.02 * std::fabs(c.dx[attr]) + 1e-3);
                    TASK_CHECK(std::fabs(numDy - c.dy[attr]) <= 0.02 * std::fabs(c.dy[attr]) + 1e-3);
                }
            }
        TASK_CHECK(tested > 50);
        return 0;
    }

    // ------------------------------------------------------------- lifetime

    int caseGraphEditAndDestroy()
    {
        auto tri = makeTestTriangle(0.0F, 0.0F, 1.2F, 0.1F, 0.2F, 1.4F);
        const float freq = 5.0F;

        sr::Texture fb1, db1;
        makeTargets(fb1, db1);

        sr::MaterialProgram program;
        {
            sr::MaterialGraph graph;
            const auto uv = graph.texCoord(0);
            const auto scale = graph.constant(sr::Vector<float, 2>{freq, freq});
            const auto ck = graph.checker(graph.multiply(uv, scale));
            const auto one = graph.constant(1.0F);
            const auto root = graph.assemble({ck, ck, ck, one});
            program = sr::compileMaterialGraph(graph, root);

            drawWithProgram(fb1, db1, program, tri.vertices);

            // edit the graph after the compile: change the constant, rewire the
            // root and add new nodes; then the graph is destroyed
            graph.setConstant(scale, sr::Vector<float, 2>{9.0F, 9.0F});
            graph.setInput(root, 0, one);
            graph.checker(graph.constant(sr::Vector<float, 2>{1.0F, 2.0F}));
        }

        sr::Texture fb2, db2;
        makeTargets(fb2, db2);
        drawWithProgram(fb2, db2, program, tri.vertices);

        // the compiled program must be unaffected by the edits and destruction
        TASK_CHECK(fb1.getData() == fb2.getData());

        // and it must still produce the original 5x checkerboard
        std::size_t covered = 0;
        for (std::size_t y = 0; y < kSize; ++y)
            for (std::size_t x = 0; x < kSize; ++x)
            {
                const auto ref = refEval(tri.ref, static_cast<float>(x), static_cast<float>(y));
                if (!ref.inside) continue;
                ++covered;
                const double cu = freq * ref.value[0];
                const double cv = freq * ref.value[1];
                const double a = std::fabs(freq * ref.dx[0]) + std::fabs(freq * ref.dy[0]);
                const double b = std::fabs(freq * ref.dx[1]) + std::fabs(freq * ref.dy[1]);
                const double expected = (1.0 - refWaveMean(cu, a) * refWaveMean(cv, b)) / 2.0;
                const auto pixel = fb2.getPixel(x, y, 0);
                TASK_CHECK(std::fabs(pixel.r - expected) <= kPixelTolerance);
            }
        TASK_CHECK(covered > 100);
        return 0;
    }

    int caseTextureSnapshot()
    {
        const std::vector<std::uint8_t> original{
            250, 10, 20, 255, 30, 240, 40, 255,
            50, 60, 230, 255, 70, 80, 90, 255
        };
        const std::vector<std::uint8_t> rewritten{
            1, 2, 3, 255, 4, 5, 6, 255,
            7, 8, 9, 255, 10, 11, 12, 255
        };

        sr::Sampler pointSampler;
        sr::Sampler linearSampler;
        linearSampler.filter = sr::Sampler::Filter::linear;

        sr::Texture reference(sr::PixelFormat::rgba8, 2, 2);
        reference.setData(original);

        sr::MaterialProgram program;
        {
            sr::Texture texture(sr::PixelFormat::rgba8, 2, 2);
            texture.setData(original);

            sr::MaterialGraph graph;
            const auto uv = graph.texCoord(0);
            const auto s1 = graph.sample(uv, &texture, pointSampler);
            const auto s2 = graph.sample(uv, &texture, linearSampler); // same texture again
            const auto root = graph.multiply(s1, s2);
            program = sr::compileMaterialGraph(graph, root);

            // two references to the same source texture share one snapshot
            TASK_CHECK(program.getTextureCount() == 1);

            // rewrite, resize and finally destroy the source texture
            texture.setData(rewritten);
            texture.resize(8, 8);
        }

        // flat uv (0.5, 0.5) over the whole triangle
        auto tri = makeTestTriangle(0.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F);

        sr::Texture fb1, db1;
        makeTargets(fb1, db1);
        drawWithProgram(fb1, db1, program, tri.vertices);

        // expected values come from the original texture data, sampled at the
        // same interpolated coordinates as the real draw
        std::size_t covered = 0;
        for (std::size_t y = 0; y < kSize; ++y)
            for (std::size_t x = 0; x < kSize; ++x)
            {
                const auto ref = refEval(tri.ref, static_cast<float>(x), static_cast<float>(y));
                if (!ref.inside) continue;
                ++covered;
                const sr::Vector<float, 2> uv{ref.value[0], ref.value[1]};
                const auto e1 = reference.sample(&pointSampler, uv);
                const auto e2 = reference.sample(&linearSampler, uv);
                const auto pixel = fb1.getPixel(x, y, 0);
                TASK_CHECK(std::fabs(pixel.r - e1.r * e2.r) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.g - e1.g * e2.g) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.b - e1.b * e2.b) <= kPixelTolerance);
                TASK_CHECK(std::fabs(pixel.a - e1.a * e2.a) <= kPixelTolerance);
            }
        TASK_CHECK(covered > 100);

        // drawing again after the source texture is gone gives the same image
        sr::Texture fb2, db2;
        makeTargets(fb2, db2);
        drawWithProgram(fb2, db2, program, tri.vertices);
        TASK_CHECK(fb1.getData() == fb2.getData());
        return 0;
    }

    int caseTwoProgramsInterleaved()
    {
        auto tri = makeTestTriangle(0.0F, 0.0F, 1.3F, 0.2F, 0.1F, 1.5F);

        sr::MaterialProgram programA;
        {
            sr::MaterialGraph g;
            const auto ck = g.checker(g.multiply(g.texCoord(0), g.constant(sr::Vector<float, 2>{4.0F, 4.0F})));
            const auto zero = g.constant(0.0F);
            programA = sr::compileMaterialGraph(g, g.assemble({ck, zero, zero, g.constant(1.0F)}));
        }
        sr::MaterialProgram programB;
        {
            // compiled after A; A must remain fully usable
            sr::MaterialGraph g;
            const auto ck = g.checker(g.multiply(g.texCoord(0), g.constant(sr::Vector<float, 2>{7.0F, 7.0F})));
            const auto zero = g.constant(0.0F);
            programB = sr::compileMaterialGraph(g, g.assemble({zero, ck, zero, g.constant(1.0F)}));
        }

        sr::Texture a1, ad1;
        sr::Texture b1, bd1;
        sr::Texture a2, ad2;
        sr::Texture b2, bd2;
        makeTargets(a1, ad1);
        makeTargets(b1, bd1);
        makeTargets(a2, ad2);
        makeTargets(b2, bd2);

        // interleave draws of both programs
        drawWithProgram(a1, ad1, programA, tri.vertices);
        drawWithProgram(b1, bd1, programB, tri.vertices);
        drawWithProgram(a2, ad2, programA, tri.vertices);
        drawWithProgram(b2, bd2, programB, tri.vertices);

        TASK_CHECK(a1.getData() == a2.getData());
        TASK_CHECK(b1.getData() == b2.getData());
        TASK_CHECK(a1.getData() != b1.getData()); // different frequencies
        TASK_CHECK(programA.getLastExecutionCount() == programA.getInstructionCount());
        TASK_CHECK(programB.getLastExecutionCount() == programB.getInstructionCount());
        return 0;
    }

    int caseFailedCompileKeepsProgram()
    {
        auto tri = makeTestTriangle(0.0F, 0.0F, 1.3F, 0.2F, 0.1F, 1.5F);

        sr::MaterialProgram program;
        {
            sr::MaterialGraph g;
            const auto ck = g.checker(g.multiply(g.texCoord(0), g.constant(sr::Vector<float, 2>{5.0F, 5.0F})));
            program = sr::compileMaterialGraph(g, g.assemble({ck, ck, ck, g.constant(1.0F)}));
        }

        sr::Texture fb1, db1;
        makeTargets(fb1, db1);
        drawWithProgram(fb1, db1, program, tri.vertices);

        // failed compiles must not damage the previously compiled program
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph bad;
            const auto one = bad.constant(1.0F);
            const auto a = bad.add(one, one);
            const auto b = bad.add(a, one);
            bad.setInput(a, 0, b); // cycle
            (void)sr::compileMaterialGraph(bad, bad.assemble({a, a, a, a}));
        }));
        TASK_CHECK(throwsMaterialError([] {
            sr::MaterialGraph bad;
            (void)sr::compileMaterialGraph(bad, bad.constant(1.0F)); // root not float4
        }));

        sr::Texture fb2, db2;
        makeTargets(fb2, db2);
        drawWithProgram(fb2, db2, program, tri.vertices);
        TASK_CHECK(fb1.getData() == fb2.getData());
        return 0;
    }

    sr::Color parityFragmentShader(const sr::VertexShaderOutput& input,
                                   const std::array<const sr::Sampler*, 2>& samplers,
                                   const std::array<const sr::Texture*, 2>& textures)
    {
        const auto sampled = textures[0]->sample(samplers[0], input.texCoords[0]);
        return sr::Color{sampled.r * input.color.r,
                         sampled.g * input.color.g,
                         sampled.b * input.color.b,
                         sampled.a * input.color.a};
    }

    int caseOldShaderParity()
    {
        sr::Texture texture(sr::PixelFormat::rgba8, 4, 4);
        std::vector<std::uint8_t> data(4 * 4 * 4);
        for (std::size_t y = 0; y < 4; ++y)
            for (std::size_t x = 0; x < 4; ++x)
            {
                auto* px = &data[(y * 4 + x) * 4];
                px[0] = static_cast<std::uint8_t>(30 + x * 60 + y * 10);
                px[1] = static_cast<std::uint8_t>(20 + y * 55 + x * 7);
                px[2] = static_cast<std::uint8_t>(200 - x * 30 - y * 20);
                px[3] = 255;
            }
        texture.setData(data);

        sr::Sampler sampler;
        sampler.filter = sr::Sampler::Filter::linear;

        const float ndc[4][2] = {{-0.8F, -0.7F}, {0.7F, -0.8F}, {0.8F, 0.7F}, {-0.7F, 0.8F}};
        const float w[4] = {1.0F, 1.6F, 1.2F, 2.0F};
        const float uv[4][2] = {{0.1F, 0.1F}, {0.9F, 0.2F}, {0.8F, 0.9F}, {0.2F, 0.8F}};
        const sr::Color colors[4] = {
            {1.0F, 0.7F, 0.4F, 0.9F},
            {0.5F, 1.0F, 0.8F, 0.7F},
            {0.9F, 0.4F, 1.0F, 1.0F},
            {1.0F, 1.0F, 0.6F, 0.8F}
        };

        std::vector<sr::Vertex> vertices;
        for (int k = 0; k < 4; ++k)
        {
            sr::Vertex vertex{};
            vertex.position = sr::Vector<float, 4>{ndc[k][0] * w[k], ndc[k][1] * w[k], 0.5F * w[k], w[k]};
            vertex.color = colors[k];
            vertex.texCoords[0] = sr::Vector<float, 2>{uv[k][0], uv[k][1]};
            vertices.push_back(vertex);
        }
        const std::vector<std::size_t> indices{0, 1, 2, 0, 2, 3};

        sr::Matrix<float, 4> mvp;
        mvp.setIdentity();

        sr::BlendState blend;
        blend.enabled = true;
        blend.colorBlendSource = sr::BlendState::Factor::srcAlpha;
        blend.colorBlendDest = sr::BlendState::Factor::invSrcAlpha;
        blend.alphaBlendSource = sr::BlendState::Factor::srcAlpha;
        blend.alphaBlendDest = sr::BlendState::Factor::invSrcAlpha;

        sr::DepthState depthState;
        depthState.read = true;
        depthState.write = true;

        const sr::Color clearColor{0.1F, 0.2F, 0.3F, 1.0F};

        sr::Texture fbOld(sr::PixelFormat::rgba8, kSize, kSize);
        sr::Texture dbOld(sr::PixelFormat::float32, kSize, kSize);
        sr::clear(fbOld, clearColor);
        sr::clear(dbOld, 1.0F);

        // old entry point: function pointer shader
        sr::drawTriangles(fbOld, dbOld, testVertexShader, parityFragmentShader,
                          {&sampler, nullptr}, {&texture, nullptr},
                          sr::Rect<float>{kViewport, kViewport},
                          sr::Rect<float>{1.0F, 1.0F},
                          blend, depthState, indices, vertices, mvp);

        // new entry point: compiled material program with the same meaning
        sr::MaterialGraph graph;
        const auto root = graph.multiply(graph.sample(graph.texCoord(0), &texture, sampler),
                                         graph.vertexColor());
        const auto program = sr::compileMaterialGraph(graph, root);
        TASK_CHECK(program.getTextureCount() == 1);

        sr::Texture fbNew(sr::PixelFormat::rgba8, kSize, kSize);
        sr::Texture dbNew(sr::PixelFormat::float32, kSize, kSize);
        sr::clear(fbNew, clearColor);
        sr::clear(dbNew, 1.0F);

        sr::drawTriangles(fbNew, dbNew, testVertexShader, program,
                          sr::Rect<float>{kViewport, kViewport},
                          sr::Rect<float>{1.0F, 1.0F},
                          blend, depthState, indices, vertices, mvp);

        // identical results through the shared raster loop, depth and blending
        TASK_CHECK(fbOld.getData() == fbNew.getData());
        TASK_CHECK(dbOld.getData() == dbNew.getData());

        sr::Texture fbClear(sr::PixelFormat::rgba8, kSize, kSize);
        sr::clear(fbClear, clearColor);
        TASK_CHECK(fbOld.getData() != fbClear.getData()); // something was drawn
        return 0;
    }
}

const task_case task_cases[] = {
    {"compiler", "reachable_only_and_counts", caseCompileReachableOnly},
    {"compiler", "shared_subgraph_evaluated_once", caseCompileSharedSubgraph},
    {"compiler", "invalid_graphs_rejected", caseCompileInvalidGraphs},
    {"differentials", "checker_mean_piecewise", caseCheckerMeanPiecewise},
    {"differentials", "uv_quotient_draw", caseUvQuotientDraw},
    {"differentials", "checker_draw_quotient", caseCheckerDrawQuotient},
    {"differentials", "sin_distortion_wide_footprint", caseSinDistortionWideFootprint},
    {"differentials", "negative_coordinates", caseNegativeCoordinates},
    {"differentials", "vertex_color_derivatives", caseVertexColorDerivatives},
    {"differentials", "derivative_finite_difference", caseDerivativeFiniteDifference},
    {"lifetime", "graph_edit_and_destroy", caseGraphEditAndDestroy},
    {"lifetime", "texture_snapshot_rewrite", caseTextureSnapshot},
    {"lifetime", "two_programs_interleaved", caseTwoProgramsInterleaved},
    {"lifetime", "failed_compile_keeps_program", caseFailedCompileKeepsProgram},
    {"lifetime", "old_shader_parity", caseOldShaderParity}
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
