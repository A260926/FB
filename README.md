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

`ClusteredLighting` (see `src/renderer/ClusteredLighting.h`) adds a CPU
clustered deferred lighting path on top of the existing `VertexProcessor` and
`Rasterizer`. No SDL, GPU or third-party code is used. Geometry is drawn with
the normal vertex processing / rasterization pipeline into a reusable
G-buffer; finite-radius point lights are then assigned to view-frustum
clusters and shading visits only the lights of the covering cluster.

Call sequence:

```c++
#include "ClusteredLighting.h"

using namespace swr;

// 1. Frustum (near/far, near-plane left/right/bottom/top), integer viewport,
//    tile size and logarithmic depth layer count. Asymmetric frustums and
//    non-zero viewport origins are supported.
ClusterConfig config;
config.nearPlane = 1.0f;
config.farPlane = 100.0f;
config.left = -1.0f; config.right = 1.0f;
config.bottom = -1.0f; config.top = 1.0f;
config.viewportX = 0; config.viewportY = 0;
config.viewportWidth = 640; config.viewportHeight = 480;
config.tileWidth = 16; config.tileHeight = 16;
config.depthLayers = 24;

ClusteredLighting lighting(config);
lighting.setRasterMode(RasterMode::Span);   // Span, Block or Adaptive
lighting.setAmbient(0.05f, 0.05f, 0.05f);
// lighting.setDiscardFunction(myDiscardCallback);  // optional

// 2. Begin the G-buffer pass. This installs the scissor, viewport and the
//    reusable G-buffer vertex/pixel shaders. Depth starts at +infinity and
//    every pixel is invalid until a surviving fragment is committed.
lighting.beginGeometry();

// 3. Draw visible surfaces with the existing pipeline. Feed GBufferVertex
//    records (clip position + view-space position, normal and linear albedo).
//    Only non-discarded fragments that pass the [0,1] depth test (smaller
//    depth wins, ties go to the later fragment) write geometry and depth.
std::vector<GBufferVertex> vertices = /* ... */;
std::vector<int> indices = /* ... */;
lighting.vertexProcessor().setVertexAttribPointer(
    0, sizeof(GBufferVertex), vertices.data());
lighting.drawElements(DrawMode::Triangle, indices.size(), indices.data());

// 4. Provide the finite-radius point lights and build the cluster lists.
//    Each light sphere first yields a conservative screen tile and depth
//    layer range; only clusters in that range run the six-plane test, so
//    there is no full cluster/light cartesian product.
std::vector<PointLight> lights = /* ... */;
lighting.setLights(lights);
lighting.buildClusters();

// 5. Shade. Reads only the completed G-buffer and light table and returns an
//    independent W*H floating point RGB image (background stays zero).
std::vector<std::array<float, 3>> image = lighting.shade();

// Real work counters (not estimates).
const ClusterBuildStats &stats = lighting.stats();
// stats.clusterSphereTests  - six-plane sphere tests actually executed
// stats.lightingEvaluations - per-pixel light evaluations performed by shade
```

Cluster layout details:

* Screen tiles use the configured pixel size; the final tile in each row or
  column is clipped to the viewport.
* Depth layer `k` covers `[near*(far/near)^(k/K), near*(far/near)^((k+1)/K)]`;
  internal boundaries belong to the later cluster and `far` belongs to the
  last layer.
* Every cluster is bounded by four side planes through the camera origin and
  two depth planes. A light sphere is a candidate when it intersects all six
  closed half spaces (tangency counts); corner over-collection is allowed but
  lights are never excluded by center or pixel-center tests.
* Each cluster exposes the original light indices in input order, unique and
  with no fixed-capacity truncation.
* `clear()`, `configure()` (resolution change) and `setLights()` reset the
  affected state, so repeated frames and interleaved contexts never see data
  from a previous run.

## License

This code is licensed under the MIT License (see [LICENSE](LICENSE)).