//
//  SoftwareRenderer
//

#ifndef SR_MATERIALGRAPH_HPP
#define SR_MATERIALGRAPH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <vector>
#include "Color.hpp"
#include "Sampler.hpp"
#include "Vector.hpp"

namespace sr
{
    class Texture;

    // Value types a material node can produce.
    enum class MaterialType
    {
        invalid,
        float1,
        float2,
        float4
    };

    [[nodiscard]] constexpr std::size_t getMaterialComponentCount(const MaterialType type) noexcept
    {
        switch (type)
        {
            case MaterialType::float1: return 1;
            case MaterialType::float2: return 2;
            case MaterialType::float4: return 4;
            default: return 0;
        }
    }

    // Operations a material node can perform.
    enum class MaterialOp
    {
        constant,    // no inputs, holds a constant value
        texCoord,    // no inputs, interpolated texture coordinate set (component = set index)
        vertexColor, // no inputs, interpolated vertex color
        component,   // one input, extracts one component (component = component index)
        assemble,    // 1..4 inputs, concatenates their components into a float2 or float4
        add,         // two inputs of the same type, component-wise addition
        multiply,    // two inputs of the same type, component-wise multiplication
        sine,        // one input, component-wise sine
        sample,      // one float2 input, samples a texture, produces a float4
        checker      // one float2 input, filtered checkerboard, produces a float
    };

    // Editable material node graph. Nodes are appended while building or editing
    // the graph; compileMaterialGraph() turns the subgraph reachable from a root
    // into an owning MaterialProgram. Later edits of the graph never affect
    // programs that were compiled before.
    class MaterialGraph final
    {
    public:
        using NodeId = std::uint32_t;
        static constexpr NodeId invalidNode = 0xFFFFFFFFU;

        struct Node final
        {
            MaterialOp op = MaterialOp::constant;
            MaterialType type = MaterialType::invalid;
            std::array<NodeId, 4> inputs{invalidNode, invalidNode, invalidNode, invalidNode};
            std::uint32_t inputCount = 0;
            std::uint32_t component = 0;
            Vector<float, 4> value{};
            const Texture* texture = nullptr;
            Sampler sampler{};
        };

        MaterialGraph() = default;

        NodeId constant(const float value)
        {
            return addConstant(MaterialType::float1, Vector<float, 4>{value, 0.0F, 0.0F, 0.0F});
        }

        NodeId constant(const Vector<float, 2>& value)
        {
            return addConstant(MaterialType::float2, Vector<float, 4>{value.v[0], value.v[1], 0.0F, 0.0F});
        }

        NodeId constant(const Vector<float, 4>& value)
        {
            return addConstant(MaterialType::float4, value);
        }

        NodeId constant(const Color& value)
        {
            return addConstant(MaterialType::float4, Vector<float, 4>{value.r, value.g, value.b, value.a});
        }

        // Interpolated texture coordinate set (0 or 1), produces a float2.
        NodeId texCoord(const std::size_t set)
        {
            Node node;
            node.op = MaterialOp::texCoord;
            node.component = static_cast<std::uint32_t>(set);
            return addNode(node);
        }

        // Interpolated vertex color, produces a float4.
        NodeId vertexColor()
        {
            Node node;
            node.op = MaterialOp::vertexColor;
            return addNode(node);
        }

        // Extracts one component of a float2 or float4 input, produces a float.
        NodeId component(const NodeId input, const std::size_t index)
        {
            Node node;
            node.op = MaterialOp::component;
            node.inputs[0] = input;
            node.inputCount = 1;
            node.component = static_cast<std::uint32_t>(index);
            return addNode(node);
        }

        // Concatenates the components of its inputs into a float2 or float4.
        NodeId assemble(const std::initializer_list<NodeId> inputs)
        {
            if (inputs.size() > 4)
                throw std::invalid_argument{"An assemble node takes at most 4 inputs"};

            Node node;
            node.op = MaterialOp::assemble;
            node.inputCount = static_cast<std::uint32_t>(inputs.size());

            std::size_t slot = 0;
            for (const NodeId input : inputs)
                node.inputs[slot++] = input;

            return addNode(node);
        }

        // Component-wise addition of two same-typed inputs.
        NodeId add(const NodeId a, const NodeId b)
        {
            return addArithmetic(MaterialOp::add, a, b);
        }

        // Component-wise multiplication of two same-typed inputs.
        NodeId multiply(const NodeId a, const NodeId b)
        {
            return addArithmetic(MaterialOp::multiply, a, b);
        }

        // Component-wise sine.
        NodeId sine(const NodeId input)
        {
            Node node;
            node.op = MaterialOp::sine;
            node.inputs[0] = input;
            node.inputCount = 1;
            return addNode(node);
        }

        // Samples a texture with a float2 coordinate, produces a float4.
        // The sampler is copied now; the texture data is snapshotted at compile
        // time, so the texture must only stay alive until the next compile.
        NodeId sample(const NodeId uv, const Texture* texture, const Sampler& sampler)
        {
            Node node;
            node.op = MaterialOp::sample;
            node.inputs[0] = uv;
            node.inputCount = 1;
            node.texture = texture;
            node.sampler = sampler;
            return addNode(node);
        }

        // Filtered checkerboard of a float2 coordinate, produces a float.
        NodeId checker(const NodeId uv)
        {
            Node node;
            node.op = MaterialOp::checker;
            node.inputs[0] = uv;
            node.inputCount = 1;
            return addNode(node);
        }

        // Replaces one input of a node. May create cycles; compileMaterialGraph()
        // rejects programs whose reachable subgraph contains a cycle.
        void setInput(const NodeId node, const std::size_t slot, const NodeId input)
        {
            checkNode(node);
            auto& n = nodes[node];
            if (slot >= n.inputCount)
                throw std::invalid_argument{"Input slot out of range"};
            n.inputs[slot] = input;
            n.type = inferType(n);
        }

        // Replaces the value of a constant node.
        void setConstant(const NodeId node, const float value)
        {
            setConstantValue(node, MaterialType::float1, Vector<float, 4>{value, 0.0F, 0.0F, 0.0F});
        }

        void setConstant(const NodeId node, const Vector<float, 2>& value)
        {
            setConstantValue(node, MaterialType::float2, Vector<float, 4>{value.v[0], value.v[1], 0.0F, 0.0F});
        }

        void setConstant(const NodeId node, const Vector<float, 4>& value)
        {
            setConstantValue(node, MaterialType::float4, value);
        }

        void clear() noexcept
        {
            nodes.clear();
        }

        [[nodiscard]] std::size_t getNodeCount() const noexcept
        {
            return nodes.size();
        }

        [[nodiscard]] MaterialType getNodeType(const NodeId node) const
        {
            checkNode(node);
            return nodes[node].type;
        }

        [[nodiscard]] const Node& getNode(const NodeId node) const
        {
            checkNode(node);
            return nodes[node];
        }

    private:
        void checkNode(const NodeId node) const
        {
            if (node == invalidNode || node >= nodes.size())
                throw std::invalid_argument{"Invalid material node"};
        }

        [[nodiscard]] MaterialType getTypeOf(const NodeId node) const noexcept
        {
            return (node != invalidNode && node < nodes.size()) ? nodes[node].type : MaterialType::invalid;
        }

        // Shallow type inference from the stored input types. The compiler
        // re-infers all types of the reachable subgraph, this only keeps the
        // stored types meaningful for introspection while editing.
        [[nodiscard]] MaterialType inferType(const Node& node) const
        {
            const auto inputType = [this, &node](const std::size_t index)
            {
                return getTypeOf(node.inputs[index]);
            };

            switch (node.op)
            {
                case MaterialOp::constant:
                    return node.type;
                case MaterialOp::texCoord:
                    return MaterialType::float2;
                case MaterialOp::vertexColor:
                    return MaterialType::float4;
                case MaterialOp::component:
                    return MaterialType::float1;
                case MaterialOp::assemble:
                {
                    std::size_t total = 0;
                    for (std::uint32_t i = 0; i < node.inputCount; ++i)
                    {
                        const auto count = getMaterialComponentCount(inputType(i));
                        if (count == 0) return MaterialType::invalid;
                        total += count;
                    }
                    if (total == 2) return MaterialType::float2;
                    if (total == 4) return MaterialType::float4;
                    return MaterialType::invalid;
                }
                case MaterialOp::add:
                case MaterialOp::multiply:
                    return (inputType(0) == inputType(1)) ? inputType(0) : MaterialType::invalid;
                case MaterialOp::sine:
                    return inputType(0);
                case MaterialOp::sample:
                    return MaterialType::float4;
                case MaterialOp::checker:
                    return MaterialType::float1;
            }
            return MaterialType::invalid;
        }

        NodeId addConstant(const MaterialType type, const Vector<float, 4>& value)
        {
            Node node;
            node.op = MaterialOp::constant;
            node.type = type;
            node.value = value;
            const auto id = static_cast<NodeId>(nodes.size());
            nodes.push_back(node);
            return id;
        }

        NodeId addArithmetic(const MaterialOp op, const NodeId a, const NodeId b)
        {
            Node node;
            node.op = op;
            node.inputs[0] = a;
            node.inputs[1] = b;
            node.inputCount = 2;
            return addNode(node);
        }

        NodeId addNode(Node node)
        {
            if (node.op != MaterialOp::constant)
                node.type = inferType(node);
            const auto id = static_cast<NodeId>(nodes.size());
            nodes.push_back(node);
            return id;
        }

        void setConstantValue(const NodeId nodeId, const MaterialType type, const Vector<float, 4>& value)
        {
            checkNode(nodeId);
            auto& node = nodes[nodeId];
            if (node.op != MaterialOp::constant || node.type != type)
                throw std::invalid_argument{"Not a matching constant node"};
            node.value = value;
        }

        std::vector<Node> nodes;
    };
}

#endif
