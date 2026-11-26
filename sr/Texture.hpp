//
//  SoftwareRenderer
//

#ifndef SR_TEXTURE_HPP
#define SR_TEXTURE_HPP

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include "PixelFormat.hpp"
#include "RenderError.hpp"
#include "Sampler.hpp"

namespace sr
{
    class Texture final
    {
    public:

        Texture(const PixelFormat initPixelFormat = PixelFormat::rgba8,
                const std::size_t initWidth = 0,
                const std::size_t initHeight = 0,
                const bool initMipMaps = false):
            pixelFormat{initPixelFormat},
            width{initWidth},
            height{initHeight},
            mipMaps{initMipMaps}
        {
            allocateLevels();
        }

        void resize(const std::size_t newWidth, const std::size_t newHeight)
        {
            const auto pixelSize = getPixelSize(pixelFormat);
            if (pixelSize == 0)
                throw RenderError{"Invalid pixel format"};

            width = newWidth;
            height = newHeight;
            allocateLevels();
        }

        [[nodiscard]] auto getPixelFormat() const noexcept { return pixelFormat; }
        [[nodiscard]] auto getWidth() const noexcept { return width; }
        [[nodiscard]] auto getHeight() const noexcept { return height; }

        [[nodiscard]] std::size_t getWidth(const std::uint32_t level) const
        {
            validateLevel(level);
            return levelExtent(width, level);
        }

        [[nodiscard]] std::size_t getHeight(const std::uint32_t level) const
        {
            validateLevel(level);
            return levelExtent(height, level);
        }

        [[nodiscard]] std::size_t getLevelCount() const noexcept
        {
            return levels.size();
        }

        [[nodiscard]] std::vector<std::uint8_t>& getData(const std::uint32_t level = 0)
        {
            validateLevel(level);
            return levels[level];
        }

        [[nodiscard]] const std::vector<std::uint8_t>& getData(const std::uint32_t level = 0) const
        {
            validateLevel(level);
            return levels[level];
        }

        void setData(const std::vector<std::uint8_t>& buffer,
                     const std::uint32_t level = 0)
        {
            const auto pixelSize = getPixelSize(pixelFormat);
            if (pixelSize == 0)
                throw RenderError{"Invalid pixel format"};

            validateLevel(level);

            if (buffer.size() != getWidth(level) * getHeight(level) * pixelSize)
                throw RenderError{"Invalid buffer size"};

            levels[level] = buffer;
            generateMipMaps(level);
        }

        // Rebuilds every level below (and including the successors of) startLevel
        // from the contents of startLevel. Ancestor levels are left untouched.
        void generateMipMaps(const std::uint32_t startLevel = 0)
        {
            validateLevel(startLevel);

            for (std::uint32_t level = startLevel + 1; level < levels.size(); ++level)
                generateMipLevel(level);
        }

        [[nodiscard]] Color getPixel(const std::size_t x,
                                     const std::size_t y,
                                     const std::uint32_t level) const
        {
            validateLevel(level);

            const auto& buffer = levels[level];
            const auto levelWidth = levelExtent(width, level);

            switch (pixelFormat)
            {
                case PixelFormat::r8:
                {
                    const auto* r = &buffer[(y * levelWidth + x) * 1];
                    return Color{*r, *r, *r, std::uint8_t(255U)};
                }
                case PixelFormat::a8:
                {
                    const auto* a = &buffer[(y * levelWidth + x) * 1];
                    return Color{std::uint8_t(0U), std::uint8_t(0U), std::uint8_t(0U), *a};
                }
                case PixelFormat::rgba8:
                {
                    const auto* rgba = &buffer[(y * levelWidth + x) * 4];
                    return Color{rgba[0], rgba[1], rgba[2], rgba[3]};
                }
                case PixelFormat::float32:
                {
                    const float f = reinterpret_cast<const float*>(buffer.data())[y * levelWidth + x];
                    return Color{f, f, f, 1.0F};
                }
                default:
                    throw RenderError{"Invalid pixel format"};
            }
        }

        [[nodiscard]] Color sample(const Sampler* sampler, const Vector<float, 2>& coord) const
        {
            if (!sampler || levels.empty())
                return Color{};

            const auto maxLevel = static_cast<std::uint32_t>(levels.size() - 1);

            float lod = std::isfinite(sampler->lod) ? sampler->lod : 0.0F;
            lod = std::clamp(lod, 0.0F, static_cast<float>(maxLevel));

            if (sampler->mipFilter == Sampler::Filter::linear && maxLevel > 0)
            {
                const auto lower = static_cast<std::uint32_t>(std::floor(lod));
                const auto upper = std::min(lower + 1, maxLevel);
                const auto factor = lod - std::floor(lod);

                const auto lowerColor = sampleLevel(sampler, coord, lower);
                if (upper == lower || factor <= 0.0F)
                    return lowerColor;

                return mix(lowerColor, sampleLevel(sampler, coord, upper), factor);
            }

            auto level = static_cast<std::uint32_t>(std::floor(lod + 0.5F));
            if (level > maxLevel) level = maxLevel;
            return sampleLevel(sampler, coord, level);
        }

    private:
        [[nodiscard]] static std::size_t levelExtent(std::size_t base, std::uint32_t level) noexcept
        {
            if (base == 0)
                return 0;

            while (level > 0)
            {
                if (base <= 1)
                    return 1;

                base >>= 1;
                --level;
            }

            return base;
        }

        void validateLevel(const std::uint32_t level) const
        {
            if (level >= levels.size())
                throw RenderError{"Invalid mip level"};
        }

        void allocateLevels()
        {
            levels.clear();

            const auto pixelSize = getPixelSize(pixelFormat);
            if (pixelSize == 0 || width == 0 || height == 0)
                return;

            levels.emplace_back(std::vector<std::uint8_t>(width * height * pixelSize));

            if (!mipMaps)
                return;

            auto levelWidth = width;
            auto levelHeight = height;

            while (levelWidth > 1 || levelHeight > 1)
            {
                levelWidth = std::max<std::size_t>(1, levelWidth >> 1);
                levelHeight = std::max<std::size_t>(1, levelHeight >> 1);

                levels.emplace_back(std::vector<std::uint8_t>(levelWidth * levelHeight * pixelSize));
            }
        }

        void generateMipLevel(const std::uint32_t level)
        {
            const auto parentWidth = getWidth(level - 1);
            const auto parentHeight = getHeight(level - 1);
            const auto childWidth = getWidth(level);
            const auto childHeight = getHeight(level);
            const auto pixelSize = getPixelSize(pixelFormat);

            const auto& parent = levels[level - 1];
            auto& child = levels[level];
            child.assign(childWidth * childHeight * pixelSize, std::uint8_t(0U));

            for (std::size_t cy = 0; cy < childHeight; ++cy)
            {
                const auto y0 = cy * parentHeight / childHeight;
                const auto y1 = (cy + 1) * parentHeight / childHeight;

                for (std::size_t cx = 0; cx < childWidth; ++cx)
                {
                    const auto x0 = cx * parentWidth / childWidth;
                    const auto x1 = (cx + 1) * parentWidth / childWidth;

                    if (pixelFormat == PixelFormat::float32)
                    {
                        const auto* parentData = reinterpret_cast<const float*>(parent.data());
                        auto* childData = reinterpret_cast<float*>(child.data());

                        double sum = 0.0;
                        std::size_t count = 0;

                        for (std::size_t py = y0; py < y1; ++py)
                            for (std::size_t px = x0; px < x1; ++px)
                            {
                                sum += parentData[py * parentWidth + px];
                                ++count;
                            }

                        childData[cy * childWidth + cx] = static_cast<float>(sum / static_cast<double>(count));
                    }
                    else
                    {
                        std::array<unsigned long long, 4> sums{};
                        std::size_t count = 0;

                        for (std::size_t py = y0; py < y1; ++py)
                            for (std::size_t px = x0; px < x1; ++px)
                            {
                                const auto* texel = &parent[(py * parentWidth + px) * pixelSize];
                                for (std::size_t channel = 0; channel < pixelSize; ++channel)
                                    sums[channel] += texel[channel];
                                ++count;
                            }

                        auto* texel = &child[(cy * childWidth + cx) * pixelSize];
                        for (std::size_t channel = 0; channel < pixelSize; ++channel)
                            texel[channel] = static_cast<std::uint8_t>(
                                std::lround(static_cast<double>(sums[channel]) / static_cast<double>(count)));
                    }
                }
            }
        }

        [[nodiscard]] static float address(const float coord,
                                           const Sampler::AddressMode mode,
                                           const std::size_t size) noexcept
        {
            if (size <= 1)
                return 0.0F;

            const auto extent = static_cast<float>(size - 1);

            switch (mode)
            {
                case Sampler::AddressMode::clamp:
                    return std::clamp(coord, 0.0F, 1.0F) * extent;
                case Sampler::AddressMode::repeat:
                    return std::fmod(coord, 1.0F) * extent;
                case Sampler::AddressMode::mirror:
                    return (1.0F - 2.0F * std::fabs(std::fmod(coord / 2.0F, 1.0F) - 0.5F)) * extent;
                default:
                    return 0.0F;
            }
        }

        [[nodiscard]] static Color mix(const Color& a, const Color& b, const float factor) noexcept
        {
            return Color{
                a.r + (b.r - a.r) * factor,
                a.g + (b.g - a.g) * factor,
                a.b + (b.b - a.b) * factor,
                a.a + (b.a - a.a) * factor
            };
        }

        [[nodiscard]] Color sampleLevel(const Sampler* sampler,
                                        const Vector<float, 2>& coord,
                                        const std::uint32_t level) const
        {
            const auto levelWidth = getWidth(level);
            const auto levelHeight = getHeight(level);

            if (levelWidth == 0 || levelHeight == 0)
                return Color{};

            const auto u = address(coord.v[0], sampler->addressModeX, levelWidth);
            const auto v = address(coord.v[1], sampler->addressModeY, levelHeight);

            if (sampler->filter == Sampler::Filter::point)
            {
                auto textureX = static_cast<std::size_t>(std::round(u));
                auto textureY = static_cast<std::size_t>(std::round(v));

                textureX = std::clamp(textureX, static_cast<std::size_t>(0U), levelWidth - 1);
                textureY = std::clamp(textureY, static_cast<std::size_t>(0U), levelHeight - 1);

                return getPixel(textureX, textureY, level);
            }

            const auto floorX = std::floor(u);
            const auto floorY = std::floor(v);

            auto textureX0 = static_cast<std::size_t>(std::max(0.0F, floorX));
            auto textureX1 = static_cast<std::size_t>(std::max(0.0F, std::ceil(u)));
            auto textureY0 = static_cast<std::size_t>(std::max(0.0F, floorY));
            auto textureY1 = static_cast<std::size_t>(std::max(0.0F, std::ceil(v)));

            textureX0 = std::clamp(textureX0, static_cast<std::size_t>(0U), levelWidth - 1);
            textureX1 = std::clamp(textureX1, static_cast<std::size_t>(0U), levelWidth - 1);
            textureY0 = std::clamp(textureY0, static_cast<std::size_t>(0U), levelHeight - 1);
            textureY1 = std::clamp(textureY1, static_cast<std::size_t>(0U), levelHeight - 1);

            const auto factorX = u - floorX;
            const auto factorY = v - floorY;

            const auto top = mix(getPixel(textureX0, textureY0, level),
                                 getPixel(textureX1, textureY0, level),
                                 factorX);
            const auto bottom = mix(getPixel(textureX0, textureY1, level),
                                    getPixel(textureX1, textureY1, level),
                                    factorX);

            return mix(top, bottom, factorY);
        }

        PixelFormat pixelFormat;
        std::size_t width = 0;
        std::size_t height = 0;
        bool mipMaps = false;
        std::vector<std::vector<std::uint8_t>> levels;
        std::uint32_t minLOD = 0;
        std::uint32_t maxLOD = UINT_MAX;
        float lodBias = 0.0F;
    };

    inline void clear(Texture& renderTarget, const Color color)
    {
        if (renderTarget.getPixelFormat() != PixelFormat::rgba8)
            throw RenderError{"Color render target must be rgba8"};

        const auto level = std::uint32_t(0U);
        auto& buffer = renderTarget.getData(level);

        const auto bufferData = reinterpret_cast<std::uint32_t*>(buffer.data());
        const auto rgba = color.getIntValueRaw();

        const auto bufferSize = renderTarget.getWidth(level) * renderTarget.getHeight(level);
        for (std::size_t p = 0; p < bufferSize; ++p)
            bufferData[p] = rgba;

        renderTarget.generateMipMaps(level);
    }

    inline void clear(Texture& renderTarget, const float depth)
    {
        if (renderTarget.getPixelFormat() != PixelFormat::float32)
            throw RenderError{"Depth render target must be float32"};

        const auto level = std::uint32_t(0U);
        auto& buffer = renderTarget.getData(level);

        const auto bufferData = reinterpret_cast<float*>(buffer.data());

        const auto bufferSize = renderTarget.getWidth(level) * renderTarget.getHeight(level);
        for (std::size_t p = 0; p < bufferSize; ++p)
            bufferData[p] = depth;

        renderTarget.generateMipMaps(level);
    }
}

#endif
