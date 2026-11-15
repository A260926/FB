//
//  SoftwareRenderer
//

#include "MaterialCompiler.hpp"
#include <utility>
#include <vector>

namespace sr
{
    MaterialProgram compileMaterialGraph(const MaterialGraph& graph, const MaterialGraph::NodeId root)
    {
        const auto nodeCount = graph.getNodeCount();
        if (root == MaterialGraph::invalidNode || root >= nodeCount)
            throw MaterialError{"Invalid material root node"};

        // Iterative depth-first traversal of the reachable subgraph with cycle
        // detection. Emits nodes in post-order, so every instruction appears
        // after its inputs and shared subexpressions appear exactly once.
        enum class VisitState : std::uint8_t
        {
            none,
            visiting,
            done
        };

        std::vector<VisitState> states(nodeCount, VisitState::none);
        std::vector<MaterialGraph::NodeId> order;
        std::vector<std::pair<MaterialGraph::NodeId, std::uint32_t>> stack;

        states[root] = VisitState::visiting;
        stack.emplace_back(root, 0);

        while (!stack.empty())
        {
            const auto id = stack.back().first;
            const auto& node = graph.getNode(id);
            auto& nextInput = stack.back().second;

            if (nextInput < node.inputCount)
            {
                const auto input = node.inputs[nextInput++];
                if (input == MaterialGraph::invalidNode || input >= nodeCount)
                    throw MaterialError{"Material node is missing an input"};
                if (states[input] == VisitState::visiting)
                    throw MaterialError{"Material graph contains a reachable cycle"};
                if (states[input] == VisitState::none)
                {
                    states[input] = VisitState::visiting;
                    stack.emplace_back(input, 0);
                }
            }
            else
            {
                states[id] = VisitState::done;
                order.push_back(id);
                stack.pop_back();
            }
        }

        // Type inference and validation in evaluation order (inputs first).
        std::vector<MaterialType> types(nodeCount, MaterialType::invalid);

        for (const auto id : order)
        {
            const auto& node = graph.getNode(id);
            const auto inputType = [&types, &node](const std::size_t index)
            {
                return types[node.inputs[index]];
            };

            auto type = MaterialType::invalid;

            switch (node.op)
            {
                case MaterialOp::constant:
                    type = node.type;
                    break;
                case MaterialOp::texCoord:
                    if (node.component >= 2)
                        throw MaterialError{"Texture coordinate set index out of range"};
                    type = MaterialType::float2;
                    break;
                case MaterialOp::vertexColor:
                    type = MaterialType::float4;
                    break;
                case MaterialOp::component:
                {
                    if (node.inputCount != 1)
                        throw MaterialError{"Component node expects one input"};
                    const auto count = getMaterialComponentCount(inputType(0));
                    if (count < 2)
                        throw MaterialError{"Component extraction requires a float2 or float4 input"};
                    if (node.component >= count)
                        throw MaterialError{"Component index out of range"};
                    type = MaterialType::float1;
                    break;
                }
                case MaterialOp::assemble:
                {
                    if (node.inputCount < 1)
                        throw MaterialError{"Assemble node expects at least one input"};
                    std::size_t total = 0;
                    for (std::uint32_t i = 0; i < node.inputCount; ++i)
                        total += getMaterialComponentCount(inputType(i));
                    if (total == 2) type = MaterialType::float2;
                    else if (total == 4) type = MaterialType::float4;
                    else throw MaterialError{"Assemble node inputs must add up to 2 or 4 components"};
                    break;
                }
                case MaterialOp::add:
                case MaterialOp::multiply:
                {
                    if (node.inputCount != 2)
                        throw MaterialError{"Arithmetic node expects two inputs"};
                    if (inputType(0) != inputType(1))
                        throw MaterialError{"Arithmetic node inputs must have the same type"};
                    type = inputType(0);
                    break;
                }
                case MaterialOp::sine:
                {
                    if (node.inputCount != 1)
                        throw MaterialError{"Sine node expects one input"};
                    type = inputType(0);
                    break;
                }
                case MaterialOp::sample:
                {
                    if (node.inputCount != 1)
                        throw MaterialError{"Sample node expects one input"};
                    if (inputType(0) != MaterialType::float2)
                        throw MaterialError{"Sample node coordinates must be a float2"};
                    if (!node.texture)
                        throw MaterialError{"Sample node has no texture"};
                    if (node.texture->getLevelCount() == 0 ||
                        node.texture->getWidth() == 0 ||
                        node.texture->getHeight() == 0)
                        throw MaterialError{"Sample node references an empty texture"};
                    type = MaterialType::float4;
                    break;
                }
                case MaterialOp::checker:
                {
                    if (node.inputCount != 1)
                        throw MaterialError{"Checker node expects one input"};
                    if (inputType(0) != MaterialType::float2)
                        throw MaterialError{"Checker node coordinates must be a float2"};
                    type = MaterialType::float1;
                    break;
                }
                default:
                    throw MaterialError{"Unknown material node operation"};
            }

            if (type == MaterialType::invalid)
                throw MaterialError{"Material node has an invalid type"};

            types[id] = type;
        }

        if (types[root] != MaterialType::float4)
            throw MaterialError{"Material root node must produce a float4"};

        // Emit the owning program: constants, topology, samplers and one
        // texture snapshot per distinct source texture.
        MaterialProgram program;
        program.instructions.reserve(order.size());

        std::vector<std::uint32_t> instructionIndices(nodeCount, 0);
        std::vector<std::pair<const Texture*, std::uint32_t>> textureIndices;

        for (const auto id : order)
        {
            const auto& node = graph.getNode(id);

            MaterialProgram::Instruction instruction;
            instruction.op = node.op;
            instruction.type = types[id];
            instruction.inputCount = node.inputCount;
            instruction.component = node.component;

            for (std::uint32_t i = 0; i < node.inputCount; ++i)
            {
                instruction.inputs[i] = instructionIndices[node.inputs[i]];
                instruction.inputTypes[i] = types[node.inputs[i]];
            }

            if (node.op == MaterialOp::constant)
            {
                instruction.data = static_cast<std::uint32_t>(program.constants.size());
                program.constants.push_back({node.value.v[0], node.value.v[1], node.value.v[2], node.value.v[3]});
            }
            else if (node.op == MaterialOp::sample)
            {
                auto textureIndex = static_cast<std::uint32_t>(program.textures.size());
                bool found = false;
                for (const auto& entry : textureIndices)
                    if (entry.first == node.texture)
                    {
                        textureIndex = entry.second;
                        found = true;
                        break;
                    }
                if (!found)
                {
                    textureIndices.emplace_back(node.texture, textureIndex);
                    program.textures.push_back(*node.texture); // snapshot the texture data
                }
                instruction.data = textureIndex;
                instruction.sampler = node.sampler; // snapshot the sampler state
            }

            instructionIndices[id] = static_cast<std::uint32_t>(program.instructions.size());
            program.instructions.push_back(instruction);
        }

        program.scratch.resize(program.instructions.size());
        program.lastExecutionCount = 0;
        return program;
    }
}
