//
//  Mip render target regression tests.
//
//  Windowless numeric checks for: per-level storage and mip chain generation,
//  LOD-aware texture sampling, and rendering into individual mip levels
//  (including feedback draws where a target is also an input).
//

#include "sr/sr.hpp"
#include "task_support/check.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    using sr::Color;
    using sr::DepthState;
    using sr::BlendState;
    using sr::PixelFormat;
    using sr::Rect;
    using sr::RenderError;
    using sr::Sampler;
    using sr::Texture;
    using sr::TextureView;
    using sr::Vertex;
    using sr::VertexShaderOutput;

    constexpr float kEps = 1e-5F;

    bool nearFloat(const float a, const float b, const float eps = kEps)
    {
        return std::fabs(a - b) <= eps;
    }

    // Independent reference implementation of the level dimension rules.
    std::size_t refDim(const std::size_t base, const std::uint32_t level)
    {
        return std::max<std::size_t>(1, base >> level);
    }

    // Reference box downsample for byte formats (r8, a8, rgba8).
    std::vector<std::uint8_t> refDownsampleBytes(const std::vector<std::uint8_t>& parent,
                                                 const std::size_t parentW,
                                                 const std::size_t parentH,
                                                 const std::size_t childW,
                                                 const std::size_t childH,
                                                 const std::size_t pixelSize)
    {
        std::vector<std::uint8_t> child(childW * childH * pixelSize, 0);

        for (std::size_t y = 0; y < childH; ++y)
        {
            const auto y0 = y * parentH / childH;
            const auto y1 = (y + 1) * parentH / childH;

            for (std::size_t x = 0; x < childW; ++x)
            {
                const auto x0 = x * parentW / childW;
                const auto x1 = (x + 1) * parentW / childW;
                const auto count = (x1 - x0) * (y1 - y0);

                for (std::size_t channel = 0; channel < pixelSize; ++channel)
                {
                    std::uint32_t sum = 0;
                    for (auto py = y0; py < y1; ++py)
                        for (auto px = x0; px < x1; ++px)
                            sum += parent[(py * parentW + px) * pixelSize + channel];

                    child[(y * childW + x) * pixelSize + channel] =
                        static_cast<std::uint8_t>((sum + count / 2) / count);
                }
            }
        }

        return child;
    }

    // Reference box downsample for float32 (averaged raw, never clamped).
    std::vector<float> refDownsampleFloats(const std::vector<float>& parent,
                                           const std::size_t parentW,
                                           const std::size_t parentH,
                                           const std::size_t childW,
                                           const std::size_t childH)
    {
        std::vector<float> child(childW * childH, 0.0F);

        for (std::size_t y = 0; y < childH; ++y)
        {
            const auto y0 = y * parentH / childH;
            const auto y1 = (y + 1) * parentH / childH;

            for (std::size_t x = 0; x < childW; ++x)
            {
                const auto x0 = x * parentW / childW;
                const auto x1 = (x + 1) * parentW / childW;
                const auto count = static_cast<float>((x1 - x0) * (y1 - y0));

                double sum = 0.0;
                for (auto py = y0; py < y1; ++py)
                    for (auto px = x0; px < x1; ++px)
                        sum += static_cast<double>(parent[py * parentW + px]);

                child[y * childW + x] = static_cast<float>(sum / static_cast<double>(count));
            }
        }

        return child;
    }

    std::vector<std::vector<std::uint8_t>> refByteChain(const std::vector<std::uint8_t>& base,
                                                        const std::size_t width,
                                                        const std::size_t height,
                                                        const std::size_t pixelSize)
    {
        std::vector<std::vector<std::uint8_t>> chain{base};
        auto pw = width;
        auto ph = height;

        while (pw > 1 || ph > 1)
        {
            const auto cw = std::max<std::size_t>(1, pw / 2);
            const auto ch = std::max<std::size_t>(1, ph / 2);
            chain.push_back(refDownsampleBytes(chain.back(), pw, ph, cw, ch, pixelSize));
            pw = cw;
            ph = ch;
        }

        return chain;
    }

    std::vector<std::vector<float>> refFloatChain(const std::vector<float>& base,
                                                  const std::size_t width,
                                                  const std::size_t height)
    {
        std::vector<std::vector<float>> chain{base};
        auto pw = width;
        auto ph = height;

        while (pw > 1 || ph > 1)
        {
            const auto cw = std::max<std::size_t>(1, pw / 2);
            const auto ch = std::max<std::size_t>(1, ph / 2);
            chain.push_back(refDownsampleFloats(chain.back(), pw, ph, cw, ch));
            pw = cw;
            ph = ch;
        }

        return chain;
    }

    template <class F>
    bool threwRenderError(const F& fn)
    {
        try
        {
            fn();
        }
        catch (const RenderError&)
        {
            return true;
        }
        return false;
    }

    Vertex makeVertex(const float x, const float y, const float z)
    {
        Vertex vertex;
        vertex.position = sr::Vector<float, 4>{x, y, z, 1.0F};
        vertex.color = Color{1.0F, 1.0F, 1.0F, 1.0F};
        vertex.texCoords[0] = sr::Vector<float, 2>{(x + 1.0F) / 2.0F, (y + 1.0F) / 2.0F};
        vertex.texCoords[1] = sr::Vector<float, 2>{0.0F, 0.0F};
        vertex.normal = sr::Vector<float, 3>{0.0F, 0.0F, 1.0F};
        return vertex;
    }

    // Full-screen quad at a constant NDC depth.
    std::vector<Vertex> quadVertices(const float z)
    {
        return {
            makeVertex(-1.0F, -1.0F, z),
            makeVertex( 1.0F, -1.0F, z),
            makeVertex( 1.0F,  1.0F, z),
            makeVertex(-1.0F,  1.0F, z)
        };
    }

    const std::vector<std::size_t> kQuadIndices{0, 1, 2, 0, 2, 3};

    VertexShaderOutput identityVertexShader(const sr::Matrix<float, 4>&, const Vertex& vertex)
    {
        return {vertex.position, vertex.color,
                {vertex.texCoords[0], vertex.texCoords[1]}, vertex.normal};
    }

    Color constantShader(const VertexShaderOutput&,
                         const std::array<const Sampler*, 2>&,
                         const std::array<const Texture*, 2>&)
    {
        return Color{1.0F, 0.0F, 0.0F, 1.0F};
    }

    Color sampleCenterShader(const VertexShaderOutput&,
                             const std::array<const Sampler*, 2>& samplers,
                             const std::array<const Texture*, 2>& textures)
    {
        return textures[0]->sample(samplers[0], sr::Vector<float, 2>{0.5F, 0.5F});
    }

    // Samples texel (0, 0) and swaps red/blue, so written pixels differ from
    // the source contents.
    Color feedbackShader(const VertexShaderOutput&,
                         const std::array<const Sampler*, 2>& samplers,
                         const std::array<const Texture*, 2>& textures)
    {
        const auto sampled = textures[0]->sample(
            samplers[0], sr::Vector<float, 2>{0.0F, 0.0F});
        return Color{sampled.b, sampled.g, sampled.r, 1.0F};
    }

    Color halfRedShader(const VertexShaderOutput&,
                        const std::array<const Sampler*, 2>&,
                        const std::array<const Texture*, 2>&)
    {
        return Color{1.0F, 0.0F, 0.0F, 0.5F};
    }

    // ------------------------------------------------------------------
    // storage group
    // ------------------------------------------------------------------

    int storageAllocation()
    {
        Texture nonSquare{PixelFormat::rgba8, 6, 4, true};
        const std::size_t expectedW[] = {6, 3, 1};
        const std::size_t expectedH[] = {4, 2, 1};
        TASK_CHECK(nonSquare.getLevelCount() == 3);
        for (std::uint32_t level = 0; level < 3; ++level)
        {
            TASK_CHECK(nonSquare.getWidth(level) == expectedW[level]);
            TASK_CHECK(nonSquare.getHeight(level) == expectedH[level]);
            TASK_CHECK(nonSquare.getData(level).size() ==
                       expectedW[level] * expectedH[level] * 4);
        }

        Texture wide{PixelFormat::rgba8, 8, 2, true}; // 8x2, 4x1, 2x1, 1x1
        TASK_CHECK(wide.getLevelCount() == 4);
        TASK_CHECK(wide.getWidth(0) == 8 && wide.getHeight(0) == 2);
        TASK_CHECK(wide.getWidth(1) == 4 && wide.getHeight(1) == 1);
        TASK_CHECK(wide.getWidth(2) == 2 && wide.getHeight(2) == 1);
        TASK_CHECK(wide.getWidth(3) == 1 && wide.getHeight(3) == 1);

        Texture singleRow{PixelFormat::r8, 4, 1, true}; // 4x1, 2x1, 1x1
        TASK_CHECK(singleRow.getLevelCount() == 3);
        TASK_CHECK(singleRow.getHeight(0) == 1 && singleRow.getHeight(2) == 1);
        TASK_CHECK(singleRow.getData(2).size() == 1);

        Texture plain{PixelFormat::rgba8, 5, 3, false};
        TASK_CHECK(plain.getLevelCount() == 1);
        TASK_CHECK(plain.getWidth() == 5 && plain.getHeight() == 3);
        TASK_CHECK(threwRenderError([&] { (void)plain.getWidth(1); }));
        TASK_CHECK(threwRenderError([&] { (void)plain.getData(1); }));

        Texture zeroWidth{PixelFormat::rgba8, 0, 4, true};
        Texture zeroHeight{PixelFormat::rgba8, 4, 0, true};
        TASK_CHECK(zeroWidth.getLevelCount() == 0);
        TASK_CHECK(zeroHeight.getLevelCount() == 0);
        TASK_CHECK(threwRenderError([&] { (void)zeroWidth.getWidth(0); }));

        return 0;
    }

    int storageUploadValidation()
    {
        Texture texture{PixelFormat::rgba8, 4, 2, true}; // 32, 8, 4 byte levels

        // Buffer size is checked against the selected level, not the base.
        TASK_CHECK(threwRenderError([&] { texture.setData(std::vector<std::uint8_t>(31), 0); }));
        TASK_CHECK(threwRenderError([&] { texture.setData(std::vector<std::uint8_t>(8), 0); }));
        TASK_CHECK(threwRenderError([&] { texture.setData(std::vector<std::uint8_t>(4), 1); }));
        TASK_CHECK(!threwRenderError([&] { texture.setData(std::vector<std::uint8_t>(8, 0xAB), 1); }));

        // Uploads must never create levels that were not allocated.
        TASK_CHECK(threwRenderError([&] { texture.setData(std::vector<std::uint8_t>(4), 3); }));
        TASK_CHECK(texture.getLevelCount() == 3);

        Texture plain{PixelFormat::rgba8, 4, 2, false};
        TASK_CHECK(threwRenderError([&] { plain.setData(std::vector<std::uint8_t>(8), 1); }));
        TASK_CHECK(plain.getLevelCount() == 1);

        // A rejected upload leaves the texture untouched.
        Texture guarded{PixelFormat::rgba8, 2, 2, false};
        guarded.setData(std::vector<std::uint8_t>(16, 0x7F), 0);
        const auto before = guarded.getData(0);
        TASK_CHECK(threwRenderError([&] { guarded.setData(std::vector<std::uint8_t>(8), 0); }));
        TASK_CHECK(guarded.getData(0) == before);

        // Render target validation happens before any write.
        Texture badColor{PixelFormat::r8, 4, 4, true};
        Texture depth{PixelFormat::float32, 4, 4, true};
        Texture color{PixelFormat::rgba8, 8, 8, true};
        const auto vertices = quadVertices(0.2F);
        DepthState depthState;
        depthState.read = depthState.write = true;
        Sampler sampler;

        TASK_CHECK(threwRenderError([&] {
            sr::drawTriangles(TextureView{badColor, 0}, TextureView{depth, 0},
                              identityVertexShader, constantShader,
                              {&sampler, nullptr}, {nullptr, nullptr},
                              Rect<float>{0, 0, 4, 4}, Rect<float>{0, 0, 1, 1},
                              BlendState{}, depthState, kQuadIndices, vertices,
                              sr::Matrix<float, 4>{});
        }));

        Texture badDepth{PixelFormat::rgba8, 4, 4, true};
        TASK_CHECK(threwRenderError([&] {
            sr::drawTriangles(TextureView{color, 0}, TextureView{badDepth, 0},
                              identityVertexShader, constantShader,
                              {&sampler, nullptr}, {nullptr, nullptr},
                              Rect<float>{0, 0, 8, 8}, Rect<float>{0, 0, 1, 1},
                              BlendState{}, depthState, kQuadIndices, vertices,
                              sr::Matrix<float, 4>{});
        }));

        // Selected levels must exist and match in size.
        TASK_CHECK(threwRenderError([&] {
            sr::drawTriangles(TextureView{color, 9}, TextureView{depth, 0},
                              identityVertexShader, constantShader,
                              {&sampler, nullptr}, {nullptr, nullptr},
                              Rect<float>{0, 0, 1, 1}, Rect<float>{0, 0, 1, 1},
                              BlendState{}, depthState, kQuadIndices, vertices,
                              sr::Matrix<float, 4>{});
        }));
        Texture bigDepth{PixelFormat::float32, 8, 8, true};
        TASK_CHECK(threwRenderError([&] {
            // color level 1 is 4x4, bigDepth level 0 is 8x8
            sr::drawTriangles(TextureView{color, 1}, TextureView{bigDepth, 0},
                              identityVertexShader, constantShader,
                              {&sampler, nullptr}, {nullptr, nullptr},
                              Rect<float>{0, 0, 4, 4}, Rect<float>{0, 0, 1, 1},
                              BlendState{}, depthState, kQuadIndices, vertices,
                              sr::Matrix<float, 4>{});
        }));

        return 0;
    }

    int storageChainMeans()
    {
        // rgba8 on a non-square base with an uneven 5x3 -> 2x1 -> 1x1 chain.
        std::vector<std::uint8_t> baseRgba(5 * 3 * 4);
        for (std::size_t p = 0; p < 5 * 3; ++p)
        {
            baseRgba[p * 4 + 0] = static_cast<std::uint8_t>(p + 1);
            baseRgba[p * 4 + 1] = static_cast<std::uint8_t>(200 - p);
            baseRgba[p * 4 + 2] = static_cast<std::uint8_t>(p * 7);
            baseRgba[p * 4 + 3] = 255;
        }
        const auto expectedRgba = refByteChain(baseRgba, 5, 3, 4);

        Texture rgbaTexture{PixelFormat::rgba8, 5, 3, true};
        rgbaTexture.setData(baseRgba, 0);
        TASK_CHECK(rgbaTexture.getLevelCount() == expectedRgba.size());
        for (std::uint32_t level = 0; level < expectedRgba.size(); ++level)
        {
            TASK_CHECK(rgbaTexture.getWidth(level) == refDim(5, level));
            TASK_CHECK(rgbaTexture.getHeight(level) == refDim(3, level));
            TASK_CHECK(rgbaTexture.getData(level) == expectedRgba[level]);
        }

        // r8 single row: rounding at the 0.5 boundary.
        const std::vector<std::uint8_t> baseR8{0, 1, 10, 11};
        Texture r8Texture{PixelFormat::r8, 4, 1, true};
        r8Texture.setData(baseR8, 0);
        TASK_CHECK(r8Texture.getData(1).size() == 2);
        TASK_CHECK(r8Texture.getData(1)[0] == 1);  // (0+1)/2 = 0.5 -> 1
        TASK_CHECK(r8Texture.getData(1)[1] == 11); // (10+11)/2 = 10.5 -> 11
        TASK_CHECK(r8Texture.getData(2)[0] == 6);  // (1+11)/2 = 6
        const auto expectedR8 = refByteChain(baseR8, 4, 1, 1);
        for (std::uint32_t level = 0; level < expectedR8.size(); ++level)
            TASK_CHECK(r8Texture.getData(level) == expectedR8[level]);

        // a8: 2x2 mean of 0,1,2,3 -> sum 6, 6/4 = 1.5 rounds to 2.
        const std::vector<std::uint8_t> baseA8{0, 1, 2, 3};
        Texture a8Texture{PixelFormat::a8, 2, 2, true};
        a8Texture.setData(baseA8, 0);
        TASK_CHECK(a8Texture.getData(1)[0] == 2);

        // float32 values outside the color range are averaged raw.
        const std::vector<float> baseFloats{3.0F, -2.0F, 1.0F, 4.0F};
        const auto expectedFloats = refFloatChain(baseFloats, 4, 1);
        Texture floatTexture{PixelFormat::float32, 4, 1, true};
        std::vector<std::uint8_t> floatBytes(baseFloats.size() * sizeof(float));
        std::memcpy(floatBytes.data(), baseFloats.data(), floatBytes.size());
        floatTexture.setData(floatBytes, 0);
        for (std::uint32_t level = 0; level < expectedFloats.size(); ++level)
        {
            const auto& actualLevel = floatTexture.getData(level);
            TASK_CHECK(actualLevel.size() == expectedFloats[level].size() * sizeof(float));
            const auto* actualFloats = reinterpret_cast<const float*>(actualLevel.data());
            for (std::size_t p = 0; p < expectedFloats[level].size(); ++p)
                TASK_CHECK(nearFloat(actualFloats[p], expectedFloats[level][p], 1e-6F));
        }
        const auto* level1 = reinterpret_cast<const float*>(floatTexture.getData(1).data());
        TASK_CHECK(nearFloat(level1[0], 0.5F, 1e-6F));  // (3 + -2) / 2
        TASK_CHECK(nearFloat(level1[1], 2.5F, 1e-6F));

        // generateMipMaps(startLevel) rebuilds descendants only.
        auto& editedLevel = r8Texture.getData(1);
        editedLevel[0] = 100;
        editedLevel[1] = 100;
        const auto untouchedBase = r8Texture.getData(0);
        r8Texture.generateMipMaps(1);
        TASK_CHECK(r8Texture.getData(0) == untouchedBase);
        TASK_CHECK(r8Texture.getData(2)[0] == 100);

        // Direct byte editing does not auto-update; an explicit call does.
        Texture manual{PixelFormat::rgba8, 2, 2, true};
        manual.setData(std::vector<std::uint8_t>(16, 0), 0);
        manual.getData(0)[0] = 200;
        TASK_CHECK(manual.getData(1)[0] == 0);
        manual.generateMipMaps();
        TASK_CHECK(manual.getData(1)[0] == 50); // 200/4 rounded

        return 0;
    }

    int storageResizeAndViews()
    {
        Texture texture{PixelFormat::rgba8, 4, 4, true};
        TextureView view{texture, 1};
        TASK_CHECK(view.getWidth() == 2 && view.getHeight() == 2);

        // The view does not own the texture and resolves against current size.
        texture.resize(8, 2);
        TASK_CHECK(texture.getLevelCount() == 4);
        TASK_CHECK(view.getLevel() == 1);
        TASK_CHECK(view.getWidth() == 4 && view.getHeight() == 1);
        TASK_CHECK(view.getData().size() == 4 * 1 * 4);

        // Old Texture& entry points still mean level zero.
        TASK_CHECK(texture.getWidth() == 8 && texture.getHeight() == 2);

        texture.resize(0, 0);
        TASK_CHECK(texture.getLevelCount() == 0);
        TASK_CHECK(threwRenderError([&] { (void)view.getWidth(); }));
        TASK_CHECK(threwRenderError([&] { (void)view.getData(); }));

        // Resizing back restores a usable chain through the same view.
        texture.resize(4, 4);
        TASK_CHECK(view.getWidth() == 2 && view.getHeight() == 2);

        return 0;
    }

    // ------------------------------------------------------------------
    // sampling group
    // ------------------------------------------------------------------

    // Fills every texel of a level with one rgba color.
    void fillLevelUniform(Texture& texture, const std::uint32_t level,
                          const std::uint8_t r, const std::uint8_t g,
                          const std::uint8_t b, const std::uint8_t a)
    {
        auto& buffer = texture.getData(level);
        for (std::size_t p = 0; p < buffer.size(); p += 4)
        {
            buffer[p + 0] = r;
            buffer[p + 1] = g;
            buffer[p + 2] = b;
            buffer[p + 3] = a;
        }
    }

    int samplingPointLod()
    {
        Texture texture{PixelFormat::rgba8, 4, 2, true}; // 4x2, 2x1, 1x1
        fillLevelUniform(texture, 0, 10, 20, 30, 255);
        fillLevelUniform(texture, 1, 40, 50, 60, 255);
        fillLevelUniform(texture, 2, 70, 80, 90, 255);

        Sampler sampler;
        sampler.filter = Sampler::Filter::point;
        sampler.mipFilter = Sampler::Filter::point;
        const sr::Vector<float, 2> uv{0.3F, 0.7F};

        sampler.lod = 0.0F;
        auto c = texture.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.r, 10 / 255.0F) && nearFloat(c.g, 20 / 255.0F));

        sampler.lod = 0.49F; // floor(0.99) -> level 0
        c = texture.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.r, 10 / 255.0F));

        sampler.lod = 0.5F; // floor(1.0) -> level 1
        c = texture.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.r, 40 / 255.0F) && nearFloat(c.b, 60 / 255.0F));

        sampler.lod = 1.5F; // -> level 2
        c = texture.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.g, 80 / 255.0F));

        sampler.lod = 99.0F; // clamped to the last level
        c = texture.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.r, 70 / 255.0F));

        sampler.lod = -4.0F; // clamped to level 0
        c = texture.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.r, 10 / 255.0F));

        // A non-mip texture always reads its single level.
        Texture plain{PixelFormat::rgba8, 4, 2, false};
        fillLevelUniform(plain, 0, 1, 2, 3, 255);
        sampler.lod = 2.0F;
        c = plain.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.r, 1 / 255.0F));

        return 0;
    }

    int samplingLinearLod()
    {
        Texture texture{PixelFormat::rgba8, 4, 2, true};
        fillLevelUniform(texture, 0, 0, 0, 0, 255);
        fillLevelUniform(texture, 1, 100, 100, 100, 255);
        fillLevelUniform(texture, 2, 200, 200, 200, 255);

        Sampler sampler;
        sampler.filter = Sampler::Filter::point;
        sampler.mipFilter = Sampler::Filter::linear;
        const sr::Vector<float, 2> uv{0.3F, 0.7F};

        sampler.lod = 0.5F;
        auto c = texture.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.r, 50 / 255.0F, 1e-4F));

        sampler.lod = 1.25F; // 0.75 * L1 + 0.25 * L2 = 125
        c = texture.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.r, 125 / 255.0F, 1e-4F));

        sampler.lod = 2.0F; // exact level
        c = texture.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.r, 200 / 255.0F, 1e-4F));

        sampler.lod = 99.0F; // clamped at the top
        c = texture.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.r, 200 / 255.0F, 1e-4F));

        sampler.lod = -1.0F; // clamped at the bottom
        c = texture.sample(&sampler, uv);
        TASK_CHECK(nearFloat(c.r, 0.0F, 1e-6F));

        return 0;
    }

    int samplingIntraLevel()
    {
        // 4x2 grid, texel (x, y) has rgb = (x*10, y*20, x*10+y*20).
        Texture texture{PixelFormat::rgba8, 4, 2, false};
        for (std::size_t y = 0; y < 2; ++y)
            for (std::size_t x = 0; x < 4; ++x)
            {
                auto& buffer = texture.getData(0);
                auto* pixel = &buffer[(y * 4 + x) * 4];
                pixel[0] = static_cast<std::uint8_t>(x * 10);
                pixel[1] = static_cast<std::uint8_t>(y * 20);
                pixel[2] = static_cast<std::uint8_t>(x * 10 + y * 20);
                pixel[3] = 255;
            }

        Sampler pointSampler; // default point/point, lod 0
        auto c = texture.sample(&pointSampler, sr::Vector<float, 2>{0.0F, 0.0F});
        TASK_CHECK(nearFloat(c.r, 0.0F) && nearFloat(c.g, 0.0F));
        c = texture.sample(&pointSampler, sr::Vector<float, 2>{1.0F, 1.0F});
        TASK_CHECK(nearFloat(c.r, 30 / 255.0F) && nearFloat(c.g, 20 / 255.0F));
        c = texture.sample(&pointSampler, sr::Vector<float, 2>{0.5F, 0.0F}); // u*3 = 1.5 -> 2
        TASK_CHECK(nearFloat(c.r, 20 / 255.0F));

        Sampler linearSampler;
        linearSampler.filter = Sampler::Filter::linear;
        // u*3 = 0.5 -> halfway between texels 0 and 1 on row 0.
        c = texture.sample(&linearSampler, sr::Vector<float, 2>{1.0F / 6.0F, 0.0F});
        TASK_CHECK(nearFloat(c.r, 5 / 255.0F, 1e-5F));
        TASK_CHECK(nearFloat(c.g, 0.0F, 1e-6F));
        // Edge clamping: past the far edge still returns the edge texel.
        c = texture.sample(&linearSampler, sr::Vector<float, 2>{1.5F, 1.5F});
        TASK_CHECK(nearFloat(c.r, 30 / 255.0F) && nearFloat(c.g, 20 / 255.0F));
        // Bilinear in both axes at the center of the 2x2 left block.
        c = texture.sample(&linearSampler, sr::Vector<float, 2>{1.0F / 6.0F, 0.5F});
        TASK_CHECK(nearFloat(c.r, 5 / 255.0F, 1e-5F));
        TASK_CHECK(nearFloat(c.g, 10 / 255.0F, 1e-5F));

        // Point addressing inside a non-square mip level (2x1).
        Texture mipTexture{PixelFormat::rgba8, 4, 2, true};
        fillLevelUniform(mipTexture, 0, 1, 1, 1, 255);
        auto& level1 = mipTexture.getData(1);
        level1[0] = 50; level1[1] = 50; level1[2] = 50; level1[3] = 255;
        level1[4] = 90; level1[5] = 90; level1[6] = 90; level1[7] = 255;
        pointSampler.lod = 1.0F;
        c = mipTexture.sample(&pointSampler, sr::Vector<float, 2>{0.0F, 0.0F});
        TASK_CHECK(nearFloat(c.r, 50 / 255.0F));
        c = mipTexture.sample(&pointSampler, sr::Vector<float, 2>{1.0F, 0.0F});
        TASK_CHECK(nearFloat(c.r, 90 / 255.0F));

        return 0;
    }

    int samplingShaderDraw()
    {
        // Input with uniform colors per level; the shader samples a fixed UV.
        Texture input{PixelFormat::rgba8, 4, 2, true};
        fillLevelUniform(input, 0, 10, 20, 30, 255);
        fillLevelUniform(input, 1, 40, 50, 60, 255);
        fillLevelUniform(input, 2, 70, 80, 90, 255);

        Texture color{PixelFormat::rgba8, 6, 4, true};
        Texture depth{PixelFormat::float32, 6, 4, true};
        sr::clear(color, Color{0.0F, 0.0F, 0.0F, 1.0F});
        sr::clear(depth, 1.0F);

        Sampler sampler;
        sampler.filter = Sampler::Filter::point;
        sampler.mipFilter = Sampler::Filter::point;
        sampler.lod = 1.0F;

        DepthState depthState;
        depthState.read = depthState.write = true;
        const auto vertices = quadVertices(0.2F);

        sr::drawTriangles(color, depth, identityVertexShader, sampleCenterShader,
                          {&sampler, nullptr}, {&input, nullptr},
                          Rect<float>{0, 0, 6, 4}, Rect<float>{0, 0, 1, 1},
                          BlendState{}, depthState, kQuadIndices, vertices,
                          sr::Matrix<float, 4>{});

        // Every covered pixel holds the quantized level-1 uniform color.
        const auto& output = color.getData(0);
        for (std::size_t p = 0; p < 6 * 4; ++p)
        {
            TASK_CHECK(output[p * 4 + 0] == 40);
            TASK_CHECK(output[p * 4 + 1] == 50);
            TASK_CHECK(output[p * 4 + 2] == 60);
            TASK_CHECK(output[p * 4 + 3] == 255);
        }

        // The draw also refreshed the descendant chain of the target.
        const auto expectedChain = refByteChain(output, 6, 4, 4);
        for (std::uint32_t level = 1; level < expectedChain.size(); ++level)
            TASK_CHECK(color.getData(level) == expectedChain[level]);

        // Fractional LOD through a real shader invocation.
        Texture color2{PixelFormat::rgba8, 4, 2, true};
        Texture depth2{PixelFormat::float32, 4, 2, true};
        sr::clear(color2, Color{0.0F, 0.0F, 0.0F, 1.0F});
        sr::clear(depth2, 1.0F);
        sampler.mipFilter = Sampler::Filter::linear;
        sampler.lod = 0.5F;
        sr::drawTriangles(color2, depth2, identityVertexShader, sampleCenterShader,
                          {&sampler, nullptr}, {&input, nullptr},
                          Rect<float>{0, 0, 4, 2}, Rect<float>{0, 0, 1, 1},
                          BlendState{}, depthState, kQuadIndices, quadVertices(0.2F),
                          sr::Matrix<float, 4>{});
        const auto& output2 = color2.getData(0);
        for (std::size_t p = 0; p < 4 * 2; ++p)
        {
            TASK_CHECK(output2[p * 4 + 0] == 25); // (10 + 40) / 2
            TASK_CHECK(output2[p * 4 + 1] == 35); // (20 + 50) / 2
            TASK_CHECK(output2[p * 4 + 2] == 45); // (30 + 60) / 2
        }

        return 0;
    }

    // ------------------------------------------------------------------
    // feedback group
    // ------------------------------------------------------------------

    int feedbackLevelIsolation()
    {
        // 8x4 chain: 8x4, 4x2, 2x1, 1x1.
        Texture color{PixelFormat::rgba8, 8, 4, true};
        for (std::uint32_t level = 0; level < color.getLevelCount(); ++level)
        {
            auto& buffer = color.getData(level);
            std::fill(buffer.begin(), buffer.end(), static_cast<std::uint8_t>(0xDD));
        }

        const auto ancestorSnapshot = color.getData(0);
        const std::vector<std::uint8_t> sentinel(color.getData(1).size(), 0xDD);

        Texture depth{PixelFormat::float32, 8, 4, true};
        for (std::uint32_t level = 0; level < depth.getLevelCount(); ++level)
        {
            auto& buffer = depth.getData(level);
            auto* floats = reinterpret_cast<float*>(buffer.data());
            for (std::size_t p = 0; p < buffer.size() / sizeof(float); ++p)
                floats[p] = 1.0F;
        }
        const auto depthAncestorSnapshot = depth.getData(0);

        TextureView colorView{color, 1};
        TextureView depthView{depth, 1};
        TASK_CHECK(colorView.getWidth() == 4 && colorView.getHeight() == 2);

        DepthState depthState;
        depthState.read = depthState.write = true;
        Sampler sampler;

        sr::drawTriangles(colorView, depthView,
                          identityVertexShader, constantShader,
                          {&sampler, nullptr}, {nullptr, nullptr},
                          Rect<float>{0, 0, 4, 2}, Rect<float>{0, 0, 1, 1},
                          BlendState{}, depthState, kQuadIndices,
                          quadVertices(0.2F), sr::Matrix<float, 4>{});

        // The ancestor level is untouched.
        TASK_CHECK(color.getData(0) == ancestorSnapshot);
        TASK_CHECK(depth.getData(0) == depthAncestorSnapshot);

        // The drawn level is fully red.
        const auto& drawn = color.getData(1);
        TASK_CHECK(drawn != sentinel);
        for (std::size_t p = 0; p < 4 * 2; ++p)
        {
            TASK_CHECK(drawn[p * 4 + 0] == 255);
            TASK_CHECK(drawn[p * 4 + 1] == 0);
            TASK_CHECK(drawn[p * 4 + 2] == 0);
            TASK_CHECK(drawn[p * 4 + 3] == 255);
        }

        // Descendants were regenerated from the drawn level.
        const auto expectedChain = refByteChain(drawn, 4, 2, 4);
        TASK_CHECK(color.getData(2) == expectedChain[1]);
        TASK_CHECK(color.getData(3) == expectedChain[2]);

        const auto* depthLevel1 = reinterpret_cast<const float*>(depth.getData(1).data());
        for (std::size_t p = 0; p < 4 * 2; ++p)
            TASK_CHECK(nearFloat(depthLevel1[p], 0.2F, 1e-6F));
        const auto* depthLevel2 = reinterpret_cast<const float*>(depth.getData(2).data());
        TASK_CHECK(nearFloat(depthLevel2[0], 0.2F, 1e-6F));

        // clear() follows the same rules.
        Texture cleared{PixelFormat::rgba8, 8, 4, true};
        for (std::uint32_t level = 0; level < cleared.getLevelCount(); ++level)
        {
            auto& buffer = cleared.getData(level);
            std::fill(buffer.begin(), buffer.end(), static_cast<std::uint8_t>(0xEE));
        }
        const auto clearedAncestor = cleared.getData(0);
        sr::clear(TextureView{cleared, 1}, Color{0.0F, 1.0F, 0.0F, 1.0F});
        TASK_CHECK(cleared.getData(0) == clearedAncestor);
        TASK_CHECK(cleared.getData(1)[1] == 255);
        const auto expectedCleared = refByteChain(cleared.getData(1), 4, 2, 4);
        TASK_CHECK(cleared.getData(2) == expectedCleared[1]);
        TASK_CHECK(cleared.getData(3) == expectedCleared[2]);

        return 0;
    }

    int feedbackDepthBlendScissor()
    {
        // Work on level 1 of 8x8 textures: a 4x4 target.
        Texture color{PixelFormat::rgba8, 8, 8, true};
        Texture depth{PixelFormat::float32, 8, 8, true};
        TextureView colorView{color, 1};
        TextureView depthView{depth, 1};

        sr::clear(colorView, Color{0.0F, 0.0F, 0.0F, 1.0F});
        sr::clear(depthView, 1.0F);

        DepthState depthState;
        depthState.read = depthState.write = true;
        Sampler sampler;
        BlendState blend;
        blend.enabled = true;
        blend.colorBlendSource = BlendState::Factor::srcAlpha;
        blend.colorBlendDest = BlendState::Factor::invSrcAlpha;
        blend.alphaBlendSource = BlendState::Factor::srcAlpha;
        blend.alphaBlendDest = BlendState::Factor::invSrcAlpha;

        // Single lower-right triangle, so no shared diagonal can double-write
        // a pixel. Scissor [0.5, 0.75) x [0, 0.25) of the 4x4 level maps to
        // the half-open pixel rect {2} x {0}: floor(0.5*4)=2, ceil(0.75*4)=3,
        // floor(0)=0, ceil(0.25*4)=1.
        const std::vector<Vertex> triangle{
            makeVertex(-1.0F, -1.0F, 0.2F),
            makeVertex( 1.0F, -1.0F, 0.2F),
            makeVertex( 1.0F,  1.0F, 0.2F)
        };
        const std::vector<std::size_t> triangleIndices{0, 1, 2};

        sr::drawTriangles(colorView, depthView,
                          identityVertexShader, halfRedShader,
                          {&sampler, nullptr}, {nullptr, nullptr},
                          Rect<float>{0, 0, 4, 4},
                          Rect<float>{0.5F, 0.0F, 0.25F, 0.25F},
                          blend, depthState, triangleIndices, triangle,
                          sr::Matrix<float, 4>{});

        const auto& target = color.getData(1);
        const auto* depthPixels = reinterpret_cast<const float*>(depth.getData(1).data());
        for (std::size_t y = 0; y < 4; ++y)
            for (std::size_t x = 0; x < 4; ++x)
            {
                const auto* pixel = &target[(y * 4 + x) * 4];
                const bool inside = (x == 2 && y == 0);
                if (inside)
                {
                    // One alpha blend: r = 0.5*255 truncated, a = 0.75*255.
                    TASK_CHECK(pixel[0] == 127);
                    TASK_CHECK(pixel[1] == 0 && pixel[2] == 0);
                    TASK_CHECK(pixel[3] == 191);
                    TASK_CHECK(nearFloat(depthPixels[y * 4 + x], 0.2F, 1e-6F));
                }
                else
                {
                    TASK_CHECK(pixel[0] == 0 && pixel[3] == 255);
                    TASK_CHECK(nearFloat(depthPixels[y * 4 + x], 1.0F, 1e-6F));
                }
            }

        // An opaque, farther quad must lose the depth test at the written
        // pixel and fill the still-clear pixels everywhere else.
        sr::drawTriangles(colorView, depthView,
                          identityVertexShader, constantShader,
                          {&sampler, nullptr}, {nullptr, nullptr},
                          Rect<float>{0, 0, 4, 4}, Rect<float>{0, 0, 1, 1},
                          BlendState{}, depthState, kQuadIndices,
                          quadVertices(0.9F), sr::Matrix<float, 4>{});

        for (std::size_t y = 0; y < 4; ++y)
            for (std::size_t x = 0; x < 4; ++x)
            {
                const auto* pixel = &target[(y * 4 + x) * 4];
                if (x == 2 && y == 0)
                {
                    // Blended result and near depth survive.
                    TASK_CHECK(pixel[0] == 127 && pixel[3] == 191);
                    TASK_CHECK(nearFloat(depthPixels[y * 4 + x], 0.2F, 1e-6F));
                }
                else
                {
                    TASK_CHECK(pixel[0] == 255 && pixel[3] == 255);
                    TASK_CHECK(nearFloat(depthPixels[y * 4 + x], 0.9F, 1e-6F));
                }
            }

        return 0;
    }

    int feedbackAliasVsCopy()
    {
        auto makePattern = [] {
            std::vector<std::uint8_t> pattern(4 * 4 * 4);
            for (std::size_t p = 0; p < 4 * 4; ++p)
            {
                pattern[p * 4 + 0] = static_cast<std::uint8_t>((p + 1) * 3);
                pattern[p * 4 + 1] = static_cast<std::uint8_t>(p * 5);
                pattern[p * 4 + 2] = static_cast<std::uint8_t>(250 - p * 2);
                pattern[p * 4 + 3] = 255;
            }
            // Texel (0, 0), read by every fragment, has a distinctive color.
            pattern[0] = 10;
            pattern[1] = 20;
            pattern[2] = 30;
            pattern[3] = 255;
            return pattern;
        };

        const auto pattern = makePattern();

        Texture aliased{PixelFormat::rgba8, 4, 4, true};
        aliased.setData(pattern, 0);

        Texture separate{PixelFormat::rgba8, 4, 4, true};
        separate.setData(pattern, 0);

        Texture independentCopy{PixelFormat::rgba8, 4, 4, true};
        independentCopy.setData(pattern, 0);

        Sampler sampler; // point, lod 0: uv (0,0) reads texel (0,0)
        DepthState depthState;
        depthState.read = depthState.write = true;
        Texture depthA{PixelFormat::float32, 4, 4, true};
        Texture depthB{PixelFormat::float32, 4, 4, true};
        sr::clear(depthA, 1.0F);
        sr::clear(depthB, 1.0F);

        // Real feedback draw: the target is also the input.
        sr::drawTriangles(aliased, depthA,
                          identityVertexShader, feedbackShader,
                          {&sampler, nullptr}, {&aliased, nullptr},
                          Rect<float>{0, 0, 4, 4}, Rect<float>{0, 0, 1, 1},
                          BlendState{}, depthState, kQuadIndices,
                          quadVertices(0.2F), sr::Matrix<float, 4>{});

        // Reference draw: an independent copy feeds the same shader.
        sr::drawTriangles(separate, depthB,
                          identityVertexShader, feedbackShader,
                          {&sampler, nullptr}, {&independentCopy, nullptr},
                          Rect<float>{0, 0, 4, 4}, Rect<float>{0, 0, 1, 1},
                          BlendState{}, depthState, kQuadIndices,
                          quadVertices(0.2F), sr::Matrix<float, 4>{});

        TASK_CHECK(aliased.getLevelCount() == separate.getLevelCount());
        for (std::uint32_t level = 0; level < aliased.getLevelCount(); ++level)
            TASK_CHECK(aliased.getData(level) == separate.getData(level));

        // Every fragment saw the draw-start texel (0,0): swapped r/b =
        // (30, 20, 10). Without a snapshot, fragments after the first would
        // read the already-swapped color and swap it back.
        const auto& output = aliased.getData(0);
        for (std::size_t p = 0; p < 4 * 4; ++p)
        {
            TASK_CHECK(output[p * 4 + 0] == 30);
            TASK_CHECK(output[p * 4 + 1] == 20);
            TASK_CHECK(output[p * 4 + 2] == 10);
            TASK_CHECK(output[p * 4 + 3] == 255);
        }

        // Depth targets match as well.
        for (std::uint32_t level = 0; level < depthA.getLevelCount(); ++level)
            TASK_CHECK(depthA.getData(level) == depthB.getData(level));

        return 0;
    }

    int feedbackLegacyEntryPoints()
    {
        // Original Texture& overloads keep their level-zero behavior.
        Texture color{PixelFormat::rgba8, 4, 2, true};
        Texture depth{PixelFormat::float32, 4, 2, true};
        sr::clear(color, Color{0.0F, 0.0F, 0.0F, 1.0F});
        sr::clear(depth, 1.0F);

        Sampler sampler;
        DepthState depthState;
        depthState.read = depthState.write = true;
        sr::drawTriangles(color, depth,
                          identityVertexShader, constantShader,
                          {&sampler, nullptr}, {nullptr, nullptr},
                          Rect<float>{0, 0, 4, 2}, Rect<float>{0, 0, 1, 1},
                          BlendState{}, depthState, kQuadIndices,
                          quadVertices(0.3F), sr::Matrix<float, 4>{});

        const auto& base = color.getData(0);
        for (std::size_t p = 0; p < 4 * 2; ++p)
            TASK_CHECK(base[p * 4] == 255 && base[p * 4 + 1] == 0);

        const auto expectedChain = refByteChain(base, 4, 2, 4);
        TASK_CHECK(color.getData(1) == expectedChain[1]);
        TASK_CHECK(color.getData(2) == expectedChain[2]);
        const auto* depthLevel1 = reinterpret_cast<const float*>(depth.getData(1).data());
        TASK_CHECK(nearFloat(depthLevel1[0], 0.3F, 1e-6F));

        // A view created before a resize still renders into the new chain.
        Texture resizedColor{PixelFormat::rgba8, 2, 2, true};
        Texture resizedDepth{PixelFormat::float32, 2, 2, true};
        TextureView colorView{resizedColor, 1};
        TextureView depthView{resizedDepth, 1};
        resizedColor.resize(8, 4);
        resizedDepth.resize(8, 4);
        TASK_CHECK(colorView.getWidth() == 4 && colorView.getHeight() == 2);

        sr::clear(colorView, Color{0.0F, 0.0F, 0.0F, 1.0F});
        sr::clear(depthView, 1.0F);
        sr::drawTriangles(colorView, depthView,
                          identityVertexShader, constantShader,
                          {&sampler, nullptr}, {nullptr, nullptr},
                          Rect<float>{0, 0, 4, 2}, Rect<float>{0, 0, 1, 1},
                          BlendState{}, depthState, kQuadIndices,
                          quadVertices(0.4F), sr::Matrix<float, 4>{});
        TASK_CHECK(colorView.getData()[0] == 255);
        TASK_CHECK(resizedColor.getData(0).size() == 8 * 4 * 4); // ancestor exists
        const auto* resizedDepthPixels = reinterpret_cast<const float*>(depthView.getData().data());
        TASK_CHECK(nearFloat(resizedDepthPixels[0], 0.4F, 1e-6F));

        return 0;
    }
}

const struct task_case task_cases[] = {
    {"storage", "allocation", storageAllocation},
    {"storage", "upload_validation", storageUploadValidation},
    {"storage", "chain_means", storageChainMeans},
    {"storage", "resize_and_views", storageResizeAndViews},

    {"sampling", "point_lod", samplingPointLod},
    {"sampling", "linear_lod", samplingLinearLod},
    {"sampling", "intra_level_filters", samplingIntraLevel},
    {"sampling", "shader_draw", samplingShaderDraw},

    {"feedback", "level_isolation", feedbackLevelIsolation},
    {"feedback", "depth_blend_scissor", feedbackDepthBlendScissor},
    {"feedback", "alias_vs_copy", feedbackAliasVsCopy},
    {"feedback", "legacy_entry_points", feedbackLegacyEntryPoints}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
