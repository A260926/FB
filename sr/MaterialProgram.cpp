//
//  SoftwareRenderer
//

#include "MaterialProgram.hpp"
#include <cmath>

namespace sr
{
    namespace
    {
        // s(t) = (-1)^floor(t), also for negative t.
        float checkerWave(const float t)
        {
            const float f = std::floor(t);
            return (f - 2.0F * std::floor(f * 0.5F) == 0.0F) ? 1.0F : -1.0F;
        }

        // P(t) = integral of s from 0 to t: a triangle wave with period 2,
        // P(0) = 0, P(1) = 1. Exact for any t, including negative values.
        double checkerIntegral(const double t)
        {
            const double m = t - 2.0 * std::floor(t * 0.5); // [0, 2)
            return 1.0 - std::fabs(m - 1.0);
        }

        // M(t, h) = mean of s over [t - h/2, t + h/2]; M(t, 0) = s(t).
        // Computed analytically from the antiderivative, so arbitrarily wide
        // footprints spanning many periods stay exact.
        float checkerMean(const float t, const float h)
        {
            if (h <= 0.0F) return checkerWave(t);
            const double hd = h;
            const double td = t;
            return static_cast<float>((checkerIntegral(td + hd * 0.5) - checkerIntegral(td - hd * 0.5)) / hd);
        }

        float checkerFilter(const float u, const float v, const float a, const float b)
        {
            return (1.0F - checkerMean(u, a) * checkerMean(v, b)) * 0.5F;
        }
    }

    Color MaterialProgram::evaluate(const MaterialFragment& fragment) const
    {
        if (instructions.empty())
        {
            lastExecutionCount = 0;
            return Color{};
        }

        auto& values = scratch;
        std::size_t executed = 0;

        for (std::size_t i = 0; i < instructions.size(); ++i)
        {
            const auto& instruction = instructions[i];
            auto& out = values[i];
            const auto componentCount = getMaterialComponentCount(instruction.type);

            switch (instruction.op)
            {
                case MaterialOp::constant:
                {
                    out.c = constants[instruction.data];
                    out.dx = {0.0F, 0.0F, 0.0F, 0.0F};
                    out.dy = {0.0F, 0.0F, 0.0F, 0.0F};
                    break;
                }
                case MaterialOp::texCoord:
                {
                    const auto set = instruction.component;
                    const auto& uv = fragment.texCoords[set];
                    const auto& dx = fragment.texCoordsDx[set];
                    const auto& dy = fragment.texCoordsDy[set];
                    out.c = {uv.v[0], uv.v[1], 0.0F, 0.0F};
                    out.dx = {dx.v[0], dx.v[1], 0.0F, 0.0F};
                    out.dy = {dy.v[0], dy.v[1], 0.0F, 0.0F};
                    break;
                }
                case MaterialOp::vertexColor:
                {
                    out.c = {fragment.color.r, fragment.color.g, fragment.color.b, fragment.color.a};
                    out.dx = {fragment.colorDx.r, fragment.colorDx.g, fragment.colorDx.b, fragment.colorDx.a};
                    out.dy = {fragment.colorDy.r, fragment.colorDy.g, fragment.colorDy.b, fragment.colorDy.a};
                    break;
                }
                case MaterialOp::component:
                {
                    const auto& in = values[instruction.inputs[0]];
                    const auto k = instruction.component;
                    out.c = {in.c[k], 0.0F, 0.0F, 0.0F};
                    out.dx = {in.dx[k], 0.0F, 0.0F, 0.0F};
                    out.dy = {in.dy[k], 0.0F, 0.0F, 0.0F};
                    break;
                }
                case MaterialOp::assemble:
                {
                    out.c = {0.0F, 0.0F, 0.0F, 0.0F};
                    out.dx = {0.0F, 0.0F, 0.0F, 0.0F};
                    out.dy = {0.0F, 0.0F, 0.0F, 0.0F};
                    std::size_t dst = 0;
                    for (std::uint32_t j = 0; j < instruction.inputCount && dst < 4; ++j)
                    {
                        const auto& in = values[instruction.inputs[j]];
                        const auto n = getMaterialComponentCount(instruction.inputTypes[j]);
                        for (std::size_t k = 0; k < n && dst < 4; ++k, ++dst)
                        {
                            out.c[dst] = in.c[k];
                            out.dx[dst] = in.dx[k];
                            out.dy[dst] = in.dy[k];
                        }
                    }
                    break;
                }
                case MaterialOp::add:
                {
                    const auto& a = values[instruction.inputs[0]];
                    const auto& b = values[instruction.inputs[1]];
                    std::size_t k = 0;
                    for (; k < componentCount; ++k)
                    {
                        out.c[k] = a.c[k] + b.c[k];
                        out.dx[k] = a.dx[k] + b.dx[k];
                        out.dy[k] = a.dy[k] + b.dy[k];
                    }
                    for (; k < 4; ++k)
                        out.c[k] = out.dx[k] = out.dy[k] = 0.0F;
                    break;
                }
                case MaterialOp::multiply:
                {
                    const auto& a = values[instruction.inputs[0]];
                    const auto& b = values[instruction.inputs[1]];
                    std::size_t k = 0;
                    for (; k < componentCount; ++k)
                    {
                        out.c[k] = a.c[k] * b.c[k];
                        out.dx[k] = a.dx[k] * b.c[k] + a.c[k] * b.dx[k];
                        out.dy[k] = a.dy[k] * b.c[k] + a.c[k] * b.dy[k];
                    }
                    for (; k < 4; ++k)
                        out.c[k] = out.dx[k] = out.dy[k] = 0.0F;
                    break;
                }
                case MaterialOp::sine:
                {
                    const auto& a = values[instruction.inputs[0]];
                    std::size_t k = 0;
                    for (; k < componentCount; ++k)
                    {
                        out.c[k] = std::sin(a.c[k]);
                        const auto cos = std::cos(a.c[k]);
                        out.dx[k] = cos * a.dx[k];
                        out.dy[k] = cos * a.dy[k];
                    }
                    for (; k < 4; ++k)
                        out.c[k] = out.dx[k] = out.dy[k] = 0.0F;
                    break;
                }
                case MaterialOp::sample:
                {
                    const auto& uv = values[instruction.inputs[0]];
                    const auto sampled = textures[instruction.data].sample(&instruction.sampler,
                                                                           Vector<float, 2>{uv.c[0], uv.c[1]});
                    out.c = {sampled.r, sampled.g, sampled.b, sampled.a};
                    out.dx = {0.0F, 0.0F, 0.0F, 0.0F};
                    out.dy = {0.0F, 0.0F, 0.0F, 0.0F};
                    break;
                }
                case MaterialOp::checker:
                {
                    const auto& uv = values[instruction.inputs[0]];
                    const auto a = std::fabs(uv.dx[0]) + std::fabs(uv.dy[0]);
                    const auto b = std::fabs(uv.dx[1]) + std::fabs(uv.dy[1]);
                    out.c = {checkerFilter(uv.c[0], uv.c[1], a, b), 0.0F, 0.0F, 0.0F};
                    out.dx = {0.0F, 0.0F, 0.0F, 0.0F};
                    out.dy = {0.0F, 0.0F, 0.0F, 0.0F};
                    break;
                }
            }

            ++executed;
        }

        lastExecutionCount = executed;

        // the root instruction is emitted last by the compiler
        const auto& root = values[instructions.size() - 1];
        return Color{root.c[0], root.c[1], root.c[2], root.c[3]};
    }
}
