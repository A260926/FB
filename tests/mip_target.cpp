#include "task_support/check.h"
#include "sr/sr.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace
{
    constexpr int byteTolerance = 1;

    // ------------------------------------------------------------------
    // Reference implementations (independent of sr::Texture internals)
    // ------------------------------------------------------------------

    struct RefLevel
    {
        std::size_t width = 0;
        std::size_t height = 0;
        std::vector<std::uint8_t> bytes;
    };

    std::size_t levelExtentRef(std::size_t base, std::size_t level)
    {
        std::size_t extent = base;
        while (level-- > 0)
        {
            if (extent <= 1)
                return 1;
            extent >>= 1;
        }
        return extent;
    }

    std::vector<RefLevel> buildChain(std::size_t width,
                                     std::size_t height,
                                     std::size_t channels,
                                     const std::vector<std::uint8_t>& base)
    {
        std::vector<RefLevel> chain;
        chain.push_back(RefLevel{width, height, base});

        if (width == 0 || height == 0)
            return chain;

        auto currentWidth = width;
        auto currentHeight = height;

        while (currentWidth > 1 || currentHeight > 1)
        {
            const auto parent = chain.back();
            const auto childWidth = std::max<std::size_t>(1, currentWidth >> 1);
            const auto childHeight = std::max<std::size_t>(1, currentHeight >> 1);

            std::vector<std::uint8_t> child(childWidth * childHeight * channels, 0);

            for (std::size_t y = 0; y < childHeight; ++y)
                for (std::size_t x = 0; x < childWidth; ++x)
                {
                    const auto x0 = x * currentWidth / childWidth;
                    const auto x1 = (x + 1) * currentWidth / childWidth;
                    const auto y0 = y * currentHeight / childHeight;
                    const auto y1 = (y + 1) * currentHeight / childHeight;
                    const auto count = (x1 - x0) * (y1 - y0);

                    for (std::size_t c = 0; c < channels; ++c)
                    {
                        unsigned long long sum = 0;
                        for (std::size_t sy = y0; sy < y1; ++sy)
                            for (std::size_t sx = x0; sx < x1; ++sx)
                                sum += parent.bytes[(sy * currentWidth + sx) * channels + c];

                        child[(y * childWidth + x) * channels + c] = static_cast<std::uint8_t>(
                            std::lround(static_cast<double>(sum) / static_cast<double>(count)));
                    }
                }

            chain.push_back(RefLevel{childWidth, childHeight, std::move(child)});
            currentWidth = childWidth;
            currentHeight = childHeight;
        }

        return chain;
    }

    std::vector<std::vector<float>> buildFloatChain(std::size_t width,
                                                    std::size_t height,
                                                    const std::vector<float>& base)
    {
        std::vector<std::vector<float>> chain;
        chain.push_back(base);

        if (width == 0 || height == 0)
            return chain;

        auto currentWidth = width;
        auto currentHeight = height;

        while (currentWidth > 1 || currentHeight > 1)
        {
            const auto& parent = chain.back();
            const auto childWidth = std::max<std::size_t>(1, currentWidth >> 1);
            const auto childHeight = std::max<std::size_t>(1, currentHeight >> 1);

            std::vector<float> child(childWidth * childHeight, 0.0F);

            for (std::size_t y = 0; y < childHeight; ++y)
                for (std::size_t x = 0; x < childWidth; ++x)
                {
                    const auto x0 = x * currentWidth / childWidth;
                    const auto x1 = (x + 1) * currentWidth / childWidth;
                    const auto y0 = y * currentHeight / childHeight;
                    const auto y1 = (y + 1) * currentHeight / childHeight;
                    const auto count = (x1 - x0) * (y1 - y0);

                    double sum = 0.0;
                    for (std::size_t sy = y0; sy < y1; ++sy)
                        for (std::size_t sx = x0; sx < x1; ++sx)
                            sum += parent[sy * currentWidth + sx];

                    child[y * childWidth + x] = static_cast<float>(sum / static_cast<double>(count));
                }

            chain.push_back(std::move(child));
            currentWidth = childWidth;
            currentHeight = childHeight;
        }

        return chain;
    }

    std::size_t refLodIndex(float lod, std::size_t levelCount)
    {
        auto index = static_cast<long long>(std::floor(lod + 0.5F));
        if (index < 0) index = 0;
        if (index >= static_cast<long long>(levelCount))
            index = static_cast<long long>(levelCount) - 1;
        return static_cast<std::size_t>(index);
    }

    std::array<std::uint8_t, 4> refPointSample(const std::vector<RefLevel>& chain,
                                               std::size_t level,
                                               float u,
                                               float v)
    {
        const auto& ref = chain[level];
        const auto x = std::clamp(
            static_cast<std::size_t>(std::round(std::clamp(u, 0.0F, 1.0F) * static_cast<float>(ref.width - 1))),
            static_cast<std::size_t>(0), ref.width - 1);
        const auto y = std::clamp(
            static_cast<std::size_t>(std::round(std::clamp(v, 0.0F, 1.0F) * static_cast<float>(ref.height - 1))),
            static_cast<std::size_t>(0), ref.height - 1);

        const auto* pixel = &ref.bytes[(y * ref.width + x) * 4];
        return {pixel[0], pixel[1], pixel[2], pixel[3]};
    }

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    sr::Color byteColor(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255U)
    {
        return sr::Color{r, g, b, a};
    }

    std::uint8_t toByte(float value)
    {
        const auto scaled = std::lround(value * 255.0F);
        return static_cast<std::uint8_t>(std::clamp<long>(scaled, 0L, 255L));
    }

    std::vector<std::uint8_t> rgbaGradient(std::size_t width, std::size_t height)
    {
        std::vector<std::uint8_t> data(width * height * 4U);
        for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x)
            {
                const auto i = (y * width + x) * 4U;
                data[i + 0] = static_cast<std::uint8_t>((x * 37U + y * 11U + 7U) % 256U);
                data[i + 1] = static_cast<std::uint8_t>((x * 17U + y * 53U + 91U) % 256U);
                data[i + 2] = static_cast<std::uint8_t>((x * 97U + y * 29U + 23U) % 256U);
                data[i + 3] = static_cast<std::uint8_t>(200U + (x + y) % 56U);
            }
        return data;
    }

    std::vector<std::uint8_t> uniformBytes(std::size_t width,
                                           std::size_t height,
                                           std::size_t channels,
                                           std::uint8_t value)
    {
        return std::vector<std::uint8_t>(width * height * channels, value);
    }

    std::vector<std::uint8_t> rgbaUniform(std::size_t width,
                                          std::size_t height,
                                          std::uint8_t r,
                                          std::uint8_t g,
                                          std::uint8_t b,
                                          std::uint8_t a)
    {
        std::vector<std::uint8_t> data(width * height * 4U);
        for (std::size_t p = 0; p < width * height; ++p)
        {
            data[p * 4U + 0] = r;
            data[p * 4U + 1] = g;
            data[p * 4U + 2] = b;
            data[p * 4U + 3] = a;
        }
        return data;
    }

    bool sameBytes(const std::vector<std::uint8_t>& a,
                   const std::vector<std::uint8_t>& b,
                   int tolerance = 0)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) > tolerance)
                return false;
        return true;
    }

    bool sameFloats(const std::vector<float>& a, const std::vector<float>& b, float tolerance)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::fabs(a[i] - b[i]) > tolerance)
                return false;
        return true;
    }

    bool channelNear(float actual, std::uint8_t expected, int tolerance = byteTolerance)
    {
        return std::abs(static_cast<int>(toByte(actual)) - static_cast<int>(expected)) <= tolerance;
    }

    bool colorNear(const sr::Color& color, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
    {
        return channelNear(color.r, r) && channelNear(color.g, g) &&
            channelNear(color.b, b) && channelNear(color.a, a);
    }

    bool textureLevelMatchesChain(const sr::Texture& texture,
                                  const std::vector<RefLevel>& chain)
    {
        if (texture.getLevelCount() != chain.size())
            return false;

        for (std::size_t level = 0; level < chain.size(); ++level)
        {
            if (texture.getWidth(static_cast<std::uint32_t>(level)) != chain[level].width)
                return false;
            if (texture.getHeight(static_cast<std::uint32_t>(level)) != chain[level].height)
                return false;
            if (texture.getData(static_cast<std::uint32_t>(level)) != chain[level].bytes)
                return false;
        }

        return true;
    }

    bool textureLevelIsUniform(const sr::Texture& texture,
                               std::uint32_t level,
                               std::uint8_t r,
                               std::uint8_t g,
                               std::uint8_t b,
                               std::uint8_t a)
    {
        const auto& data = texture.getData(level);
        const auto expected = rgbaUniform(texture.getWidth(level), texture.getHeight(level), r, g, b, a);
        return sameBytes(data, expected);
    }

    // ------------------------------------------------------------------
    // Draw plumbing
    // ------------------------------------------------------------------

    sr::VertexShaderOutput vsPassthrough(const sr::Matrix<float, 4>&,
                                         const sr::Vertex& vertex)
    {
        return {vertex.position, vertex.color,
                {vertex.texCoords[0], vertex.texCoords[1]}, vertex.normal};
    }

    sr::Color fsTexture(const sr::VertexShaderOutput& input,
                        const std::array<const sr::Sampler*, 2>& samplers,
                        const std::array<const sr::Texture*, 2>& textures)
    {
        return textures[0]->sample(samplers[0], input.texCoords[0]);
    }

    sr::Color fsVertexColor(const sr::VertexShaderOutput& input,
                            const std::array<const sr::Sampler*, 2>&,
                            const std::array<const sr::Texture*, 2>&)
    {
        return input.color;
    }

    sr::Vertex makeVertex(float x, float y, float z,
                          const sr::Color& color,
                          float u, float v)
    {
        sr::Vertex vertex;
        vertex.position = sr::Vector<float, 4>{x, y, z, 1.0F};
        vertex.color = color;
        vertex.texCoords[0] = sr::Vector<float, 2>{u, v};
        vertex.texCoords[1] = sr::Vector<float, 2>{0.0F, 0.0F};
        vertex.normal = sr::Vector<float, 3>{0.0F, 0.0F, 1.0F};
        return vertex;
    }

    // Covers the whole viewport. Texcoords at NDC corners are 0 and 2, so a
    // pixel at (x, y) of a W x H target gets uv (x / W, y / H).
    std::vector<sr::Vertex> viewportTriangle(float z, const sr::Color& color)
    {
        return {
            makeVertex(-1.0F, -1.0F, z, color, 0.0F, 0.0F),
            makeVertex(3.0F, -1.0F, z, color, 2.0F, 0.0F),
            makeVertex(-1.0F, 3.0F, z, color, 0.0F, 2.0F)
        };
    }

    void fullDraw(const sr::TextureView& color,
                  const sr::TextureView& depth,
                  sr::FragmentShader fragmentShader,
                  const sr::Sampler* sampler,
                  const sr::Texture* inputTexture,
                  const sr::Rect<float>& viewport,
                  const sr::Rect<float>& scissor,
                  const sr::BlendState& blend,
                  const sr::DepthState& depthState,
                  const std::vector<sr::Vertex>& vertices)
    {
        sr::drawTriangles(color, depth, vsPassthrough, fragmentShader,
                          {sampler, nullptr}, {inputTexture, nullptr},
                          viewport, scissor, blend, depthState,
                          {0, 1, 2}, vertices, sr::Matrix<float, 4>{});
    }

    sr::BlendState alphaBlend()
    {
        sr::BlendState state;
        state.enabled = true;
        state.colorBlendSource = sr::BlendState::Factor::srcAlpha;
        state.colorBlendDest = sr::BlendState::Factor::invSrcAlpha;
        state.colorOperation = sr::BlendState::Operation::add;
        state.alphaBlendSource = sr::BlendState::Factor::one;
        state.alphaBlendDest = sr::BlendState::Factor::zero;
        state.alphaOperation = sr::BlendState::Operation::add;
        return state;
    }

    // ------------------------------------------------------------------
    // Storage cases
    // ------------------------------------------------------------------

    int storageNonSquareLevels()
    {
        sr::Texture texture(sr::PixelFormat::rgba8, 7, 3, true);

        TASK_CHECK(texture.getLevelCount() == 3);
        TASK_CHECK(texture.getWidth(0) == 7 && texture.getHeight(0) == 3);
        TASK_CHECK(texture.getWidth(1) == 3 && texture.getHeight(1) == 1);
        TASK_CHECK(texture.getWidth(2) == 1 && texture.getHeight(2) == 1);
        TASK_CHECK(texture.getData(0).size() == 7 * 3 * 4);
        TASK_CHECK(texture.getData(1).size() == 3 * 1 * 4);
        TASK_CHECK(texture.getData(2).size() == 1 * 1 * 4);

        bool threw = false;
        try { (void) texture.getWidth(3); } catch (const sr::RenderError&) { threw = true; }
        TASK_CHECK(threw);

        threw = false;
        try { (void) texture.getHeight(3); } catch (const sr::RenderError&) { threw = true; }
        TASK_CHECK(threw);

        // A single-row texture still halves its width down to 1x1.
        sr::Texture row(sr::PixelFormat::rgba8, 1, 6, true);
        TASK_CHECK(row.getLevelCount() == 3);
        TASK_CHECK(row.getWidth(1) == 1 && row.getHeight(1) == 3);
        TASK_CHECK(row.getWidth(2) == 1 && row.getHeight(2) == 1);

        // Mip maps disabled: only the base level exists, whatever the shape.
        sr::Texture plain(sr::PixelFormat::rgba8, 7, 3, false);
        TASK_CHECK(plain.getLevelCount() == 1);
        TASK_CHECK(plain.getWidth(0) == 7 && plain.getHeight(0) == 3);

        // Zero-sized textures have no levels at all.
        sr::Texture empty(sr::PixelFormat::rgba8, 0, 5, true);
        TASK_CHECK(empty.getLevelCount() == 0);
        bool emptyThrew = false;
        try { (void) empty.getWidth(0); } catch (const sr::RenderError&) { emptyThrew = true; }
        TASK_CHECK(emptyThrew);

        // A zero-width texture has no levels either.
        sr::Texture zeroRow(sr::PixelFormat::rgba8, 6, 0, true);
        TASK_CHECK(zeroRow.getLevelCount() == 0);

        return 0;
    }

    int storageResizeReallocates()
    {
        sr::Texture texture(sr::PixelFormat::rgba8, 4, 2, true);
        TASK_CHECK(texture.getLevelCount() == 3);
        TASK_CHECK(texture.getWidth(1) == 2 && texture.getHeight(1) == 1);

        texture.resize(8, 2);
        TASK_CHECK(texture.getLevelCount() == 4);
        TASK_CHECK(texture.getWidth(0) == 8 && texture.getHeight(0) == 2);
        TASK_CHECK(texture.getWidth(1) == 4 && texture.getHeight(1) == 1);
        TASK_CHECK(texture.getWidth(2) == 2 && texture.getHeight(2) == 1);
        TASK_CHECK(texture.getWidth(3) == 1 && texture.getHeight(3) == 1);
        TASK_CHECK(texture.getData(0).size() == 8 * 2 * 4);
        TASK_CHECK(texture.getData(1).size() == 4 * 1 * 4);

        sr::Texture plain(sr::PixelFormat::rgba8, 8, 8, false);
        TASK_CHECK(plain.getLevelCount() == 1);
        plain.resize(3, 2);
        TASK_CHECK(plain.getLevelCount() == 1);
        TASK_CHECK(plain.getWidth(0) == 3 && plain.getHeight(0) == 2);
        TASK_CHECK(plain.getData(0).size() == 3 * 2 * 4);

        return 0;
    }

    int storageSetDataValidation()
    {
        sr::Texture texture(sr::PixelFormat::rgba8, 4, 2, true);
        TASK_CHECK(texture.getLevelCount() == 3);

        bool threw = false;
        try
        {
            // Level 1 is 2x1; a base-sized upload must be rejected.
            texture.setData(std::vector<std::uint8_t>(4 * 2 * 4, 1U), 1);
        }
        catch (const sr::RenderError&) { threw = true; }
        TASK_CHECK(threw);
        TASK_CHECK(texture.getLevelCount() == 3);

        threw = false;
        try { texture.setData(std::vector<std::uint8_t>(4, 1U), 9); }
        catch (const sr::RenderError&) { threw = true; }
        TASK_CHECK(threw);
        TASK_CHECK(texture.getLevelCount() == 3);

        // Correct per-level size is accepted and regenerates the chain below.
        const auto level1 = rgbaUniform(2, 1, 10, 20, 30, 40);
        texture.setData(level1, 1);
        TASK_CHECK(texture.getData(1) == level1);
        TASK_CHECK(textureLevelIsUniform(texture, 2, 10, 20, 30, 40));

        // getData edits are the caller's responsibility; generateMipMaps is
        // explicit and starts at the requested level.
        auto& base = texture.getData(0);
        for (std::size_t i = 0; i < base.size(); i += 4)
        {
            base[i + 0] = 100;
            base[i + 1] = 110;
            base[i + 2] = 120;
            base[i + 3] = 130;
        }
        TASK_CHECK(!textureLevelIsUniform(texture, 1, 100, 110, 120, 130));
        texture.generateMipMaps(0);
        TASK_CHECK(textureLevelIsUniform(texture, 1, 100, 110, 120, 130));
        TASK_CHECK(textureLevelIsUniform(texture, 2, 100, 110, 120, 130));

        // getPixel addressing uses the level's own dimensions.
        sr::Texture pixelTexture(sr::PixelFormat::rgba8, 4, 2, true);
        pixelTexture.setData(rgbaUniform(2, 1, 1, 2, 3, 4), 1);
        const auto pixel = pixelTexture.getPixel(1, 0, 1);
        TASK_CHECK(colorNear(pixel, 1, 2, 3, 4));

        return 0;
    }

    int storageChainFormats()
    {
        const auto width = 5U;
        const auto height = 3U;

        // rgba8
        {
            const auto base = rgbaGradient(width, height);
            const auto chain = buildChain(width, height, 4, base);
            sr::Texture texture(sr::PixelFormat::rgba8, width, height, true);
            texture.setData(base);
            TASK_CHECK(texture.getLevelCount() == chain.size());
            TASK_CHECK(textureLevelMatchesChain(texture, chain));
        }

        // r8
        {
            std::vector<std::uint8_t> base(width * height);
            for (std::size_t i = 0; i < base.size(); ++i)
                base[i] = static_cast<std::uint8_t>((i * 37U + 13U) % 256U);

            const auto chain = buildChain(width, height, 1, base);
            sr::Texture texture(sr::PixelFormat::r8, width, height, true);
            texture.setData(base);
            TASK_CHECK(textureLevelMatchesChain(texture, chain));

            // r8 replicates red and forces opaque alpha in getPixel.
            const auto pixel = texture.getPixel(1, 1, 0);
            TASK_CHECK(colorNear(pixel, base[1 * width + 1], base[1 * width + 1],
                                 base[1 * width + 1], 255U));
        }

        // a8
        {
            std::vector<std::uint8_t> base(width * height);
            for (std::size_t i = 0; i < base.size(); ++i)
                base[i] = static_cast<std::uint8_t>((i * 23U + 7U) % 256U);

            const auto chain = buildChain(width, height, 1, base);
            sr::Texture texture(sr::PixelFormat::a8, width, height, true);
            texture.setData(base);
            TASK_CHECK(textureLevelMatchesChain(texture, chain));
        }

        // float32: plain average of the raw values, no clamping.
        {
            std::vector<float> base(width * height);
            for (std::size_t i = 0; i < base.size(); ++i)
                base[i] = static_cast<float>(i) * 3.25F - 40.0F;

            const auto chain = buildFloatChain(width, height, base);
            sr::Texture texture(sr::PixelFormat::float32, width, height, true);
            std::vector<std::uint8_t> bytes(sizeof(float) * base.size());
            std::memcpy(bytes.data(), base.data(), bytes.size());
            texture.setData(bytes);

            TASK_CHECK(texture.getLevelCount() == chain.size());
            for (std::size_t level = 0; level < chain.size(); ++level)
            {
                const auto& storage = texture.getData(static_cast<std::uint32_t>(level));
                std::vector<float> values(storage.size() / sizeof(float));
                std::memcpy(values.data(), storage.data(), storage.size());
                TASK_CHECK(sameFloats(values, chain[level], 1e-5F));
            }

            const auto deep = texture.getPixel(0, 0, 1);
            TASK_CHECK(std::fabs(deep.r - chain[1][0]) <= 1e-5F);
        }

        return 0;
    }

    // ------------------------------------------------------------------
    // Sampling cases
    // ------------------------------------------------------------------

    int samplingLodSelection()
    {
        sr::Texture texture(sr::PixelFormat::rgba8, 8, 4, true);
        TASK_CHECK(texture.getLevelCount() == 4);

        // Build distinct uniform levels: L0=A, L1=B, L2=C, L3=D.
        texture.setData(rgbaUniform(8, 4, 10, 20, 30, 40), 0);
        texture.setData(rgbaUniform(4, 2, 100, 110, 120, 130), 1);
        texture.setData(rgbaUniform(2, 1, 200, 210, 220, 230), 2);
        texture.setData(rgbaUniform(1, 1, 250, 240, 230, 220), 3);
        TASK_CHECK(textureLevelIsUniform(texture, 0, 10, 20, 30, 40));
        TASK_CHECK(textureLevelIsUniform(texture, 1, 100, 110, 120, 130));
        TASK_CHECK(textureLevelIsUniform(texture, 2, 200, 210, 220, 230));
        TASK_CHECK(textureLevelIsUniform(texture, 3, 250, 240, 230, 220));

        sr::Sampler sampler;
        sampler.filter = sr::Sampler::Filter::point;
        sampler.mipFilter = sr::Sampler::Filter::point;
        const sr::Vector<float, 2> uv{0.5F, 0.5F};

        // Default request keeps the base level.
        sampler.lod = 0.0F;
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 10, 20, 30, 40));

        sampler.lod = 0.4F;
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 10, 20, 30, 40));

        sampler.lod = 0.5F;   // point LOD rounds
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 100, 110, 120, 130));

        sampler.lod = 1.49F;
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 100, 110, 120, 130));

        sampler.lod = 1.5F;
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 200, 210, 220, 230));

        sampler.lod = 2.7F;
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 250, 240, 230, 220));

        // LOD is clamped to the existing chain.
        sampler.lod = 50.0F;
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 250, 240, 230, 220));
        sampler.lod = -7.0F;
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 10, 20, 30, 40));

        // Linear mip filtering blends the adjacent levels.
        sampler.mipFilter = sr::Sampler::Filter::linear;
        sampler.lod = 0.25F;
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 32, 42, 52, 62));
        sampler.lod = 1.0F;
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 100, 110, 120, 130));
        sampler.lod = 1.5F;
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 150, 160, 170, 180));
        sampler.lod = 9.0F;
        TASK_CHECK(colorNear(texture.sample(&sampler, uv), 250, 240, 230, 220));

        // Every uv resolves inside the chosen level: uniform level => same color.
        sampler.mipFilter = sr::Sampler::Filter::point;
        sampler.lod = 1.2F;
        TASK_CHECK(colorNear(texture.sample(&sampler, sr::Vector<float, 2>{0.0F, 0.0F}),
                             100, 110, 120, 130));
        TASK_CHECK(colorNear(texture.sample(&sampler, sr::Vector<float, 2>{1.0F, 1.0F}),
                             100, 110, 120, 130));

        return 0;
    }

    int samplingNonSquareInLevel()
    {
        const auto width = 5U;
        const auto height = 3U;
        const auto base = rgbaGradient(width, height);

        sr::Texture texture(sr::PixelFormat::rgba8, width, height, false);
        texture.setData(base);

        sr::Sampler sampler;
        sampler.filter = sr::Sampler::Filter::point;

        // point: round(uv * (N - 1)) per axis.
        const auto point = texture.sample(&sampler, sr::Vector<float, 2>{0.5F, 0.5F});
        const auto* expected = &base[(1 * width + 2) * 4];
        TASK_CHECK(colorNear(point, expected[0], expected[1], expected[2], expected[3]));

        // clamp addresses the edge.
        const auto edge = texture.sample(&sampler, sr::Vector<float, 2>{-1.0F, -1.0F});
        TASK_CHECK(colorNear(edge, base[0], base[1], base[2], base[3]));

        // linear: bilinear between floor/ceil neighbours of uv * (N - 1).
        sampler.filter = sr::Sampler::Filter::linear;
        const auto linear = texture.sample(&sampler, sr::Vector<float, 2>{0.375F, 0.5F});
        // u = 0.375 * 4 = 1.5 -> pixels 1 and 2, v = 0.5 * 2 = 1 -> row 1.
        const auto expectLinear = [&base](std::size_t x0, std::size_t x1, std::size_t y)
        {
            std::array<float, 4> result{};
            for (std::size_t c = 0; c < 4; ++c)
                result[c] = (static_cast<float>(base[(y * 5 + x0) * 4 + c]) +
                             static_cast<float>(base[(y * 5 + x1) * 4 + c])) / 2.0F;
            return result;
        };
        const auto expectedLinear = expectLinear(1, 2, 1);
        TASK_CHECK(colorNear(linear,
                             toByte(expectedLinear[0] / 255.0F),
                             toByte(expectedLinear[1] / 255.0F),
                             toByte(expectedLinear[2] / 255.0F),
                             toByte(expectedLinear[3] / 255.0F)));

        // Linear sampling clamps at the far edge.
        const auto farEdge = texture.sample(&sampler, sr::Vector<float, 2>{1.0F, 1.0F});
        const auto* corner = &base[((height - 1) * width + (width - 1)) * 4];
        TASK_CHECK(colorNear(farEdge, corner[0], corner[1], corner[2], corner[3]));

        return 0;
    }

    int samplingFractionalLodNonSquare()
    {
        // 3 level chain: 6x2 -> 3x1 -> 1x1.
        sr::Texture texture(sr::PixelFormat::rgba8, 6, 2, true);
        TASK_CHECK(texture.getLevelCount() == 3);

        const auto base = rgbaGradient(6, 2);
        texture.setData(base);
        const auto chain = buildChain(6, 2, 4, base);
        TASK_CHECK(textureLevelMatchesChain(texture, chain));

        sr::Sampler sampler;
        sampler.filter = sr::Sampler::Filter::point;
        sampler.mipFilter = sr::Sampler::Filter::point;

        const std::array<sr::Vector<float, 2>, 4> coords{
            sr::Vector<float, 2>{0.0F, 0.0F},
            sr::Vector<float, 2>{0.5F, 0.5F},
            sr::Vector<float, 2>{1.0F, 1.0F},
            sr::Vector<float, 2>{0.25F, 0.8F}
        };

        for (const auto lod : {0.0F, 0.49F, 1.5F, 2.0F, 7.0F})
        {
            sampler.lod = lod;
            const auto level = refLodIndex(lod, chain.size());
            for (const auto& uv : coords)
            {
                const auto expected = refPointSample(chain, level, uv.v[0], uv.v[1]);
                const auto actual = texture.sample(&sampler, uv);
                TASK_CHECK(colorNear(actual, expected[0], expected[1], expected[2], expected[3]));
            }
        }

        // Linear mip between a non-square base and its half-size successor.
        sampler.mipFilter = sr::Sampler::Filter::linear;
        sampler.lod = 0.5F;
        const sr::Vector<float, 2> uv{0.5F, 0.5F};
        auto lowerSampler = sampler;
        lowerSampler.mipFilter = sr::Sampler::Filter::point;
        lowerSampler.lod = 0.0F;
        auto upperSampler = sampler;
        upperSampler.mipFilter = sr::Sampler::Filter::point;
        upperSampler.lod = 1.0F;
        const auto lower = texture.sample(&lowerSampler, uv);
        const auto upper = texture.sample(&upperSampler, uv);
        const auto blended = texture.sample(&sampler, uv);
        TASK_CHECK(channelNear(blended.r, toByte((lower.r + upper.r) / 2.0F)));
        TASK_CHECK(channelNear(blended.g, toByte((lower.g + upper.g) / 2.0F)));
        TASK_CHECK(channelNear(blended.b, toByte((lower.b + upper.b) / 2.0F)));
        TASK_CHECK(channelNear(blended.a, toByte((lower.a + upper.a) / 2.0F)));

        return 0;
    }

    // ------------------------------------------------------------------
    // Feedback cases
    // ------------------------------------------------------------------

    int feedbackDrawUpdatesOnlyLevelAndSuccessors()
    {
        sr::Texture color(sr::PixelFormat::rgba8, 8, 4, true);
        const auto gradient = rgbaGradient(8, 4);
        color.setData(gradient);

        sr::Texture depth(sr::PixelFormat::float32, 8, 4, true);
        sr::clear(depth, 1.0F);

        const auto baseSnapshot = color.getData(0);

        // Clearing level 1 (4x2) must leave level 0 alone and rebuild successors.
        sr::clear(sr::TextureView{color, 1}, byteColor(0, 0, 255, 255));
        TASK_CHECK(color.getData(0) == baseSnapshot);
        TASK_CHECK(textureLevelIsUniform(color, 1, 0, 0, 255, 255));
        TASK_CHECK(textureLevelIsUniform(color, 2, 0, 0, 255, 255));
        TASK_CHECK(textureLevelIsUniform(color, 3, 0, 0, 255, 255));

        const auto level0Snapshot = color.getData(0);
        const auto level1Snapshot = color.getData(1);

        // Draw into level 2 (2x1); only it and level 3 may change.
        sr::DepthState noDepth;
        const auto vertices = viewportTriangle(0.5F, byteColor(255, 0, 0, 255));
        fullDraw(sr::TextureView{color, 2}, sr::TextureView{depth, 2},
                 fsVertexColor, nullptr, nullptr,
                 sr::Rect<float>{0.0F, 0.0F, 2.0F, 1.0F},
                 sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                 sr::BlendState{}, noDepth, vertices);

        TASK_CHECK(color.getData(0) == level0Snapshot);
        TASK_CHECK(color.getData(1) == level1Snapshot);
        TASK_CHECK(textureLevelIsUniform(color, 2, 255, 0, 0, 255));
        TASK_CHECK(textureLevelIsUniform(color, 3, 255, 0, 0, 255));

        // Depth level 2 was not written (depth test disabled) and stays clear.
        const auto depthStorage = depth.getData(2);
        const auto* depthValues = reinterpret_cast<const float*>(depthStorage.data());
        TASK_CHECK(std::fabs(depthValues[0] - 1.0F) <= 1e-6F);
        TASK_CHECK(std::fabs(depthValues[1] - 1.0F) <= 1e-6F);

        const auto& depthDeep = depth.getData(3);
        TASK_CHECK(std::fabs(*reinterpret_cast<const float*>(depthDeep.data()) - 1.0F) <= 1e-6F);

        return 0;
    }

    int feedbackSamplingSnapshotSameLevel()
    {
        const auto width = 8U;
        const auto height = 4U;
        const auto original = rgbaGradient(width, height);

        sr::Texture aliased(sr::PixelFormat::rgba8, width, height, true);
        aliased.setData(original);

        // Independent copies with identical contents.
        sr::Texture inputCopy(sr::PixelFormat::rgba8, width, height, true);
        inputCopy.setData(original);
        sr::Texture plainTarget(sr::PixelFormat::rgba8, width, height, true);
        plainTarget.setData(original);

        const auto chain = buildChain(width, height, 4, original);

        sr::Sampler sampler;
        sampler.filter = sr::Sampler::Filter::point;
        sampler.mipFilter = sr::Sampler::Filter::point;
        sampler.lod = 0.0F;

        sr::DepthState noDepth;
        const auto vertices = viewportTriangle(0.5F, byteColor(255, 255, 255, 255));

        // Aliased: the same texture is color target and sampled input.
        sr::Texture aliasedDepth(sr::PixelFormat::float32, width, height, true);
        sr::clear(aliasedDepth, 1.0F);
        fullDraw(sr::TextureView{aliased, 0}, sr::TextureView{aliasedDepth, 0},
                 fsTexture, &sampler, &aliased,
                 sr::Rect<float>{0.0F, 0.0F, 8.0F, 4.0F},
                 sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                 sr::BlendState{}, noDepth, vertices);

        // Non-aliased: separate target reading an independent input copy.
        sr::Texture plainDepth(sr::PixelFormat::float32, width, height, true);
        sr::clear(plainDepth, 1.0F);
        fullDraw(sr::TextureView{plainTarget, 0}, sr::TextureView{plainDepth, 0},
                 fsTexture, &sampler, &inputCopy,
                 sr::Rect<float>{0.0F, 0.0F, 8.0F, 4.0F},
                 sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                 sr::BlendState{}, noDepth, vertices);

        TASK_CHECK(aliased.getData(0) == plainTarget.getData(0));

        // Every fragment must have read the draw-start base level.
        const auto& result = aliased.getData(0);
        bool allAsExpected = true;
        for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x)
            {
                const auto expected = refPointSample(chain, 0,
                    static_cast<float>(x) / static_cast<float>(width),
                    static_cast<float>(y) / static_cast<float>(height));
                const auto* pixel = &result[(y * width + x) * 4];
                for (std::size_t c = 0; c < 4; ++c)
                    if (std::abs(static_cast<int>(pixel[c]) - static_cast<int>(expected[c])) > byteTolerance)
                        allAsExpected = false;
            }
        TASK_CHECK(allAsExpected);

        // The regenerated chain also matches the non-aliased draw.
        for (std::size_t level = 0; level < aliased.getLevelCount(); ++level)
            TASK_CHECK(aliased.getData(static_cast<std::uint32_t>(level)) ==
                       plainTarget.getData(static_cast<std::uint32_t>(level)));

        return 0;
    }

    int feedbackSamplingSnapshotMipChain()
    {
        const auto width = 8U;
        const auto height = 4U;
        const auto original = rgbaGradient(width, height);

        sr::Texture aliased(sr::PixelFormat::rgba8, width, height, true);
        aliased.setData(original);

        sr::Texture inputCopy(sr::PixelFormat::rgba8, width, height, true);
        inputCopy.setData(original);
        sr::Texture plainTarget(sr::PixelFormat::rgba8, width, height, true);
        plainTarget.setData(original);

        const auto chain = buildChain(width, height, 4, original);

        sr::Sampler sampler;
        sampler.filter = sr::Sampler::Filter::point;
        sampler.mipFilter = sr::Sampler::Filter::point;
        sampler.lod = 1.0F; // read level 1 while writing level 0

        sr::DepthState noDepth;
        const auto vertices = viewportTriangle(0.5F, byteColor(255, 255, 255, 255));
        const sr::Rect<float> viewport{0.0F, 0.0F, 8.0F, 4.0F};
        const sr::Rect<float> scissor{0.0F, 0.0F, 1.0F, 1.0F};

        sr::Texture aliasedDepth(sr::PixelFormat::float32, width, height, true);
        sr::clear(aliasedDepth, 1.0F);
        fullDraw(sr::TextureView{aliased, 0}, sr::TextureView{aliasedDepth, 0},
                 fsTexture, &sampler, &aliased, viewport, scissor,
                 sr::BlendState{}, noDepth, vertices);

        sr::Texture plainDepth(sr::PixelFormat::float32, width, height, true);
        sr::clear(plainDepth, 1.0F);
        fullDraw(sr::TextureView{plainTarget, 0}, sr::TextureView{plainDepth, 0},
                 fsTexture, &sampler, &inputCopy, viewport, scissor,
                 sr::BlendState{}, noDepth, vertices);

        TASK_CHECK(aliased.getData(0) == plainTarget.getData(0));

        const auto& result = aliased.getData(0);
        bool allAsExpected = true;
        for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x)
            {
                const auto expected = refPointSample(chain, 1,
                    static_cast<float>(x) / static_cast<float>(width),
                    static_cast<float>(y) / static_cast<float>(height));
                const auto* pixel = &result[(y * width + x) * 4];
                for (std::size_t c = 0; c < 4; ++c)
                    if (std::abs(static_cast<int>(pixel[c]) - static_cast<int>(expected[c])) > byteTolerance)
                        allAsExpected = false;
            }
        TASK_CHECK(allAsExpected);

        return 0;
    }

    int feedbackDepthBlendScissorOnLevel()
    {
        sr::Texture color(sr::PixelFormat::rgba8, 8, 4, true);
        sr::clear(color, byteColor(0, 0, 0, 255));

        sr::Texture depth(sr::PixelFormat::float32, 8, 4, true);
        sr::clear(depth, 1.0F);

        const sr::TextureView colorView{color, 1}; // 4x2
        const sr::TextureView depthView{depth, 1}; // 4x2
        TASK_CHECK(colorView.getWidth() == 4 && colorView.getHeight() == 2);
        TASK_CHECK(depthView.getWidth() == 4 && depthView.getHeight() == 2);

        const sr::Color source{0.8F, 0.4F, 0.2F, 0.5F};
        const auto vertices = viewportTriangle(0.5F, source);

        sr::DepthState depthState;
        depthState.read = true;
        depthState.write = true;

        fullDraw(colorView, depthView, fsVertexColor, nullptr, nullptr,
                 sr::Rect<float>{0.0F, 0.0F, 4.0F, 2.0F},
                 sr::Rect<float>{0.25F, 0.25F, 0.5F, 0.5F},
                 alphaBlend(), depthState, vertices);

        // scissor: x in [floor(.25*4), ceil(.75*4)) = [1, 3), y in [0, 2).
        const auto& colorStorage = color.getData(1);
        const auto& depthStorage = depth.getData(1);
        const auto* depthValues = reinterpret_cast<const float*>(depthStorage.data());

        const std::uint8_t expectedR = toByte(0.4F);
        const std::uint8_t expectedG = toByte(0.2F);
        const std::uint8_t expectedB = toByte(0.1F);
        const std::uint8_t expectedA = toByte(0.5F);

        for (std::size_t y = 0; y < 2; ++y)
            for (std::size_t x = 0; x < 4; ++x)
            {
                const auto index = (y * 4 + x) * 4U;
                const auto inside = (x >= 1 && x < 3);
                const auto* pixel = &colorStorage[index];
                if (inside)
                {
                    TASK_CHECK(channelNear(pixel[0] / 255.0F, expectedR));
                    TASK_CHECK(channelNear(pixel[1] / 255.0F, expectedG));
                    TASK_CHECK(channelNear(pixel[2] / 255.0F, expectedB));
                    TASK_CHECK(channelNear(pixel[3] / 255.0F, expectedA));
                    TASK_CHECK(std::fabs(depthValues[y * 4 + x] - 0.5F) <= 1e-6F);
                }
                else
                {
                    TASK_CHECK(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
                    TASK_CHECK(std::fabs(depthValues[y * 4 + x] - 1.0F) <= 1e-6F);
                }
            }

        // Ancestor level is untouched by the level-1 draw.
        const auto& level0 = color.getData(0);
        bool level0Black = true;
        for (std::size_t i = 0; i < level0.size(); i += 4)
            if (level0[i] != 0 || level0[i + 1] != 0 || level0[i + 2] != 0 || level0[i + 3] != 255)
                level0Black = false;
        TASK_CHECK(level0Black);

        return 0;
    }

    int feedbackLegacyAndResizeViews()
    {
        // Legacy Texture& entry points still select level 0.
        sr::Texture color(sr::PixelFormat::rgba8, 6, 4, true);
        sr::Texture depth(sr::PixelFormat::float32, 6, 4, true);
        sr::clear(color, byteColor(0, 0, 0, 255));
        sr::clear(depth, 1.0F);

        const auto vertices = viewportTriangle(0.5F, byteColor(0, 255, 0, 255));
        sr::DepthState depthState;
        depthState.read = true;
        depthState.write = true;

        sr::drawTriangles(color, depth, vsPassthrough, fsVertexColor,
                          {nullptr, nullptr}, {nullptr, nullptr},
                          sr::Rect<float>{0.0F, 0.0F, 6.0F, 4.0F},
                          sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                          sr::BlendState{}, depthState,
                          {0, 1, 2}, vertices, sr::Matrix<float, 4>{});

        TASK_CHECK(textureLevelIsUniform(color, 0, 0, 255, 0, 255));
        // Base draw regenerated the successors from the new base.
        TASK_CHECK(textureLevelIsUniform(color, 1, 0, 255, 0, 255));

        // clear(Texture&) keeps working and selects level 0.
        sr::clear(color, byteColor(1, 2, 3, 4));
        TASK_CHECK(textureLevelIsUniform(color, 0, 1, 2, 3, 4));
        TASK_CHECK(textureLevelIsUniform(color, 1, 1, 2, 3, 4));

        // A view resolves against the owner's current dimensions after resize.
        sr::Texture resized(sr::PixelFormat::rgba8, 4, 4, true);
        sr::TextureView view{resized, 1};
        TASK_CHECK(view.getWidth() == 2 && view.getHeight() == 2);

        resized.resize(8, 8);
        TASK_CHECK(view.getWidth() == 4 && view.getHeight() == 4);
        TASK_CHECK(view.getData().size() == 4 * 4 * 4);

        const auto level0Before = resized.getData(0);
        sr::clear(view, byteColor(255, 255, 0, 255));
        TASK_CHECK(resized.getData(0) == level0Before);
        TASK_CHECK(textureLevelIsUniform(resized, 1, 255, 255, 0, 255));
        TASK_CHECK(textureLevelIsUniform(resized, 2, 255, 255, 0, 255));
        TASK_CHECK(textureLevelIsUniform(resized, 3, 255, 255, 0, 255));

        // And drawing through a view after resize uses the new level size.
        sr::Texture resizedDepth(sr::PixelFormat::float32, 8, 8, true);
        sr::clear(resizedDepth, 1.0F);
        sr::TextureView depthView{resizedDepth, 1};
        const auto yellowVertices = viewportTriangle(0.5F, byteColor(9, 8, 7, 255));
        fullDraw(view, depthView, fsVertexColor, nullptr, nullptr,
                 sr::Rect<float>{0.0F, 0.0F, 4.0F, 4.0F},
                 sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                 sr::BlendState{}, sr::DepthState{}, yellowVertices);
        TASK_CHECK(textureLevelIsUniform(resized, 1, 9, 8, 7, 255));

        return 0;
    }

    int feedbackValidation()
    {
        sr::Texture color(sr::PixelFormat::rgba8, 8, 4, true);
        sr::Texture depth(sr::PixelFormat::float32, 8, 4, true);
        sr::clear(color, byteColor(5, 6, 7, 8));
        sr::clear(depth, 0.25F);

        const auto colorBefore = color.getData(0);
        const auto depthBefore = depth.getData(0);
        const auto vertices = viewportTriangle(0.5F, byteColor(1, 2, 3, 4));

        // Level out of range: throws before any write.
        bool threw = false;
        try
        {
            fullDraw(sr::TextureView{color, 9}, sr::TextureView{depth, 0},
                     fsVertexColor, nullptr, nullptr,
                     sr::Rect<float>{0.0F, 0.0F, 8.0F, 4.0F},
                     sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                     sr::BlendState{}, sr::DepthState{}, vertices);
        }
        catch (const sr::RenderError&) { threw = true; }
        TASK_CHECK(threw);
        TASK_CHECK(color.getData(0) == colorBefore);

        // Color target must be rgba8.
        threw = false;
        try
        {
            fullDraw(sr::TextureView{depth, 0}, sr::TextureView{depth, 0},
                     fsVertexColor, nullptr, nullptr,
                     sr::Rect<float>{0.0F, 0.0F, 8.0F, 4.0F},
                     sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                     sr::BlendState{}, sr::DepthState{}, vertices);
        }
        catch (const sr::RenderError&) { threw = true; }
        TASK_CHECK(threw);

        // Depth target must be float32.
        threw = false;
        try
        {
            fullDraw(sr::TextureView{color, 0}, sr::TextureView{color, 0},
                     fsVertexColor, nullptr, nullptr,
                     sr::Rect<float>{0.0F, 0.0F, 8.0F, 4.0F},
                     sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                     sr::BlendState{}, sr::DepthState{}, vertices);
        }
        catch (const sr::RenderError&) { threw = true; }
        TASK_CHECK(threw);

        // Selected levels must be the same size.
        sr::Texture small(sr::PixelFormat::float32, 4, 2, true);
        threw = false;
        try
        {
            fullDraw(sr::TextureView{color, 0}, sr::TextureView{small, 0},
                     fsVertexColor, nullptr, nullptr,
                     sr::Rect<float>{0.0F, 0.0F, 8.0F, 4.0F},
                     sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                     sr::BlendState{}, sr::DepthState{}, vertices);
        }
        catch (const sr::RenderError&) { threw = true; }
        TASK_CHECK(threw);
        TASK_CHECK(color.getData(0) == colorBefore);
        TASK_CHECK(depth.getData(0) == depthBefore);

        // clear on a wrong-format view throws.
        threw = false;
        try { sr::clear(sr::TextureView{depth, 0}, byteColor(1, 2, 3, 4)); }
        catch (const sr::RenderError&) { threw = true; }
        TASK_CHECK(threw);

        // Level mismatch across color/depth views also throws.
        threw = false;
        try
        {
            fullDraw(sr::TextureView{color, 1}, sr::TextureView{depth, 0},
                     fsVertexColor, nullptr, nullptr,
                     sr::Rect<float>{0.0F, 0.0F, 4.0F, 2.0F},
                     sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                     sr::BlendState{}, sr::DepthState{}, vertices);
        }
        catch (const sr::RenderError&) { threw = true; }
        TASK_CHECK(threw);

        return 0;
    }
}

extern const struct task_case task_cases[] = {
    {"storage", "nonsquare_levels", storageNonSquareLevels},
    {"storage", "resize_reallocates", storageResizeReallocates},
    {"storage", "setdata_validation", storageSetDataValidation},
    {"storage", "chain_formats", storageChainFormats},
    {"sampling", "lod_selection", samplingLodSelection},
    {"sampling", "nonsquare_in_level", samplingNonSquareInLevel},
    {"sampling", "fractional_lod_nonsquare", samplingFractionalLodNonSquare},
    {"feedback", "level_and_successors", feedbackDrawUpdatesOnlyLevelAndSuccessors},
    {"feedback", "sampling_snapshot_same_level", feedbackSamplingSnapshotSameLevel},
    {"feedback", "sampling_snapshot_mip_chain", feedbackSamplingSnapshotMipChain},
    {"feedback", "depth_blend_scissor_level", feedbackDepthBlendScissorOnLevel},
    {"feedback", "legacy_and_resize_views", feedbackLegacyAndResizeViews},
    {"feedback", "validation", feedbackValidation}
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
