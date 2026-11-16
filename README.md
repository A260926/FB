# CPU Software Rasterizer

A software-rasterizer in C++, that I wrote to get a basic understanding of the OpenGL graphics pipeline.

![alt](img/banner.png)
l.t.r. ["Sad toaster"](https://skfb.ly/on9Dn) by tasha.lime, ["Coatlicue"](https://sketchfab.com/3d-models/coatlicue-dd61c5e8c8734d9399d57c2ad04ec226) by aptass, ["Gameboy Cartridge"](https://sketchfab.com/3d-models/gameboy-cartridge-7c0160c8097a43e0a5e146bf4d7e01a1) by Karolina D, ["Demon Skull"](https://sketchfab.com/3d-models/demon-skull-0cb0efd8bd8a405f89ce5f757ecf6e8d) by Auxiar Molkhun

## Features

- C++ implementation
  - [x] generic vertex and fragment attributes
  - [x] programmable vertex and fragment shader (functors)
  - [x] generic framebuffer targets (for "offscreen" rendering) 
  - [x] small math library (2D, 3D, 4D vectors and 2x2, 3x3, 4x4 matrices)
  - [x] .obj and .mat loading
  - [x] GLFW/OpenGL viewer (uploads framebuffer each frame)
- Rasterizer
  - [x] perspective-correct attribute interpolation
  - [x] z-buffering
  - [x] texture sampler filter (nearest, linear)
  - [x] texture sampler wrapping (repeat, edge) 
  - [x] face culling
  - [x] custom framebuffer
  - [x] line rendering (wireframe rendering)
  - [x] mip map generation
  - [ ] mip map level computation
  - [ ] anisotropic filtering
  - [ ] cubemap
- Examples
  - [x] minimal examples
    - [x] colored triangle
    - [x] index buffer
    - [x] texture and sampler
    - [x] custom framebuffer
    - [x] glfw/gl viewer
    - [x] model loading
  - [x] Blinn-Phong illumination
  - [x] Cel Shading with depth buffer edge detection
  - [x] Normal Mapping
  - [x] Shadow Mapping
  - [X] Screen Space Ambient Occlusion
  - [X] Physically-based rendering + offline pre-integration    

## Examples

### Minimal setup (colored triangle)

Similar to traditional Graphic APIs we define the input and output of the shader stages, i.e. the data passing through the rasterization pipeline.

All members from `Varying` contained in the `VARYING(...)` macro are interpolated per fragment by the rasterizer.
Note, that it is mandatory to have a `Vec4 position` interpolated attribute.
```cpp
/* vertex data -> input to draw call (via Buffer) */
struct Vertex
{
    Vec3 pos;
    Vec3 color;
};

/*
 * Output of vertex stage, Input to fragment stage
 * -> position is mandatory
 * -> members to interpolate are declared by VARYING macro (member need scalar multiplication, and addition)
*/
struct Varying
{
    Vec4 position;
    Vec3 color;

    VARYING(position, color);
};

/* uniform struct accessable from both "shaders" */
struct Uniforms {};
```
Vertex and fragment shaders are written as function objects which need to be set accordingly with `onVertex` and `onFragment` (currently no default shader).
```cpp
Program<Vertex, Varying, Uniforms> program;
program.onVertex([](const Uniforms& uniform, const Vertex& in, Varying& out)
{
    out.position = Vec4(in.pos, 1.0f);
    out.color = in.color;
});

program.onFragment([](const Uniforms& uniform, const Varying& in, Vec4& out)
{
    out = Vec4(in.color, 1.0f);
});
```

Mesh data is provided to the renderer with a `Buffer` object.

```cpp
Buffer<Vertex> buffer;
buffer.primitive = ePrimitive::TRIANGLES;
buffer.vertices = { { {-0.5, -0.5, 0.5}, {1.0, 0.0, 0.0} },
                    { { 0.5, -0.5, 0.5}, {0.0, 1.0, 0.0} },
                    { { 0.0,  0.5, 0.5}, {0.0, 0.0, 1.0} } };

```
An instance of `Renderer` contains a default framebuffer with a color and depth target.

After clearing the framebuffer, a draw call can be submitted with the previously defined shader program and vertex buffer.

<img src="img/00_triangle.png" align="right" height=250px>

```cpp
/* rasterizer with framebuffer size */
Renderer rasterizer(480, 480);

/* clear framebuffer */
rasterizer.framebuffer().clear(Vec4(0, 0, 0, 1));

/* submit draw call */
rasterizer.draw(program, buffer);

/* save framebuffer as .png */
rasterizer.framebuffer().color().save("00_triangle.png");

```
## Results

Model Loading and Texture Mapping ("Sad toaster" [Link](https://skfb.ly/on9Dn) by tasha.lime.)
![alt](img/example_model.jpg)

Blinn-Phong Illumination with diffuse, normal and ambient occlusion map ("Demon Skull" [Link](https://sketchfab.com/3d-models/demon-skull-0cb0efd8bd8a405f89ce5f757ecf6e8d) by Auxiar Molkhun)
![alt](img/example_ao_map.png)

Physically-based rendering with pre-integrated irradiance and radiance maps ("Bilora Bella 46 Camera" [Link](https://sketchfab.com/3d-models/dae-bilora-bella-46-camera-game-ready-asset-eeb9d9f0627f4783b5d16a8732f0d1a4) by [Martijn Vaes](https://sketchfab.com/MartijnVaes) licensed under [CC-BY-4.0](http://creativecommons.org/licenses/by/4.0/))
![alt](img/example_pbr.png)

Cel-Shading with post-process edge detection on the depth buffer ("Bird" [Link](https://sketchfab.com/3d-models/bird-e63479b07d8c4ae6a69b34247d572299)
 by [Václav Pleticha](https://sketchfab.com/klidas8) licensed under [CC-BY-4.0](http://creativecommons.org/licenses/by/4.0/))
![alt](img/example_cel_shading.png)

Shadow Mapping (based on "low poly house" [Link](https://sketchfab.com/3d-models/low-poly-house-02635c7c191848699e88085c2ab27538) by [cofitelle](https://sketchfab.com/cofitelle) licensed under CC-BY-4.0 [CC-BY-4.0](http://creativecommons.org/licenses/by/4.0/))
![alt](img/example_shadow_mapping.png)

Screen-space ambient occlussion ("Pokemon FireRed - Player's Room" [Link](https://sketchfab.com/3d-models/pokemon-firered-players-room-b23b6b253207463c97db2a7092adff74) by [Wesai](https://sketchfab.com/Wesai) licensed under [CC-BY-4.0](http://creativecommons.org/licenses/by/4.0/))
![alt](img/example_ssao.png)

## Per-Face Quad Textures

The `face_texture.h` module gives every quadrilateral face its own
independent linear floating-point `Vec4` image. No atlas is built and
neighbouring faces may use different (power-of-two, possibly rectangular)
resolutions.

### Topology and edge direction

A face is an oriented quad whose shared position indices name its local
corners in the order `(0,0) (1,0) (1,1) (0,1)`. Each face is rasterized as
two triangles, `0-1-2` and `0-2-3`. Boundary edges are allowed. Resource
construction rejects an edge that is incident to more than two faces and a
shared edge that the two faces traverse in the same direction; the faces of
a manifold edge must point in opposite directions along it.

### Mip levels and gutters

Each face generates its own mip chain down to `1x1`: every axis halves
independently and clamps at one, so an axis that already has size one keeps
sampling the same coordinate while the other axis keeps shrinking. Every
level carries its interior texels plus a one-cell gutter ring. Sampling in
a level uses bilinear weights at the continuous coordinates
`uv * size - 0.5`, so the gutter cells participate in edge interpolation,
and the two neighbouring levels are blended by the clamped fractional LOD.
UV is restricted to the unit face square and the LOD must be explicit and
finite; no anisotropic filtering is performed.

- **Edge gutters** – a gutter cell represents the local rectangle of that
  level (width `1/W`, height `1/H`). It is unfolded across the shared edge
  into the neighbour's unit square. The shared vertex ordering determines
  the reversed direction along the edge and the axis permutation, and the
  cell stores an exact area-weighted average of the neighbour's
  piecewise-constant base texels covered by the unfolded rectangle. It is
  never a copy of the neighbour's nearest row and is never taken from the
  neighbour's integer mip level. Boundary edges instead extend this face's
  own edge texel.
- **Corner gutters** – a corner cell averages the base corner texel of
  *every* face incident to that topology vertex (the whole vertex star),
  with equal weights, not just the two edge-adjacent faces. Boundary
  corners follow the same rule, so a star of one reproduces the face's own
  corner texel.

### Snapshots, partial rebuilds and lifetime

`FaceMaterial` owns the live state and publishes immutable,
reference-counted `FaceSnapshot` objects. Replacing one face's base data or
size rebuilds only:

1. that face's interior mip chain and all of its gutters,
2. the edge gutters, at every level, of faces sharing an edge with it, and
3. the corner gutters of every face in the vertex stars of its four
   corners.

Faces and levels with no dependency keep their storage object and are
shared between the old and new snapshot; the whole mesh is never copied or
regenerated. `rebuild_count()` reports the number of published snapshots and
`layer_storage(face, level)` exposes the read-only layer storage identity so
this scope can be verified. A failed update publishes nothing and leaves the
previous snapshot in place.

A snapshot is self-contained: it stays valid and drawable after the source
material and all original input arrays have been destroyed, and old and new
snapshots never modify one another. Draw calls keep no global "current face"
or "current texture" state, so two `Renderer` instances can interleave
quad draws. The quad entry `Renderer::drawQuads` reuses the existing
triangle rasterization, depth test and `Program` vertex/fragment flow; the
flat per-face id replicated on a face's four corners interpolates to the
exact original face number on both sub-triangles (an interpolatable vertex
index is never used as a face id). The legacy triangle, indexed-buffer and
`Sampler` paths remain unchanged. See `examples/face_texture.cpp` and the
`topology`, `gutters` and `rendering` groups in `checks/target.cpp`.

## :books: Useful Resources
[Tiny Renderer](https://github.com/ssloy/tinyrenderer/wiki/Lesson-0:-getting-started)   
[Scratchpixel Lesson](https://www.scratchapixel.com/lessons/3d-basic-rendering/rasterization-practical-implementation/overview-rasterization-algorithm.html)
[Rasterator](https://github.com/diharaw/Rasterator)   
[SRPBR](https://github.com/niepp/srpbr)   
[Stack Overflow Perspective Interpolation](https://stackoverflow.com/questions/24441631/how-exactly-does-opengl-do-perspectively-correct-linear-interpolation)   
[learnopengl Tutorials](https://learnopengl.com/PBR/Lighting)   
[IBL Blogpost by Bruno Opsenica](https://bruop.github.io/ibl/)