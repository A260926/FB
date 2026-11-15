# Software Renderer
With this project, I am trying to implement the GPU rendering pipeline in software to understand the steps deeper. This project can be used for learning purposes (I try to keep the code clean).

# Features

* Indexed triangle rasterization
* Depth testing
* Blending
* Texture sampling with clamp, repeat, and mirror address modes
* Custom shader support (by extending the Shader class)
* Point and linear texture filtering

# Usage

Just include the headers from the sr directory in your project and build. You can check the code in the demo directory and build the demo project to see how to use the library.

# Material node graphs

Besides raw function-pointer shaders, materials can be assembled as editable node graphs and compiled into owning programs that plug into `drawTriangles`. Graphs support `float`, `float2` and `float4` nodes: constants, two UV sets, vertex color, component extraction, vector assembly, component-wise add/multiply/sin, texture sampling and an anti-aliased checkerboard.

```cpp
#include "sr/sr.hpp"

sr::Texture texture(sr::PixelFormat::rgba8, 64, 64);
// ... fill the texture ...
sr::Sampler sampler;

// 1. Edit: build or modify the node graph (nodes can also be rewired later
//    with setInput/setConstant).
sr::MaterialGraph graph;
const auto uv = graph.texCoord(0);
const auto warped = graph.add(uv, graph.multiply(
    graph.sine(graph.multiply(uv, graph.constant(sr::Vector<float, 2>{3.0f, 3.0f}))),
    graph.constant(sr::Vector<float, 2>{0.3f, 0.3f})));
const auto pattern = graph.checker(graph.multiply(warped, graph.constant(sr::Vector<float, 2>{4.0f, 4.0f})));
const auto texel = graph.sample(uv, &texture, sampler);
const auto mask = graph.assemble({pattern, pattern, pattern, graph.constant(1.0f)});
const auto root = graph.multiply(mask, texel); // the root must produce a float4

// 2. Compile: snapshots constants, topology, samplers and texture data (one
//    snapshot per distinct source texture). The graph and the source texture
//    may be edited or destroyed afterwards; invalid graphs throw
//    sr::MaterialError and never damage previously compiled programs.
sr::MaterialProgram program = sr::compileMaterialGraph(graph, root);

// 3. Draw: shares the raster loop, perspective interpolation, depth and
//    blending with the function-pointer entry point.
sr::drawTriangles(frameBuffer, depthBuffer, vertexShader, program,
                  viewport, scissorRect, blendState, depthState,
                  indices, vertices, modelViewProjection);
```

`checker` nodes are anti-aliased with an exact analytic box filter driven by the screen-space derivatives that are propagated through the coordinate expression. Shared subexpressions are evaluated once per fragment; `program.getInstructionCount()` and `program.getLastExecutionCount()` expose both counts for verification. See `examples/material_program.cpp` for a complete renderer that writes a PPM image (`c++ -std=c++17 -I. examples/material_program.cpp sr/*.cpp -o material_example && ./material_example out.ppm`).


# Showcase

The demonstration app is in the demo directory and it can be built for macOS/iOS/tvOS (Xcode project or GNU makefile), Linux/Solaris/BSD (GNU makefile), Windows (Visual Studio project or GNU makefile) and Haiku (GNU makefile). This is a sample output of the renderer (a box with one side transparent and another colored):
![SR sample](https://elviss.lv/files/sr_sample_filtered.png)