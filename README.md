# Software Renderer
With this project, I am trying to implement the GPU rendering pipeline in software to understand the steps deeper. This project can be used for learning purposes (I try to keep the code clean).

# Features

* Indexed triangle rasterization
* Depth testing
* Blending
* Texture sampling with clamp, repeat, and mirror address modes
* Custom shader support (by extending the Shader class)
* Point and linear texture filtering
* Reusable prepared meshes (vertex shading + homogeneous clipping done once)
* Continuous hidden-line edge export (boundary / silhouette / crease edges with analytic occlusion)

# Usage

Just include the headers from the sr directory in your project and build. You can check the code in the demo directory and build the demo project to see how to use the library.

## Prepared meshes and hidden-line export

`sr/PreparedMesh.hpp` and `sr/HiddenLines.hpp` (both pulled in by `sr/sr.hpp`, C++17 standard library only) add a reusable mesh preparation result that face drawing and continuous visible-edge output share: the vertex shader runs once per vertex, every triangle is clipped in homogeneous space against `-w <= x, y <= w` and `0 <= z <= w`, and each clipped piece keeps its original face index.

```cpp
sr::PreparedMesh mesh(vertexShader, modelViewProjection, vertices, indices);
```

The result owns all prepared data: after construction the caller may modify or release the vertices, indices, matrix and shaders, and still use the mesh any number of times. `mesh.prepare(...)` re-prepares; on `std::invalid_argument` (illegal indices, degenerate or non-manifold input, non-finite numbers) the previous content is preserved. Several prepared meshes can be used interleaved; no state is shared between them.

### Drawing a prepared mesh

```cpp
sr::drawTriangles(frameBuffer, depthBuffer, fragmentShader, samplers, textures,
                  viewport, scissorRect, blendState, depthState, mesh);
```

This overload uses the existing fragment shader, samplers, textures, depth and blend state. Projected `z/w` is interpolated affinely in screen space; the remaining attributes are perspective-correct (depth does not get a second perspective weighting). The original `drawTriangles(...)` taking a vertex shader, indices, vertices and a matrix is unchanged.

### Exporting visible edges

```cpp
std::vector<sr::VisibleEdgeSegment> segments =
    sr::visibleEdges(mesh, viewPositions, viewport, creaseAngleRadians);
```

`viewPositions` holds one view-space position per original vertex (the eye is at the origin) and is used for face orientation and dihedral angles. The viewport may have a non-zero origin and a non-square size; the screen y direction matches the rest of the library, so the same mesh can be drawn with one viewport and exported with another.

Emitted edges are boundary edges, silhouette edges (one adjacent face front-facing, the other back-facing) and crease edges (angle between the outward normals greater than the threshold, in radians). Coplanar internal diagonals are never emitted, and no frustum cut lines or clipped-fan internal edges are produced: only original edges, clipped against the frustum. Occlusion is computed analytically in the continuous screen domain against all clipped faces treated as double-sided and opaque: an edge point is hidden exactly where a face covers it with strictly smaller depth (equal depth does not occlude; the faces adjacent to an edge never erase it). Segments are sorted by edge index (order of first appearance in the index buffer); each segment carries `index0`/`index1` (the original vertex indices), the linear parameter interval `[t0, t1]` on the original edge (restored from the screen-space parameterization, which differs from it when the endpoint `w`s differ) and the viewport-space endpoints `p0`/`p1`. Touching intervals on the same edge are merged and only segments with a positive screen length are emitted. Invalid view positions, a non-finite crease angle or an invalid viewport throw `std::invalid_argument`.

### Example and tests

`examples/hidden_lines.cpp` renders one scene (a bent bracket with a crease, two overlapping boxes occluding it, one crossing the near plane) and writes `faces.ppm` (prepared-mesh rasterization) and `lines.svg` (the actual visible segments) into a directory given on the command line. `tests/hidden_lines.cpp` registers the `geometry`, `visibility` and `reuse` test cases. To build and run everything, including the original rendering baseline:

```sh
bash task_support/run.sh target
```

# Showcase

The demonstration app is in the demo directory and it can be built for macOS/iOS/tvOS (Xcode project or GNU makefile), Linux/Solaris/BSD (GNU makefile), Windows (Visual Studio project or GNU makefile) and Haiku (GNU makefile). This is a sample output of the renderer (a box with one side transparent and another colored):
![SR sample](https://elviss.lv/files/sr_sample_filtered.png)