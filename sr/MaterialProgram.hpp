//
//  SoftwareRenderer
//

#ifndef SR_MATERIALPROGRAM_HPP
#define SR_MATERIALPROGRAM_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "Color.hpp"
#include "MaterialGraph.hpp"
#include "Sampler.hpp"
#include "Texture.hpp"
#include "Vector.hpp"

namespace sr
{
    // Per-fragment inputs of a material program evaluation: interpolated
    // values plus their analytic screen-space x/y derivatives.
    struct MaterialFragment final
    {
        std::array<Vector<float, 2>, 2> texCoords{};
        std::array<Vector<float, 2>, 2> texCoordsDx{};
        std::array<Vector<float, 2>, 2> texCoordsDy{};
        Color color{};
        Color colorDx{};
        Color colorDy{};
    };

    // Owning compiled material program. Holds snapshots of the constants,
    // topology, samplers and texture data taken at compile time, so it keeps
    // working after the source graph or textures are edited, recompiled or
    // destroyed. Can be drawn with any number of times, interleaved with other
    // programs; evaluation temporaries live in per-program members, never in
    // shared globals.
    class MaterialProgram final
    {
    public:
        MaterialProgram() = default;

        [[nodiscard]] bool isValid() const noexcept
        {
            return !instructions.empty();
        }

        // Number of reachable instructions (shared subexpressions count once).
        [[nodiscard]] std::size_t getInstructionCount() const noexcept
        {
            return instructions.size();
        }

        // Number of texture snapshots taken at compile time.
        [[nodiscard]] std::size_t getTextureCount() const noexcept
        {
            return textures.size();
        }

        // Number of instructions executed by the most recent evaluate() call.
        [[nodiscard]] std::size_t getLastExecutionCount() const noexcept
        {
            return lastExecutionCount;
        }

        // Evaluates the program for one fragment and returns the linear color.
        [[nodiscard]] Color evaluate(const MaterialFragment& fragment) const;

    private:
        friend MaterialProgram compileMaterialGraph(const MaterialGraph& graph,
                                                    MaterialGraph::NodeId root);

        struct Instruction final
        {
            MaterialOp op = MaterialOp::constant;
            MaterialType type = MaterialType::invalid;
            std::array<MaterialType, 4> inputTypes{};
            std::array<std::uint32_t, 4> inputs{};
            std::uint32_t inputCount = 0;
            std::uint32_t component = 0;
            std::uint32_t data = 0; // constant index or texture snapshot index
            Sampler sampler{};
        };

        // Value of one instruction: up to 4 components, each with its
        // screen-space x/y derivatives.
        struct Value final
        {
            std::array<float, 4> c{};
            std::array<float, 4> dx{};
            std::array<float, 4> dy{};
        };

        std::vector<Instruction> instructions;
        std::vector<std::array<float, 4>> constants;
        std::vector<Texture> textures;
        mutable std::vector<Value> scratch;
        mutable std::size_t lastExecutionCount = 0;
    };
}

#endif
