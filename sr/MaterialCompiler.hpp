//
//  SoftwareRenderer
//

#ifndef SR_MATERIALCOMPILER_HPP
#define SR_MATERIALCOMPILER_HPP

#include <stdexcept>
#include "MaterialGraph.hpp"
#include "MaterialProgram.hpp"

namespace sr
{
    // Thrown by compileMaterialGraph() when the reachable subgraph is invalid
    // (reachable cycle, type mismatch, missing input, invalid texture
    // reference or a root that does not produce a float4).
    class MaterialError final: public std::runtime_error
    {
    public:
        using runtime_error::runtime_error;
    };

    // Compiles the subgraph of graph reachable from root into an owning
    // MaterialProgram. Only reachable nodes are considered; the root must
    // produce a float4. Constants, topology, samplers and texture data are
    // snapshotted into the program (one snapshot per distinct source texture),
    // so a failed compile never damages previously compiled programs and later
    // edits of the graph or the textures never affect the returned program.
    [[nodiscard]] MaterialProgram compileMaterialGraph(const MaterialGraph& graph,
                                                       MaterialGraph::NodeId root);
}

#endif
