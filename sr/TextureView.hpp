//
//  SoftwareRenderer
//

#ifndef SR_TEXTUREVIEW_HPP
#define SR_TEXTUREVIEW_HPP

#include <cstdint>
#include <vector>
#include "Texture.hpp"

namespace sr
{
    // Non-owning reference to a single mip level of a Texture. Every call
    // resolves against the owner's current dimensions, so the view stays
    // valid across Texture::resize.
    class TextureView final
    {
    public:
        TextureView(Texture& initTexture, const std::uint32_t initLevel = 0) noexcept:
            texture{initTexture},
            level{initLevel}
        {
        }

        [[nodiscard]] Texture& getTexture() const noexcept { return texture; }
        [[nodiscard]] std::uint32_t getLevel() const noexcept { return level; }

        [[nodiscard]] std::size_t getWidth() const
        {
            return texture.getWidth(level);
        }

        [[nodiscard]] std::size_t getHeight() const
        {
            return texture.getHeight(level);
        }

        [[nodiscard]] std::vector<std::uint8_t>& getData() const
        {
            return texture.getData(level);
        }

    private:
        Texture& texture;
        std::uint32_t level = 0;
    };
}

#endif
