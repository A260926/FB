//
//  SoftwareRenderer
//

#ifndef SR_TEXTUREVIEW_HPP
#define SR_TEXTUREVIEW_HPP

#include <cstdint>
#include "RenderError.hpp"
#include "Texture.hpp"

namespace sr
{
    class TextureView final
    {
    public:
        explicit TextureView(Texture& initTexture,
                             const std::uint32_t initLevel = 0) noexcept:
            texture{&initTexture},
            level{initLevel}
        {
        }

        [[nodiscard]] Texture& getTexture() const noexcept { return *texture; }
        [[nodiscard]] std::uint32_t getLevel() const noexcept { return level; }

        [[nodiscard]] std::size_t getWidth() const { return texture->getWidth(level); }
        [[nodiscard]] std::size_t getHeight() const { return texture->getHeight(level); }
        [[nodiscard]] PixelFormat getPixelFormat() const { return texture->getPixelFormat(); }

        [[nodiscard]] std::vector<std::uint8_t>& getData() const { return texture->getData(level); }

        [[nodiscard]] Color getPixel(const std::size_t x, const std::size_t y) const
        {
            return texture->getPixel(x, y, level);
        }

    private:
        Texture* texture;
        std::uint32_t level;
    };

    inline void clear(const TextureView& renderTarget, const Color color)
    {
        if (renderTarget.getPixelFormat() != PixelFormat::rgba8)
            throw RenderError{"Color render target must be rgba8"};

        auto& texture = renderTarget.getTexture();
        const auto level = renderTarget.getLevel();
        auto& buffer = texture.getData(level);
        const auto bufferData = reinterpret_cast<std::uint32_t*>(buffer.data());
        const auto rgba = color.getIntValueRaw();

        const auto bufferSize = texture.getWidth(level) * texture.getHeight(level);
        for (std::size_t p = 0; p < bufferSize; ++p)
            bufferData[p] = rgba;

        texture.generateMipMaps(level);
    }

    inline void clear(const TextureView& renderTarget, const float depth)
    {
        if (renderTarget.getPixelFormat() != PixelFormat::float32)
            throw RenderError{"Depth render target must be float32"};

        auto& texture = renderTarget.getTexture();
        const auto level = renderTarget.getLevel();
        auto& buffer = texture.getData(level);
        const auto bufferData = reinterpret_cast<float*>(buffer.data());

        const auto bufferSize = texture.getWidth(level) * texture.getHeight(level);
        for (std::size_t p = 0; p < bufferSize; ++p)
            bufferData[p] = depth;

        texture.generateMipMaps(level);
    }
}

#endif
