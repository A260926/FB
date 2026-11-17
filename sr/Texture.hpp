//
//  SoftwareRenderer
//

#ifndef SR_TEXTURE_HPP
#define SR_TEXTURE_HPP

#include <algorithm>
#include <cassert>
#include <cmath>
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
            width = newWidth;
            height = newHeight;
            allocateLevels();
        }

        [[nodiscard]] auto getPixelFormat() const noexcept { return pixelFormat; }
        [[nodiscard]] auto getWidth() const noexcept { return width; }
        [[nodiscard]] auto getHeight() const noexcept { return height; }
        [[nodiscard]] auto hasMipMaps() const noexcept { return mipMaps; }

        [[nodiscard]] std::size_t getWidth(const std::uint32_t level) const
        {
            if (level >= levels.size())
                throw RenderError{"Invalid mip level"};

            return levelWidth(level);
        }

        [[nodiscard]] std::size_t getHeight(const std::uint32_t level) const
        {
            if (level >= levels.size())
                throw RenderError{"Invalid mip level"};

            return levelHeight(level);
        }

        [[nodiscard]] std::size_t getLevelCount() const noexcept
        {
            return levels.size();
        }

        [[nodiscard]] std::vector<std::uint8_t>& getData(const std::uint32_t level = 0)
        {
            if (level >= levels.size())
                throw RenderError{"Invalid mip level"};

            return levels[level];
        }

        [[nodiscard]] const std::vector<std::uint8_t>& getData(const std::uint32_t level = 0) const
        {
            if (level >= levels.size())
                throw RenderError{"Invalid mip level"};

            return levels[level];
        }

        void setData(const std::vector<std::uint8_t>& buffer,
                     const std::uint32_t level = 0)
        {
            const auto pixelSize = getPixelSize(pixelFormat);
            if (pixelSize == 0)
                throw RenderError{"Invalid pixel format"};

            if (level >= levels.size())
                throw RenderError{"Invalid mip level"};

            if (buffer.size() != levelWidth(level) * levelHeight(level) * pixelSize)
                throw RenderError{"Invalid buffer size"};

            levels[level] = buffer;
            updateSuccessorMipMaps(level);
        }

        // Rebuilds every level above startLevel from its parent. The parent
        // levels themselves are never touched.
        void generateMipMaps(const std::uint32_t startLevel = 0)
        {
            if (!mipMaps || levels.empty())
                return;

            if (startLevel >= levels.size())
                throw RenderError{"Invalid mip level"};

            updateSuccessorMipMaps(startLevel);
        }

        [[nodiscard]] Color getPixel(const std::size_t x,
                                     const std::size_t y,
                                     const std::uint32_t level = 0) const
        {
            if (level >= levels.size())
                throw RenderError{"Invalid mip level"};

            const auto levelW = levelWidth(level);
            const auto levelH = levelHeight(level);

            if (x >= levelW || y >= levelH)
                throw RenderError{"Invalid pixel coordinates"};

            const auto& buffer = levels[level];

            switch (pixelFormat)
            {
                case PixelFormat::r8:
                {
                    const auto* r = &buffer[(y * levelW + x) * 1];
                    return Color{*r, *r, *r, std::uint8_t(255U)};
                }
                case PixelFormat::a8:
                {
                    const auto* a = &buffer[(y * levelW + x) * 1];
                    return Color{std::uint8_t(0U), std::uint8_t(0U), std::uint8_t(0U), *a};
                }
                case PixelFormat::rgba8:
                {
                    const auto* rgba = &buffer[(y * levelW + x) * 4];
                    return Color{rgba[0], rgba[1], rgba[2], rgba[3]};
                }
                case PixelFormat::float32:
                {
                    const float f = reinterpret_cast<const float*>(buffer.data())[y * levelW + x];
                    return Color{f, f, f, 1.0F};
                }
                default:
                    throw RenderError{"Invalid pixel format"};
            }
        }

        [[nodiscard]] Color sample(const Sampler* sampler,
                                   const Vector<float, 2>& coord,
                                   const float sampleLod) const
        {
            if (!sampler || levels.empty())
                return Color{};

            if (!std::isfinite(sampleLod))
                throw RenderError{"Invalid LOD"};

            const auto lastLevel = static_cast<float>(levels.size() - 1);
            const auto lod = std::clamp(sampleLod, 0.0F, lastLevel);

            if (sampler->mipFilter == Sampler::Filter::point || levels.size() == 1)
            {
                const auto level = static_cast<std::uint32_t>(
                    std::min(static_cast<std::size_t>(std::floor(lod + 0.5F)), levels.size() - 1));
                return sampleLevel(level, sampler, coord);
            }
            else
            {
                const auto level0 = static_cast<std::uint32_t>(std::floor(lod));
                const auto level1 = std::min(level0 + 1U,
                                             static_cast<std::uint32_t>(levels.size() - 1));
                const auto fraction = lod - static_cast<float>(level0);

                const Color color0 = sampleLevel(level0, sampler, coord);
                const Color color1 = sampleLevel(level1, sampler, coord);

                return Color{
                    color0.r + (color1.r - color0.r) * fraction,
                    color0.g + (color1.g - color0.g) * fraction,
                    color0.b + (color1.b - color0.b) * fraction,
                    color0.a + (color1.a - color0.a) * fraction
                };
            }
        }

        [[nodiscard]] Color sample(const Sampler* sampler,
                                   const Vector<float, 2>& coord) const
        {
            const auto lod = sampler ? sampler->lod : 0.0F;
            return sample(sampler, coord, lod);
        }

    private:
        PixelFormat pixelFormat;
        std::size_t width = 0;
        std::size_t height = 0;
        bool mipMaps = false;
        std::vector<std::vector<std::uint8_t>> levels;
        std::uint32_t minLOD = 0;
        std::uint32_t maxLOD = UINT_MAX;
        float lodBias = 0.0F;

        [[nodiscard]] std::size_t levelWidth(const std::uint32_t level) const noexcept
        {
            return std::max<std::size_t>(1U, width >> level);
        }

        [[nodiscard]] std::size_t levelHeight(const std::uint32_t level) const noexcept
        {
            return std::max<std::size_t>(1U, height >> level);
        }

        void allocateLevels()
        {
            levels.clear();

            const auto pixelSize = getPixelSize(pixelFormat);
            if (pixelSize == 0 || width == 0 || height == 0)
                return;

            // Without mipmaps only the base level exists. With mipmaps the
            // chain runs down to 1x1, each dimension halving (never below 1).
            for (std::uint32_t level = 0;; ++level)
            {
                const auto levelW = levelWidth(level);
                const auto levelH = levelHeight(level);
                levels.push_back(std::vector<std::uint8_t>(levelW * levelH * pixelSize));

                if (!mipMaps || (levelW == 1 && levelH == 1))
                    break;
            }
        }

        [[nodiscard]] Color sampleLevel(const std::uint32_t level,
                                        const Sampler* sampler,
                                        const Vector<float, 2>& coord) const
        {
            const auto levelW = levelWidth(level);
            const auto levelH = levelHeight(level);

            const auto u =
                (sampler->addressModeX == Sampler::AddressMode::clamp) ? std::clamp(coord.v[0], 0.0F, 1.0F) * (levelW - 1) :
                (sampler->addressModeX == Sampler::AddressMode::repeat) ? std::fmod(coord.v[0], 1.0F) * (levelW - 1) :
                (sampler->addressModeX == Sampler::AddressMode::mirror) ? 1.0F - 2.0F * std::fabs(std::fmod(coord.v[0] / 2.0F, 1.0F) - 0.5F) * (levelW - 1) :
                0.0F;

            const auto v =
                (sampler->addressModeY == Sampler::AddressMode::clamp) ? std::clamp(coord.v[1], 0.0F, 1.0F) * (levelH - 1) :
                (sampler->addressModeY == Sampler::AddressMode::repeat) ? std::fmod(coord.v[1], 1.0F) * (levelH - 1) :
                (sampler->addressModeY == Sampler::AddressMode::mirror) ? 1.0F - 2.0F * std::fabs(std::fmod(coord.v[1] / 2.0F, 1.0F) - 0.5F) * (levelH - 1) :
                0.0F;

            if (sampler->filter == Sampler::Filter::point)
            {
                const auto textureX = static_cast<std::size_t>(std::clamp(std::round(u), 0.0F, static_cast<float>(levelW - 1)));
                const auto textureY = static_cast<std::size_t>(std::clamp(std::round(v), 0.0F, static_cast<float>(levelH - 1)));
                return getPixel(textureX, textureY, level);
            }
            else
            {
                auto textureX0 = static_cast<std::size_t>(std::floor(u));
                auto textureY0 = static_cast<std::size_t>(std::floor(v));
                auto textureX1 = textureX0 + 1;
                auto textureY1 = textureY0 + 1;

                textureX0 = std::clamp(textureX0, static_cast<std::size_t>(0U), levelW - 1);
                textureX1 = std::clamp(textureX1, static_cast<std::size_t>(0U), levelW - 1);
                textureY0 = std::clamp(textureY0, static_cast<std::size_t>(0U), levelH - 1);
                textureY1 = std::clamp(textureY1, static_cast<std::size_t>(0U), levelH - 1);

                const Color color[4] = {
                    getPixel(textureX0, textureY0, level),
                    getPixel(textureX1, textureY0, level),
                    getPixel(textureX0, textureY1, level),
                    getPixel(textureX1, textureY1, level)
                };

                const auto fx = u - static_cast<float>(textureX0);
                const auto fy = v - static_cast<float>(textureY0);

                return Color{
                    color[0].r * (1.0F - fx) * (1.0F - fy) + color[1].r * fx * (1.0F - fy) + color[2].r * (1.0F - fx) * fy + color[3].r * fx * fy,
                    color[0].g * (1.0F - fx) * (1.0F - fy) + color[1].g * fx * (1.0F - fy) + color[2].g * (1.0F - fx) * fy + color[3].g * fx * fy,
                    color[0].b * (1.0F - fx) * (1.0F - fy) + color[1].b * fx * (1.0F - fy) + color[2].b * (1.0F - fx) * fy + color[3].b * fx * fy,
                    color[0].a * (1.0F - fx) * (1.0F - fy) + color[1].a * fx * (1.0F - fy) + color[2].a * (1.0F - fx) * fy + color[3].a * fx * fy
                };
            }
        }

        // Rebuilds the whole chain below parentLevel (parentLevel excluded).
        void updateSuccessorMipMaps(const std::uint32_t parentLevel)
        {
            if (!mipMaps)
                return;

            for (auto level = parentLevel + 1; level < levels.size(); ++level)
                downsampleLevel(level - 1, level);
        }

        // Child pixel (x, y) is the 2D mean of the parent region
        // [floor(x*parent/child), floor((x+1)*parent/child)) on both axes.
        void downsampleLevel(const std::uint32_t parentLevel,
                             const std::uint32_t childLevel)
        {
            const auto parentW = levelWidth(parentLevel);
            const auto parentH = levelHeight(parentLevel);
            const auto childW = levelWidth(childLevel);
            const auto childH = levelHeight(childLevel);
            const auto pixelSize = getPixelSize(pixelFormat);
            const auto& parent = levels[parentLevel];
            auto& child = levels[childLevel];

            const std::size_t channelCount =
                (pixelFormat == PixelFormat::rgba8) ? 4 : 1;

            for (std::size_t y = 0; y < childH; ++y)
            {
                const auto parentY0 = y * parentH / childH;
                const auto parentY1 = (y + 1) * parentH / childH;

                for (std::size_t x = 0; x < childW; ++x)
                {
                    const auto parentX0 = x * parentW / childW;
                    const auto parentX1 = (x + 1) * parentW / childW;

                    const auto regionWidth = parentX1 - parentX0;
                    const auto regionHeight = parentY1 - parentY0;
                    const auto regionPixels = regionWidth * regionHeight;

                    for (std::size_t channel = 0; channel < channelCount; ++channel)
                    {
                        if (pixelFormat == PixelFormat::float32)
                        {
                            double sum = 0.0;
                            for (auto py = parentY0; py < parentY1; ++py)
                                for (auto px = parentX0; px < parentX1; ++px)
                                {
                                    const float value =
                                        reinterpret_cast<const float*>(parent.data())[py * parentW + px];
                                    sum += static_cast<double>(value);
                                }

                            reinterpret_cast<float*>(child.data())[y * childW + x] =
                                static_cast<float>(sum / static_cast<double>(regionPixels));
                        }
                        else
                        {
                            std::uint32_t sum = 0;
                            for (auto py = parentY0; py < parentY1; ++py)
                                for (auto px = parentX0; px < parentX1; ++px)
                                    sum += parent[(py * parentW + px) * pixelSize + channel];

                            // Round half away from zero (all values are non-negative).
                            const auto averaged =
                                (sum + regionPixels / 2) / regionPixels;
                            child[(y * childW + x) * pixelSize + channel] =
                                static_cast<std::uint8_t>(averaged);
                        }
                    }
                }
            }
        }
    };
}

#include "TextureView.hpp"

namespace sr
{
    inline void clear(TextureView renderTarget, const Color color)
    {
        Texture& texture = renderTarget.getTexture();
        const auto level = renderTarget.getLevel();

        if (texture.getPixelFormat() != PixelFormat::rgba8)
            throw RenderError{"Color render target must be rgba8"};

        if (level >= texture.getLevelCount())
            throw RenderError{"Invalid mip level"};

        auto& buffer = texture.getData(level);
        const auto bufferData = reinterpret_cast<std::uint32_t*>(buffer.data());
        const auto rgba = color.getIntValueRaw();

        const auto bufferSize = texture.getWidth(level) * texture.getHeight(level);
        for (std::size_t p = 0; p < bufferSize; ++p)
            bufferData[p] = rgba;

        texture.generateMipMaps(level);
    }

    inline void clear(Texture& renderTarget, const Color color)
    {
        clear(TextureView{renderTarget, 0}, color);
    }

    inline void clear(TextureView depthTarget, const float depth)
    {
        Texture& texture = depthTarget.getTexture();
        const auto level = depthTarget.getLevel();

        if (texture.getPixelFormat() != PixelFormat::float32)
            throw RenderError{"Depth render target must be float32"};

        if (level >= texture.getLevelCount())
            throw RenderError{"Invalid mip level"};

        auto& buffer = texture.getData(level);
        const auto bufferData = reinterpret_cast<float*>(buffer.data());

        const auto bufferSize = texture.getWidth(level) * texture.getHeight(level);
        for (std::size_t p = 0; p < bufferSize; ++p)
            bufferData[p] = depth;

        texture.generateMipMaps(level);
    }

    inline void clear(Texture& depthTarget, const float depth)
    {
        clear(TextureView{depthTarget, 0}, depth);
    }
}

#endif
