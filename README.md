# SoftGLRender

[![CMake MacOS](https://github.com/keith2018/SoftGLRender/actions/workflows/cmake_macos.yml/badge.svg)](https://github.com/keith2018/SoftGLRender/actions/workflows/cmake_macos.yml)
[![CMake Windows](https://github.com/keith2018/SoftGLRender/actions/workflows/cmake_windows.yml/badge.svg)](https://github.com/keith2018/SoftGLRender/actions/workflows/cmake_windows.yml)
[![CMake Linux](https://github.com/keith2018/SoftGLRender/actions/workflows/cmake_linux.yml/badge.svg)](https://github.com/keith2018/SoftGLRender/actions/workflows/cmake_linux.yml)

Tiny C++ software renderer/rasterizer that implements the main steps of the GPU rendering pipeline, including point,
line and polygon rasterization, texture mapping, depth testing, color blending, etc., and emulates vertex shaders and
fragment shaders using C++, 3D models (GLTF) are loaded
by [Assimp](https://github.com/assimp/assimp), and using [GLM](https://github.com/g-truc/glm) as math library. The
project also adds OpenGL and Vulkan renderers implementation, you can switch between them (Software/OpenGL/Vulkan) in
real time while running.

The purpose of this project is to provide a starting point for developers who want to learn about modern graphics
programming.

## Examples

|                               |                                |
|-------------------------------|--------------------------------|
| ![](screenshot/boombox.png)   | ![](screenshot/robot.png)      |
| ![](screenshot/helmet.png)    | ![](screenshot/glasstable.png) |
| ![](screenshot/brickwall.png) | ![](screenshot/cube.png)       |

## Features

### Software Renderer

#### Pipeline

- Vertex shading
- View Frustum culling (line & triangle)
- Perspective Correct Interpolation
- Back-Front face culling
- Point/Line/Triangle rasterization
- Fragment Shading
- Shader derivative `dFdx` `dFdy`
- Depth test & Alpha blending
- Early Z test & Reversed Z
- MSAA 4x

#### Texture Mapping

- Mipmaps generation and sampling
- Sample parameters: `Lod`, `Bias`, `Offset`
- All kinds of texture filtering & wrapping mode
- Image storage tiling and swizzling
    - Linear: pixel values are stored line by line, commonly used as image RGBA buffer
    - Tiled: block base storage, inside the block pixels are stored as `Linear`
    - Morton: block base storage, inside the block pixels are stored as morton pattern (similar to zigzag)

#### Optimization

- Multi-Threading: rasterization is block based with multi-threading support
- SIMD: barycentric coordinate calculation, shader's varying interpolation, etc.

### Viewer

- Config panel based on `imgui`
- Orbit camera controller
- Blinn-Phong shading
- PBR & IBL shading
- Skybox cubeMap & equirectangular
- Shadow mapping
- FXAA anti-aliasing
- RenderDoc in-application frame capture

## Renderer abstraction

All renderers (Software/OpenGL/Vulkan) are implemented based on this abstract renderer class:

```cpp
class Renderer {
 public:
  // framebuffer
  std::shared_ptr<FrameBuffer> createFrameBuffer(bool offscreen);

  // texture
  std::shared_ptr<Texture> createTexture(const TextureDesc &desc);

  // vertex
  std::shared_ptr<VertexArrayObject> createVertexArrayObject(const VertexArray &vertexArray);

  // shader program
  std::shared_ptr<ShaderProgram> createShaderProgram();

  // pipeline states
  std::shared_ptr<PipelineStates> createPipelineStates(const RenderStates &renderStates);

  // uniform
  std::shared_ptr<UniformBlock> createUniformBlock(const std::string &name, int size);
  std::shared_ptr<UniformSampler> createUniformSampler(const std::string &name, const TextureDesc &desc);

  // pipeline
  void beginRenderPass(std::shared_ptr<FrameBuffer> &frameBuffer, const ClearStates &states);
  void setViewPort(int x, int y, int width, int height);
  void setVertexArrayObject(std::shared_ptr<VertexArrayObject> &vao);
  void setShaderProgram(std::shared_ptr<ShaderProgram> &program);
  void setShaderResources(std::shared_ptr<ShaderResources> &uniforms);
  void setPipelineStates(std::shared_ptr<PipelineStates> &states);
  void draw();
  void endRenderPass();
};
```

## Recorded Command Lists (Software Renderer)

The software backend can also record a group of CPU draws once and hand the
recorded list to another software renderer to replay repeatedly into different
offscreen targets. Recording never runs the vertex or fragment shader and never
stores the final image or pre-transformed vertices: it freezes the currently
bound state so the list stays valid after every source object (VAO, uniform
blocks, `ShaderResources`, `ShaderProgram`, textures, external `Buffer`s) and
even the recording `RendererSoft` / `SnapshotStore` have been destroyed.

All types live in `Render/Software/CommandListSoft.h`:

```cpp
namespace SoftGL {

class CommandListSoft {
 public:
  // Append an immutable copy of the currently bound state of `renderer`.
  // Runs no vertex or fragment shader and stores no final image or
  // transformed vertices. Returns false (leaving the list and store
  // untouched) for an incomplete binding or an unsupported resource.
  bool appendFrom(RendererSoft &renderer, SnapshotStore &snapshots);

  // Replay in recorded order onto `renderer` / `frameBuffer`.
  // The caller owns clearing and target selection; the previous target,
  // viewport, render states, VAO and program are restored before returning.
  bool replay(RendererSoft &renderer, std::shared_ptr<FrameBuffer> &frameBuffer);

  size_t size() const;
  bool empty() const;
};

class SnapshotStore {
 public:
  // Number of distinct image / mesh snapshots currently alive
  size_t imageSnapshotCount() const;
  size_t meshSnapshotCount() const;
  size_t snapshotCount() const;

  // Bytes held by live snapshots and the cumulative bytes copied so far
  size_t heldBytes() const;
  size_t copiedBytes() const;
};

}
```

Each `appendFrom` freezes the vertex/index buffers, viewport, Early-Z flag,
`RenderStates`, program defines, uniform bytes, and every valid sampler binding
together with its sampling state and a copy of the image content. Supported
images are 2D RGBA8 and FLOAT32 textures including a complete mip chain;
multi-sample sampler images are rejected before anything is recorded.

Deduplication is content based: two samplers pointing at the same texture share
one frozen image while keeping independent sampling states, and appending the
same content twice reuses the already-live snapshot instead of copying it
again. Rewriting an external `Buffer` (including via `Buffer::set`) is picked
up on the next append, because the fingerprint is recomputed from the pixels
rather than a manual dirty flag. The cache only holds weak references, so a
snapshot is released as soon as no command list uses it.

The original immediate-drawing interface is unchanged, and shader subclasses
still follow the existing `clone()` contract; only explicitly bound resources
are frozen, and pointers a shader borrows on its own are not taken over.

`checks/target.cpp` exercises the capture, ownership and replay behaviour, and
`examples/recorded_material.cpp` records two materials, releases the source
objects and replays onto two offscreen targets as PPM images. Run them with:

```bash
bash task_support/run.sh target
```


## Getting Started

### Prerequisites

To build the project, you must first install the following tools:

- CMake 3.10 or higher
- Compiler environment with C++11 support

If you want to run the vulkan renderer, you need to install vulkan library as follows:

1. Download the Vulkan SDK from the official website: https://vulkan.lunarg.com/sdk/home
2. Install the SDK on your system. The installation process may vary depending on your operating system.
3. Set the environment variable VULKAN_SDK/VK_ICD_FILENAMES/VK_LAYER_PATH to the path where you installed the SDK.

### Getting the Code

To get the code for this library, you can clone the GitHub repository using the following command:

```bash
git clone https://github.com/keith2018/SoftGLRender.git
```

Alternatively, you can download the source code as a ZIP file from the GitHub repository.

### Building the Project

To build the project, navigate to the root directory of the repository and run the following commands:

```bash
mkdir build
cmake -B ./build -DCMAKE_BUILD_TYPE=Release
cmake --build ./build --config Release
```

This will generate the executable file and copy assets to directory `bin`, which you can run by executing the following
command:

```bash
cd bin/Release
./SoftGLRender
```

## Directory structure

- `assets`: GLTF models and skybox textures, `assets.json` is the index of all model & skybox materials
- `src`: Main source code directory
    - `Base`: Basic utility classes like file, hash, timer, etc.
    - `Render`: Renderer abstraction, include vertex, texture, uniform, framebuffer, etc.
        - `Software`: Software renderer implementation
        - `OpenGL`: OpenGL renderer implementation
        - `Vulkan`: Vulkan renderer implementation
    - `Viewer`: GLTF loading (based on Assimp), camera & controller, config panel, and render pass management
        - `Shader/GLSL`: GLSL shader code, for OpenGL & Vulkan renderer
        - `Shader/Software`: C++ simulation of vertex shaders and fragment shaders
- `third_party`: External libraries and assets

## Third Party Libraries

- `assimp` [https://github.com/assimp/assimp](https://github.com/assimp/assimp)
- `glfw` [https://github.com/glfw/glfw](https://github.com/glfw/glfw)
- `glm` [https://github.com/g-truc/glm](https://github.com/g-truc/glm)
- `glslang` [https://github.com/KhronosGroup/glslang](https://github.com/KhronosGroup/glslang)
- `imgui` [https://github.com/ocornut/imgui](https://github.com/ocornut/imgui)
- `json11` [https://github.com/dropbox/json11](https://github.com/dropbox/json11)
- `stb_image` [https://github.com/nothings/stb](https://github.com/nothings/stb)
- `vma` [https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)

## Contributing

If you would like to contribute to the project, you are welcome to submit pull requests with bug fixes, new features, or
other improvements. Please ensure that your code is well-documented and adheres to the project's coding standards.

## License

This code is licensed under the MIT License (see [LICENSE](LICENSE)).
