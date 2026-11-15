//
//  SoftwareRenderer
//

#ifndef SR_RENDERER_HPP
#define SR_RENDERER_HPP

#include <cassert>
#include <algorithm>
#include <array>
#include <limits>
#include "BlendState.hpp"
#include "Color.hpp"
#include "DepthState.hpp"
#include "MaterialProgram.hpp"
#include "Matrix.hpp"
#include "Rect.hpp"
#include "RenderError.hpp"
#include "Sampler.hpp"
#include "Shader.hpp"
#include "Texture.hpp"
#include "Vector.hpp"
#include "Vertex.hpp"

namespace sr
{
    namespace detail
    {
        // Analytic screen-space x/y derivatives of the interpolated fragment
        // inputs, derived from the perspective quotient at the current integer
        // sample position.
        struct FragmentDerivatives final
        {
            std::array<Vector<float, 2>, 2> texCoordsDx{};
            std::array<Vector<float, 2>, 2> texCoordsDy{};
            Color colorDx{};
            Color colorDy{};
        };

        template <bool computeDerivatives, typename FragmentCallback>
        void rasterizeTriangles(Texture& frameBuffer,
                                Texture& depthBuffer,
                                VertexShader vertexShader,
                                FragmentCallback&& fragmentCallback,
                                const Rect<float>& viewport,
                                const Rect<float>& scissorRect,
                                const BlendState& blendState,
                                const DepthState& depthState,
                                const std::vector<std::size_t>& indices,
                                const std::vector<Vertex>& vertices,
                                const Matrix<float, 4>& modelViewProjection)
        {
            const auto frameBufferData = reinterpret_cast<std::uint32_t*>(frameBuffer.getData().data());
            const auto depthBufferData = reinterpret_cast<float*>(depthBuffer.getData().data());

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

                screenMin.v[0] = std::clamp(screenMin.v[0], 0.0F, static_cast<float>(frameBuffer.getWidth() - 1) * scissorRect.position.v[0]);
                screenMax.v[0] = std::clamp(screenMax.v[0], 0.0F, static_cast<float>(frameBuffer.getWidth() - 1) * (scissorRect.position.v[0] + scissorRect.size.v[0]));
                screenMin.v[1] = std::clamp(screenMin.v[1], 0.0F, static_cast<float>(frameBuffer.getHeight() - 1) * scissorRect.position.v[1]);
                screenMax.v[1] = std::clamp(screenMax.v[1], 0.0F, static_cast<float>(frameBuffer.getHeight() - 1) * (scissorRect.position.v[1] + scissorRect.size.v[1]));

                const auto v0 = viewportPositions[1] - viewportPositions[0];
                const auto v1 = viewportPositions[2] - viewportPositions[0];
                const auto den = v0.v[0] * v1.v[1] - v1.v[0] * v0.v[1];

                float dbarycentricDx[3]{};
                float dbarycentricDy[3]{};
                float reciprocalW[3]{};
                if constexpr (computeDerivatives)
                {
                    reciprocalW[0] = 1.0F / vsOutputs[0].position.v[3];
                    reciprocalW[1] = 1.0F / vsOutputs[1].position.v[3];
                    reciprocalW[2] = 1.0F / vsOutputs[2].position.v[3];

                    // derivatives of the barycentric coordinates (u, v, w) of the
                    // three vertices with respect to the screen x and y
                    dbarycentricDx[0] = (v0.v[1] - v1.v[1]) / den;
                    dbarycentricDx[1] = v1.v[1] / den;
                    dbarycentricDx[2] = -v0.v[1] / den;
                    dbarycentricDy[0] = (v1.v[0] - v0.v[0]) / den;
                    dbarycentricDy[1] = -v1.v[0] / den;
                    dbarycentricDy[2] = v0.v[0] / den;
                }

                for (auto screenY = static_cast<std::size_t>(screenMin.v[1]); screenY <= static_cast<std::size_t>(screenMax.v[1]); ++screenY)
                    for (auto screenX = static_cast<std::size_t>(screenMin.v[0]); screenX <= static_cast<std::size_t>(screenMax.v[0]); ++screenX)
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
                            const auto clipSum = clip.v[0] + clip.v[1] + clip.v[2];
                            clip /= clipSum;

                            const auto depth = ndcPositions[0].v[2] * clip.v[0] + ndcPositions[1].v[2] * clip.v[1] + ndcPositions[2].v[2] * clip.v[2];

                            if (depthState.read && depthBufferData[screenY * depthBuffer.getWidth() + screenX] < depth)
                                continue; // discard the pixel

                            if (depthState.write)
                                depthBufferData[screenY * depthBuffer.getWidth() + screenX] = depth;

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

                            Color srcColor;
                            if constexpr (computeDerivatives)
                            {
                                // derivatives of the perspective quotient clip_i = q_i / S
                                // with q_i = bary_i / w_i and S = q_0 + q_1 + q_2
                                const auto dClipSumDx = dbarycentricDx[0] * reciprocalW[0] + dbarycentricDx[1] * reciprocalW[1] + dbarycentricDx[2] * reciprocalW[2];
                                const auto dClipSumDy = dbarycentricDy[0] * reciprocalW[0] + dbarycentricDy[1] * reciprocalW[1] + dbarycentricDy[2] * reciprocalW[2];

                                float dClipDx[3];
                                float dClipDy[3];
                                for (std::size_t k = 0; k < 3; ++k)
                                {
                                    dClipDx[k] = (dbarycentricDx[k] * reciprocalW[k] - clip.v[k] * dClipSumDx) / clipSum;
                                    dClipDy[k] = (dbarycentricDy[k] * reciprocalW[k] - clip.v[k] * dClipSumDy) / clipSum;
                                }

                                FragmentDerivatives derivatives;
                                for (std::size_t t = 0; t < 2; ++t)
                                    for (std::size_t c = 0; c < 2; ++c)
                                    {
                                        derivatives.texCoordsDx[t].v[c] =
                                            vsOutputs[0].texCoords[t].v[c] * dClipDx[0] +
                                            vsOutputs[1].texCoords[t].v[c] * dClipDx[1] +
                                            vsOutputs[2].texCoords[t].v[c] * dClipDx[2];
                                        derivatives.texCoordsDy[t].v[c] =
                                            vsOutputs[0].texCoords[t].v[c] * dClipDy[0] +
                                            vsOutputs[1].texCoords[t].v[c] * dClipDy[1] +
                                            vsOutputs[2].texCoords[t].v[c] * dClipDy[2];
                                    }

                                const float colors[3][4] = {
                                    {vsOutputs[0].color.r, vsOutputs[0].color.g, vsOutputs[0].color.b, vsOutputs[0].color.a},
                                    {vsOutputs[1].color.r, vsOutputs[1].color.g, vsOutputs[1].color.b, vsOutputs[1].color.a},
                                    {vsOutputs[2].color.r, vsOutputs[2].color.g, vsOutputs[2].color.b, vsOutputs[2].color.a}
                                };
                                float colorDx[4]{};
                                float colorDy[4]{};
                                for (std::size_t c = 0; c < 4; ++c)
                                    for (std::size_t k = 0; k < 3; ++k)
                                    {
                                        colorDx[c] += colors[k][c] * dClipDx[k];
                                        colorDy[c] += colors[k][c] * dClipDy[k];
                                    }
                                derivatives.colorDx = Color{colorDx[0], colorDx[1], colorDx[2], colorDx[3]};
                                derivatives.colorDy = Color{colorDy[0], colorDy[1], colorDy[2], colorDy[3]};

                                srcColor = fragmentCallback(psInput, &derivatives);
                            }
                            else
                            {
                                srcColor = fragmentCallback(psInput, nullptr);
                            }

                            if (blendState.enabled)
                            {
                                const auto pixel = reinterpret_cast<std::uint8_t*>(&frameBufferData[screenY * frameBuffer.getWidth() + screenX]);
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

                                frameBufferData[screenY * frameBuffer.getWidth() + screenX] = resultColor.getIntValueRaw();
                            }
                            else
                                frameBufferData[screenY * frameBuffer.getWidth() + screenX] = srcColor.getIntValueRaw();
                        }
                    }
            }
        }
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
        detail::rasterizeTriangles<false>(frameBuffer,
                                          depthBuffer,
                                          vertexShader,
                                          [fragmentShader, &samplers, &textures](const VertexShaderOutput& input,
                                                                                 const detail::FragmentDerivatives*)
                                          {
                                              return fragmentShader(input, samplers, textures);
                                          },
                                          viewport,
                                          scissorRect,
                                          blendState,
                                          depthState,
                                          indices,
                                          vertices,
                                          modelViewProjection);
    }

    // Draws with a compiled material program. Shares the rasterization loop,
    // coverage, perspective interpolation, depth and blending with the
    // function-pointer entry point above; the program output is submitted as
    // the linear fragment color.
    inline void drawTriangles(Texture& frameBuffer,
                              Texture& depthBuffer,
                              VertexShader vertexShader,
                              const MaterialProgram& materialProgram,
                              const Rect<float>& viewport,
                              const Rect<float>& scissorRect,
                              const BlendState& blendState,
                              const DepthState& depthState,
                              const std::vector<std::size_t>& indices,
                              const std::vector<Vertex>& vertices,
                              const Matrix<float, 4>& modelViewProjection)
    {
        detail::rasterizeTriangles<true>(frameBuffer,
                                         depthBuffer,
                                         vertexShader,
                                         [&materialProgram](const VertexShaderOutput& input,
                                                            const detail::FragmentDerivatives* derivatives)
                                         {
                                             MaterialFragment fragment;
                                             fragment.texCoords = input.texCoords;
                                             fragment.color = input.color;

                                             if (derivatives)
                                             {
                                                 fragment.texCoordsDx = derivatives->texCoordsDx;
                                                 fragment.texCoordsDy = derivatives->texCoordsDy;
                                                 fragment.colorDx = derivatives->colorDx;
                                                 fragment.colorDy = derivatives->colorDy;
                                             }

                                             return materialProgram.evaluate(fragment);
                                         },
                                         viewport,
                                         scissorRect,
                                         blendState,
                                         depthState,
                                         indices,
                                         vertices,
                                         modelViewProjection);
    }
}

#endif
