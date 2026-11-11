#pragma once

#include "texture.h"

#include <cstddef>
#include <limits>
#include <vector>

/*
    MultisampleFramebuffer: fixed 4x MSAA render target.

    Each pixel stores four independent RGBA8 color samples and four
    independent float depth samples. The sub-pixel sample positions
    (in pixel-local coordinates, origin at the lower-left corner) are:

        s = 0: (3/8, 1/8)
        s = 1: (7/8, 3/8)
        s = 2: (1/8, 5/8)
        s = 3: (5/8, 7/8)

    Samples keep their own coverage, depth and interpolated varying data;
    uncovered samples retain their previous color. resolve() averages the
    four stored color bytes per channel, rounding with (sum + 2) / 4.
*/
struct MultisampleFramebuffer
{
    static constexpr std::size_t sample_count = 4;

    MultisampleFramebuffer(unsigned int width, unsigned int height)
        : m_width(static_cast<int>(width)), m_height(static_cast<int>(height)),
          m_color(width * height * sample_count),
          m_depth(width * height * sample_count, std::numeric_limits<float>::max())
    {

    }

    int width() const { return m_width; }
    int height() const { return m_height; }

    /* all four samples get the given color, all depths go to the far value */
    void clear(const RGBA8& color)
    {
        std::fill(m_color.begin(), m_color.end(), color);
        std::fill(m_depth.begin(), m_depth.end(), std::numeric_limits<float>::max());
    }

    void clear(const Vec4& color)
    {
        clear(RGBA8(color * 255));
    }

    void clear()
    {
        clear(RGBA8());
    }

    RGBA8& color(int x, int y, std::size_t s)
    {
        return m_color[offset(x, y, s)];
    }

    const RGBA8& color(int x, int y, std::size_t s) const
    {
        return m_color[offset(x, y, s)];
    }

    float& depth(int x, int y, std::size_t s)
    {
        return m_depth[offset(x, y, s)];
    }

    const float& depth(int x, int y, std::size_t s) const
    {
        return m_depth[offset(x, y, s)];
    }

    /* sample positions in pixel-local coordinates, same orientation as the viewport */
    static Vec2 sample_position(std::size_t s)
    {
        static const Vec2 positions[sample_count] =
        {
            { 3.0f / 8.0f, 1.0f / 8.0f },
            { 7.0f / 8.0f, 3.0f / 8.0f },
            { 1.0f / 8.0f, 5.0f / 8.0f },
            { 5.0f / 8.0f, 7.0f / 8.0f }
        };

        return positions[s];
    }

    /* average the four stored samples; the returned texture owns independent data */
    Texture<RGBA8> resolve() const
    {
        Texture<RGBA8> result(static_cast<unsigned int>(m_width), static_cast<unsigned int>(m_height));

        for(int y = 0; y < m_height; y++)
        {
            for(int x = 0; x < m_width; x++)
            {
                int sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
                for(std::size_t s = 0; s < sample_count; s++)
                {
                    const RGBA8& c = color(x, y, s);
                    sum_r += c.x;
                    sum_g += c.y;
                    sum_b += c.z;
                    sum_a += c.w;
                }

                result(x, y) = RGBA8(
                    static_cast<std::uint8_t>((sum_r + 2) / 4),
                    static_cast<std::uint8_t>((sum_g + 2) / 4),
                    static_cast<std::uint8_t>((sum_b + 2) / 4),
                    static_cast<std::uint8_t>((sum_a + 2) / 4));
            }
        }

        return result;
    }

private:
    std::size_t offset(int x, int y, std::size_t s) const
    {
        assert(x >= 0 && x < m_width);
        assert(y >= 0 && y < m_height);
        assert(s < sample_count);
        return (static_cast<std::size_t>(y) * m_width + x) * sample_count + s;
    }

    int m_width;
    int m_height;
    std::vector<RGBA8> m_color;
    std::vector<float> m_depth;
};
