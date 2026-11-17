//
//  SoftwareRenderer
//

#ifndef SR_RENDERER_HPP
#define SR_RENDERER_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
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
    inline void drawTriangles(TextureView frameBufferView,
                              TextureView depthBufferView,
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
        Texture& frameBuffer = frameBufferView.getTexture();
        Texture& depthBuffer = depthBufferView.getTexture();
        const auto colorLevel = frameBufferView.getLevel();
        const auto depthLevel = depthBufferView.getLevel();

        // Validate everything before a single byte is written.
        if (frameBuffer.getPixelFormat() != PixelFormat::rgba8)
            throw RenderError{"Color render target must be rgba8"};

        if (depthBuffer.getPixelFormat() != PixelFormat::float32)
            throw RenderError{"Depth render target must be float32"};

        if (colorLevel >= frameBuffer.getLevelCount() ||
            depthLevel >= depthBuffer.getLevelCount())
            throw RenderError{"Invalid mip level"};

        const auto targetWidth = frameBuffer.getWidth(colorLevel);
        const auto targetHeight = frameBuffer.getHeight(colorLevel);

        if (depthBuffer.getWidth(depthLevel) != targetWidth ||
            depthBuffer.getHeight(depthLevel) != targetHeight)
            throw RenderError{"Render target levels must have the same size"};

        // Normalized scissor rect converted to a half-open pixel rect and
        // intersected with the render target.
        const auto scissorX0 = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(std::floor(scissorRect.left() * static_cast<float>(targetWidth))));
        const auto scissorX1 = std::min<std::int64_t>(
            static_cast<std::int64_t>(targetWidth),
            static_cast<std::int64_t>(std::ceil(scissorRect.right() * static_cast<float>(targetWidth))));
        const auto scissorY0 = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(std::floor(scissorRect.bottom() * static_cast<float>(targetHeight))));
        const auto scissorY1 = std::min<std::int64_t>(
            static_cast<std::int64_t>(targetHeight),
            static_cast<std::int64_t>(std::ceil(scissorRect.top() * static_cast<float>(targetHeight))));

        // Any texture that is also a render target is copied up front, so the
        // fragment shader always sees the level chain as it was at draw start.
        std::array<std::unique_ptr<Texture>, 2> snapshots;
        std::array<const Texture*, 2> effectiveTextures = textures;

        for (std::size_t slot = 0; slot < textures.size(); ++slot)
        {
            const auto* input = textures[slot];
            if (input && (input == &frameBuffer || input == &depthBuffer))
            {
                auto snapshot = std::make_unique<Texture>(
                    input->getPixelFormat(), input->getWidth(), input->getHeight(),
                    input->hasMipMaps());

                for (std::uint32_t level = 0; level < input->getLevelCount(); ++level)
                    snapshot->getData(level) = input->getData(level);

                effectiveTextures[slot] = snapshot.get();
                snapshots[slot] = std::move(snapshot);
            }
        }

        const auto frameBufferData = reinterpret_cast<std::uint32_t*>(
            frameBufferView.getData().data());
        const auto depthBufferData = reinterpret_cast<float*>(
            depthBufferView.getData().data());

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
                // transform to viewport coordinates (pixels of the selected level)
                viewportPosition.v[0] = viewportPosition.v[0] * viewport.size.v[0] / 2.0F + viewport.position.v[0] + viewport.size.v[0] / 2.0F; // xndc * width / 2 + x + width / 2
                viewportPosition.v[1] = viewportPosition.v[1] * viewport.size.v[1] / 2.0F + viewport.position.v[1] + viewport.size.v[1] / 2.0F;  // yndc * height / 2 + y + height / 2
                //viewportPosition.v[2] = viewportPosition.v[2] * (1.0F - 0.0F) / 2.0F + (1.0F + 0.0F) / 2.0F; // zndc * (far - near) / 2 + (far + near) / 2

                if (viewportPosition.v[0] < screenMin.v[0]) screenMin.v[0] = viewportPosition.v[0];
                if (viewportPosition.v[0] > screenMax.v[0]) screenMax.v[0] = viewportPosition.v[0];
                if (viewportPosition.v[1] < screenMin.v[1]) screenMin.v[1] = viewportPosition.v[1];
                if (viewportPosition.v[1] > screenMax.v[1]) screenMax.v[1] = viewportPosition.v[1];
            }

            // Half-open pixel bounding box, clipped against the scissor rect.
            const auto pixelX0 = std::max<std::int64_t>(
                scissorX0, static_cast<std::int64_t>(std::floor(screenMin.v[0])));
            const auto pixelX1 = std::min<std::int64_t>(
                scissorX1, static_cast<std::int64_t>(std::ceil(screenMax.v[0])) + 1);
            const auto pixelY0 = std::max<std::int64_t>(
                scissorY0, static_cast<std::int64_t>(std::floor(screenMin.v[1])));
            const auto pixelY1 = std::min<std::int64_t>(
                scissorY1, static_cast<std::int64_t>(std::ceil(screenMax.v[1])) + 1);

            if (pixelX0 >= pixelX1 || pixelY0 >= pixelY1)
                continue;

            const auto v0 = viewportPositions[1] - viewportPositions[0];
            const auto v1 = viewportPositions[2] - viewportPositions[0];
            const auto den = v0.v[0] * v1.v[1] - v1.v[0] * v0.v[1];

            for (auto screenY = static_cast<std::size_t>(pixelY0);
                 screenY < static_cast<std::size_t>(pixelY1); ++screenY)
                for (auto screenX = static_cast<std::size_t>(pixelX0);
                     screenX < static_cast<std::size_t>(pixelX1); ++screenX)
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

                        if (depthState.read && depthBufferData[screenY * targetWidth + screenX] < depth)
                            continue; // discard the pixel

                        if (depthState.write)
                            depthBufferData[screenY * targetWidth + screenX] = depth;

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

                        const auto srcColor = fragmentShader(psInput, samplers, effectiveTextures);

                        if (blendState.enabled)
                        {
                            const auto pixel = reinterpret_cast<std::uint8_t*>(&frameBufferData[screenY * targetWidth + screenX]);
                            const Color destColor{pixel[0], pixel[1], pixel[2], pixel[3]};

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

                            frameBufferData[screenY * targetWidth + screenX] = resultColor.getIntValueRaw();
                        }
                        else
                            frameBufferData[screenY * targetWidth + screenX] = srcColor.getIntValueRaw();
                    }
                }
        }

        // The draw is finished: refresh the descendant mip levels of both
        // targets. Ancestor levels (including the drawn level) are untouched
        // by this call beyond what the rasterizer already wrote.
        frameBuffer.generateMipMaps(colorLevel);
        depthBuffer.generateMipMaps(depthLevel);
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
        drawTriangles(TextureView{frameBuffer, 0},
                      TextureView{depthBuffer, 0},
                      vertexShader, fragmentShader,
                      samplers, textures,
                      viewport, scissorRect,
                      blendState, depthState,
                      indices, vertices, modelViewProjection);
    }
}

#endif
