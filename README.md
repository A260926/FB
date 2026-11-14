# C++ Software Renderer/Rasterizer

This project implements a C++ software renderer/rasterizer with vertex- and
pixel shader support.

## Features
* Generic vertex arrays for arbitrary data in the vertex processing stage.
* Internal vertex cache for better vertex processing.
* Affine and perspective correct per vertex parameter interpolation.
* Vertex and pixel shaders written in C++ using some C++ template magic.

## Resources

* [Triangle Rasterization](http://www.cs.unc.edu/~blloyd/comp770/Lecture08.pdf)
* [Accelerated Half-Space Triangle Rasterization](https://www.researchgate.net/publication/286441992_Accelerated_Half-Space_Triangle_Rasterization)

## Example

```c++

#include "Renderer.h"

// Define a pixel shader
class PixelShader : public PixelShaderBase<PixelShader> {
public:
  static const bool InterpolateZ = true;
  static const bool InterpolateW = true;

  // Number of affine/linear variables used.
  static const int AVarCount = 3;

  // Number of perspective correct variables used.
  static const int PVarCount = 2;

  static void drawPixel(const PixelData &p)
  {
      ...
  }
};

// Define a vertex shader
class VertexShader : public VertexShaderBase<VertexShader> {
public:
  // Number of input vertex attributes used.
  static const int AttribCount = 1;

  static void processVertex(VertexShaderInput in, VertexShaderOutput *out)
  {
    // Write result to out.
    ...
  }
};

// Use the renderer
Rasterizer r;
VertexProcessor v(&r);

r.setRasterMode(RasterMode::Span);
r.setScissorRect(0, 0, 640, 480);
r.setPixelShader<PixelShader>();

v.setViewport(0, 0, 640, 480);
v.setCullMode(CullMode::CW);
v.setVertexShader<VertexShader>();

// Draw
v.setVertexAttribPointer(0, sizeof(VertexData), vertexData);
v.drawElements(DrawMode::Triangle, indexData.size(), indexData);

```

## CPU clustered deferred lighting

`ClusteredLighting` adds a CPU deferred path so a fragment shader no longer has
to loop over every point light. Visible surfaces are first rasterized into a
geometry buffer, finite-radius point lights are assigned to frustum clusters,
and shading iterates only over the lights of the cluster each pixel belongs
to. The original renderer interfaces and all three raster modes are unchanged.

### Call order

```c++
#include "ClusteredLighting.h"

using namespace swr;

ClusteredLighting ctx;

// 1. Begin a geometry pass: allocates/clears the buffers and installs the
//    geometry buffer pixel shader on the owned rasterizer.
ctx.beginGeometryPass(width, height);
ctx.rasterizer().setRasterMode(RasterMode::Block); // Span/Block/Adaptive all work
ctx.rasterizer().setScissorRect(vx, vy, width, height);
ctx.vertexProcessor().setViewport(vx, vy, width, height);
ctx.vertexProcessor().setCullMode(CullMode::None);

// Optional: discard fragments before the depth test.
ctx.setDiscardCallback([](const GeometryFragment &f) { return /* ... */; });

// 2. Draw visible surfaces with the existing VertexProcessor/Rasterizer.
//    Use GBufferVertexShader and feed GBufferVertex vertices (clip position
//    plus view-space position, normal and linear RGB albedo). Only fragments
//    that are not discarded and pass the depth test write geometry + depth.
ctx.vertexProcessor().setVertexAttribPointer(0, sizeof(GBufferVertex), vertices);
ctx.vertexProcessor().setVertexShader<GBufferVertexShader>();
ctx.vertexProcessor().drawElements(DrawMode::Triangle, indexCount, indices);
ctx.endGeometryPass();

// 3. Describe the perspective frustum and the cluster grid.
ClusterConfig cfg;
cfg.nearPlane = near; cfg.farPlane = far;
cfg.left = l; cfg.right = r; cfg.bottom = b; cfg.top = t; // near-plane rectangle
cfg.viewportX = vx; cfg.viewportY = vy;
cfg.viewportWidth = width; cfg.viewportHeight = height;
cfg.tileWidth = tw; cfg.tileHeight = th; // last tile is clipped to the viewport
cfg.depthSlices = K;                     // logarithmic depth layers
ctx.configureClusters(cfg);

// 4. Assign lights. Depth layers use d_k = near*(far/near)^(k/K); each cluster
//    is bounded by four origin side planes and two depth planes. A light is a
//    candidate when its sphere intersects all six closed half spaces
//    (tangency kept). Lists hold original light indices, input order, unique.
ctx.buildClusters(lights);

// 5. Shade from the completed buffers. Returns an independent linear RGB image
//    (ambient*albedo plus the per-light term, never clamped); background is 0.
std::vector<Color3f> image = ctx.shade(lights, ambient);

// Real counters (not estimates) for the work actually performed.
std::size_t sphereTests = ctx.clusterSphereCheckCount();
std::size_t lightCalcs  = ctx.lightCalculationCount();
```

### Cluster assignment performance

`buildClusters` never tests the Cartesian product of clusters and lights. For
each light it derives a conservative depth-layer range from the sphere's depth
extent and a contiguous screen tile range by testing the sphere against the
ordered, origin-passing tile side planes, then runs the full six-plane test
only inside that range. This stays conservative for near-plane crossings,
spheres whose center is behind the eye, and asymmetric frusta; a sphere that
crosses the eye plane simply expands to the full tile range.

## License

This code is licensed under the MIT License (see [LICENSE](LICENSE)).