//
//  SoftwareRenderer
//

#ifndef SR_RENDERER_HPP
#define SR_RENDERER_HPP

#include <cassert>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>
#include "BlendState.hpp"
#include "Color.hpp"
#include "DepthState.hpp"
#include "Matrix.hpp"
#include "Rect.hpp"
#include "RenderError.hpp"
#include "Sampler.hpp"
#include "Shader.hpp"
#include "Texture.hpp"
#include "TextureView.hpp"
#include "Vector.hpp"
#include "Vertex.hpp"

namespace sr
{
    inline void drawTriangles(TextureView frameBuffer,
                              TextureView depthBuffer,
                              VertexShader vertexShader,
                              FragmentShader fragmentShader,
                              const std::array<const Sampler*, 2>& samplers,
                              const std::array<const Texture*, 2>& textures,
                              const Rect<float>& viewport,
                              const Rect<float>& scissorRect,
                              const BlendState& blendState,
                              const DepthState& depthState,
                              const std::vector<std::size_t>& indices,
                              const std::vector<Vertex>& vertices,
                              const Matrix<float, 4>& modelViewProjection)
    {
        if (frameBuffer.getPixelFormat() != PixelFormat::rgba8)
            throw RenderError{"Color render target must be rgba8"};
        if (depthBuffer.getPixelFormat() != PixelFormat::float32)
            throw RenderError{"Depth render target must be float32"};

        Texture& frameBufferTexture = frameBuffer.getTexture();
        Texture& depthBufferTexture = depthBuffer.getTexture();

        const auto frameBufferLevel = frameBuffer.getLevel();
        const auto depthBufferLevel = depthBuffer.getLevel();

        if (frameBufferTexture.getWidth(frameBufferLevel) != depthBufferTexture.getWidth(depthBufferLevel) ||
            frameBufferTexture.getHeight(frameBufferLevel) != depthBufferTexture.getHeight(depthBufferLevel))
            throw RenderError{"Color and depth render target sizes do not match"};

        const auto frameWidth = frameBufferTexture.getWidth(frameBufferLevel);
        const auto frameHeight = frameBufferTexture.getHeight(frameBufferLevel);

        auto& frameBufferStorage = frameBufferTexture.getData(frameBufferLevel);
        auto& depthBufferStorage = depthBufferTexture.getData(depthBufferLevel);

        const auto frameBufferData = reinterpret_cast<std::uint32_t*>(frameBufferStorage.data());
        const auto depthBufferData = reinterpret_cast<float*>(depthBufferStorage.data());

        // Pixels covered by the triangles. They are written back to the real
        // target only after the whole draw, so aliased reads (target used as
        // input) all see the mip chain as it was when the draw started.
        std::vector<std::uint8_t> colorAccum(frameWidth * frameHeight * 4U);
        std::vector<std::uint8_t> colorWritten(frameWidth * frameHeight, std::uint8_t(0U));
        std::vector<std::uint8_t> depthAccum(frameWidth * frameHeight * sizeof(float));
        std::vector<std::uint8_t> depthWritten(frameWidth * frameHeight, std::uint8_t(0U));

        const auto colorAt = [&frameBufferData](const std::size_t index) noexcept
        {
            const auto pixel = reinterpret_cast<const std::uint8_t*>(&frameBufferData[index]);
            return Color{pixel[0], pixel[1], pixel[2], pixel[3]};
        };

        const auto writeColor = [&colorAccum, &colorWritten](const std::size_t index, const Color& color)
        {
            const auto value = color.getIntValueRaw();
            std::uint8_t* pixel = &colorAccum[index * 4U];
            pixel[0] = static_cast<std::uint8_t>((value >> 0U) & 0xFFU);
            pixel[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
            pixel[2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
            pixel[3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
            colorWritten[index] = 1U;
        };

        const auto writeDepth = [&depthAccum, &depthWritten](const std::size_t index, const float value)
        {
            std::uint8_t* bytes = &depthAccum[index * sizeof(float)];
            const auto* source = reinterpret_cast<const std::uint8_t*>(&value);
            for (std::size_t b = 0; b < sizeof(float); ++b)
                bytes[b] = source[b];
            depthWritten[index] = 1U;
        };

        // scissorRect is normalized; convert it to pixels of the selected level
        // and intersect it with the render target.
        const auto scissorLeft = static_cast<long long>(std::floor(scissorRect.position.v[0] * static_cast<float>(frameWidth)));
        const auto scissorRight = static_cast<long long>(std::ceil((scissorRect.position.v[0] + scissorRect.size.v[0]) * static_cast<float>(frameWidth)));
        const auto scissorBottom = static_cast<long long>(std::floor(scissorRect.position.v[1] * static_cast<float>(frameHeight)));
        const auto scissorTop = static_cast<long long>(std::ceil((scissorRect.position.v[1] + scissorRect.size.v[1]) * static_cast<float>(frameHeight)));

        const auto clipMinX = std::clamp(scissorLeft, 0LL, static_cast<long long>(frameWidth));
        const auto clipMaxX = std::clamp(scissorRight, 0LL, static_cast<long long>(frameWidth));
        const auto clipMinY = std::clamp(scissorBottom, 0LL, static_cast<long long>(frameHeight));
        const auto clipMaxY = std::clamp(scissorTop, 0LL, static_cast<long long>(frameHeight));

        for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            const std::array<VertexShaderOutput, 3> vsOutputs{
                vertexShader(modelViewProjection, vertices[indices[i + 0]]),
                vertexShader(modelViewProjection, vertices[indices[i + 1]]),
                vertexShader(modelViewProjection, vertices[indices[i + 2]])
            };

            std::array<Vector<float, 4>, 3> ndcPositions{
                vsOutputs[0].position,
                vsOutputs[1].position,
                vsOutputs[2].position
            };

            // transform to normalized device coordinates
            for (sr::Vector<float, 4>& ndcPosition : ndcPositions)
                ndcPosition /= ndcPosition.v[3];

            std::array<Vector<float, 2>, 3> viewportPositions{
                Vector<float, 2>{ndcPositions[0].v[0], ndcPositions[0].v[1]},
                Vector<float, 2>{ndcPositions[1].v[0], ndcPositions[1].v[1]},
                Vector<float, 2>{ndcPositions[2].v[0], ndcPositions[2].v[1]}
            };

            Vector<float, 2> screenMin{
                std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity()
            };
            Vector<float, 2> screenMax{
                -std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity()
            };

            for (auto& viewportPosition : viewportPositions)
            {
                // transform to viewport coordinates
                viewportPosition.v[0] = viewportPosition.v[0] * viewport.size.v[0] / 2.0F + viewport.position.v[0] + viewport.size.v[0] / 2.0F; // xndc * width / 2 + x + width / 2
                viewportPosition.v[1] = viewportPosition.v[1] * viewport.size.v[1] / 2.0F + viewport.position.v[1] + viewport.size.v[1] / 2.0F;  // yndc * height / 2 + y + height / 2
                //viewportPosition.v[2] = viewportPosition.v[2] * (1.0F - 0.0F) / 2.0F + (1.0F + 0.0F) / 2.0F; // zndc * (far - near) / 2 + (far + near) / 2

                if (viewportPosition.v[0] < screenMin.v[0]) screenMin.v[0] = viewportPosition.v[0];
                if (viewportPosition.v[0] > screenMax.v[0]) screenMax.v[0] = viewportPosition.v[0];
                if (viewportPosition.v[1] < screenMin.v[1]) screenMin.v[1] = viewportPosition.v[1];
                if (viewportPosition.v[1] > screenMax.v[1]) screenMax.v[1] = viewportPosition.v[1];
            }

            const auto pixelMinX = std::max(static_cast<long long>(0),
                static_cast<long long>(std::floor(screenMin.v[0])));
            const auto pixelMinY = std::max(static_cast<long long>(0),
                static_cast<long long>(std::floor(screenMin.v[1])));
            const auto pixelMaxX = std::min(static_cast<long long>(frameWidth - 1),
                static_cast<long long>(std::ceil(screenMax.v[0])));
            const auto pixelMaxY = std::min(static_cast<long long>(frameHeight - 1),
                static_cast<long long>(std::ceil(screenMax.v[1])));

            const auto loopMinX = std::max(pixelMinX, clipMinX);
            const auto loopMinY = std::max(pixelMinY, clipMinY);
            const auto loopMaxX = std::min(pixelMaxX, clipMaxX - 1);
            const auto loopMaxY = std::min(pixelMaxY, clipMaxY - 1);

            const auto v0 = viewportPositions[1] - viewportPositions[0];
            const auto v1 = viewportPositions[2] - viewportPositions[0];
            const auto den = v0.v[0] * v1.v[1] - v1.v[0] * v0.v[1];

            if (den == 0.0F)
                continue;

            for (auto screenY = std::max(loopMinY, 0LL); screenY <= loopMaxY; ++screenY)
                for (auto screenX = std::max(loopMinX, 0LL); screenX <= loopMaxX; ++screenX)
                {
                    const Vector<float, 2> p{
                        static_cast<float>(screenX),
                        static_cast<float>(screenY)
                    };

                    const auto v2 = p - viewportPositions[0];

                    // calculate barycentric coordinates
                    const auto v = (v2.v[0] * v1.v[1] - v1.v[0] * v2.v[1]) / den;
                    const auto w = (v0.v[0] * v2.v[1] - v2.v[0] * v0.v[1]) / den;

                    if (v >= 0.0F && w >= 0.0F && v + w <= 1.0F)
                    {
                        const auto u = 1.0F - v - w;

                        Vector<float, 3> clip{
                            u / vsOutputs[0].position.v[3],
                            v / vsOutputs[1].position.v[3],
                            w / vsOutputs[2].position.v[3]
                        };
                        clip /= (clip.v[0] + clip.v[1] + clip.v[2]);

                        const auto depth = ndcPositions[0].v[2] * clip.v[0] + ndcPositions[1].v[2] * clip.v[1] + ndcPositions[2].v[2] * clip.v[2];

                        const auto index = static_cast<std::size_t>(screenY) * frameWidth +
                            static_cast<std::size_t>(screenX);

                        const auto currentDepth = depthWritten[index] ?
                            *reinterpret_cast<const float*>(&depthAccum[index * sizeof(float)]) :
                            depthBufferData[index];

                        if (depthState.read && currentDepth < depth)
                            continue; // discard the pixel

                        if (depthState.write)
                            writeDepth(index, depth);

                        VertexShaderOutput psInput;
                        psInput.position = Vector<float, 4>{clip.v[0], clip.v[1], clip.v[2], 1.0F};
                        psInput.color = Color{
                            vsOutputs[0].color.r * clip.v[0] + vsOutputs[1].color.r * clip.v[1] + vsOutputs[2].color.r * clip.v[2],
                            vsOutputs[0].color.g * clip.v[0] + vsOutputs[1].color.g * clip.v[1] + vsOutputs[2].color.g * clip.v[2],
                            vsOutputs[0].color.b * clip.v[0] + vsOutputs[1].color.b * clip.v[1] + vsOutputs[2].color.b * clip.v[2],
                            vsOutputs[0].color.a * clip.v[0] + vsOutputs[1].color.a * clip.v[1] + vsOutputs[2].color.a * clip.v[2]
                        };

                        psInput.texCoords[0] = Vector<float, 2>{
                            vsOutputs[0].texCoords[0].v[0] * clip.v[0] + vsOutputs[1].texCoords[0].v[0] * clip.v[1] + vsOutputs[2].texCoords[0].v[0] * clip.v[2],
                            vsOutputs[0].texCoords[0].v[1] * clip.v[0] + vsOutputs[1].texCoords[0].v[1] * clip.v[1] + vsOutputs[2].texCoords[0].v[1] * clip.v[2]
                        };

                        psInput.texCoords[1] = Vector<float, 2>{
                            vsOutputs[0].texCoords[1].v[0] * clip.v[0] + vsOutputs[1].texCoords[1].v[0] * clip.v[1] + vsOutputs[2].texCoords[1].v[0] * clip.v[2],
                            vsOutputs[0].texCoords[1].v[1] * clip.v[0] + vsOutputs[1].texCoords[1].v[1] * clip.v[1] + vsOutputs[2].texCoords[1].v[1] * clip.v[2]
                        };

                        psInput.normal = vsOutputs[0].normal * clip.v[0] + vsOutputs[1].normal * clip.v[1] + vsOutputs[2].normal * clip.v[2];

                        const auto srcColor = fragmentShader(psInput, samplers, textures);

                        if (blendState.enabled)
                        {
                            const auto destColor = colorWritten[index] ?
                                Color{
                                    colorAccum[index * 4U + 0U],
                                    colorAccum[index * 4U + 1U],
                                    colorAccum[index * 4U + 2U],
                                    colorAccum[index * 4U + 3U]
                                } :
                                colorAt(index);

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

                            writeColor(index, resultColor);
                        }
                        else
                            writeColor(index, srcColor);
                    }
                }
        }

        for (std::size_t index = 0; index < colorWritten.size(); ++index)
        {
            if (colorWritten[index])
            {
                const auto* pixel = &colorAccum[index * 4U];
                auto* destination = reinterpret_cast<std::uint8_t*>(&frameBufferData[index]);
                destination[0] = pixel[0];
                destination[1] = pixel[1];
                destination[2] = pixel[2];
                destination[3] = pixel[3];
            }

            if (depthWritten[index])
                depthBufferData[index] = *reinterpret_cast<const float*>(&depthAccum[index * sizeof(float)]);
        }

        // Cover the successors of the touched levels; the draw itself has
        // already finished above.
        frameBufferTexture.generateMipMaps(frameBufferLevel);
        depthBufferTexture.generateMipMaps(depthBufferLevel);
    }

    // Legacy entry points: a plain texture selects its base level.
    inline void drawTriangles(Texture& frameBuffer,
                              TextureView depthBuffer,
                              VertexShader vertexShader,
                              FragmentShader fragmentShader,
                              const std::array<const Sampler*, 2>& samplers,
                              const std::array<const Texture*, 2>& textures,
                              const Rect<float>& viewport,
                              const Rect<float>& scissorRect,
                              const BlendState& blendState,
                              const DepthState& depthState,
                              const std::vector<std::size_t>& indices,
                              const std::vector<Vertex>& vertices,
                              const Matrix<float, 4>& modelViewProjection)
    {
        drawTriangles(TextureView{frameBuffer, 0U}, depthBuffer, vertexShader,
                      fragmentShader, samplers, textures, viewport, scissorRect,
                      blendState, depthState, indices, vertices, modelViewProjection);
    }

    inline void drawTriangles(TextureView frameBuffer,
                              Texture& depthBuffer,
                              VertexShader vertexShader,
                              FragmentShader fragmentShader,
                              const std::array<const Sampler*, 2>& samplers,
                              const std::array<const Texture*, 2>& textures,
                              const Rect<float>& viewport,
                              const Rect<float>& scissorRect,
                              const BlendState& blendState,
                              const DepthState& depthState,
                              const std::vector<std::size_t>& indices,
                              const std::vector<Vertex>& vertices,
                              const Matrix<float, 4>& modelViewProjection)
    {
        drawTriangles(frameBuffer, TextureView{depthBuffer, 0U}, vertexShader,
                      fragmentShader, samplers, textures, viewport, scissorRect,
                      blendState, depthState, indices, vertices, modelViewProjection);
    }

    inline void drawTriangles(Texture& frameBuffer,
                              Texture& depthBuffer,
                              VertexShader vertexShader,
                              FragmentShader fragmentShader,
                              const std::array<const Sampler*, 2>& samplers,
                              const std::array<const Texture*, 2>& textures,
                              const Rect<float>& viewport,
                              const Rect<float>& scissorRect,
                              const BlendState& blendState,
                              const DepthState& depthState,
                              const std::vector<std::size_t>& indices,
                              const std::vector<Vertex>& vertices,
                              const Matrix<float, 4>& modelViewProjection)
    {
        drawTriangles(TextureView{frameBuffer, 0U}, TextureView{depthBuffer, 0U},
                      vertexShader, fragmentShader, samplers, textures, viewport,
                      scissorRect, blendState, depthState, indices, vertices,
                      modelViewProjection);
    }
}

#endif
