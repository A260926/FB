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

# Mip level render targets

A `Texture` created with `initMipMaps = true` owns a chain of levels, each
halving its dimensions (never below 1x1). A `TextureView` references one level
without owning the texture and can be passed wherever a render target is
accepted; the plain `Texture&` overloads keep meaning level zero.

## Post-processing example

Render an effect into a small mip level of one texture, then draw a second
triangle that samples that texture at a chosen LOD. Writes through
`setData`, `clear` and `drawTriangles` refresh all descendant levels
automatically; bytes edited directly through `getData` need an explicit
`generateMipMaps` call.

```cpp
#include "sr/sr.hpp"

// One mip-mapped color target, e.g. 256x128 -> 128x64 -> 64x32 -> ... -> 1x1.
sr::Texture sceneTexture{sr::PixelFormat::rgba8, 256, 128, true};
sr::Texture depthTexture{sr::PixelFormat::float32, 256, 128, true};

sr::Sampler pointSampler; // point filtering, LOD 0, clamped UVs

// 1. Draw the scene into level 0 as usual (the Texture& overloads pick level 0).
sr::clear(sceneTexture, sr::Color{0.0F, 0.0F, 0.0F, 1.0F});
sr::clear(depthTexture, 1.0F);
sr::drawTriangles(sceneTexture, depthTexture,
                  vertexShader, sceneFragmentShader,
                  {&pointSampler, nullptr}, {nullptr, nullptr},
                  sr::Rect<float>{0, 0, 256, 128},
                  sr::Rect<float>{0, 0, 1, 1},
                  sr::BlendState{}, depthState,
                  indices, vertices, modelViewProjection);
// Descendant levels now hold the box-averaged chain of the scene.

// 2. Render a blur/effect pass directly into the 64x32 level (level 2).
sr::TextureView smallColor{sceneTexture, 2};
sr::TextureView smallDepth{depthTexture, 2};
sr::clear(smallColor, sr::Color{0.0F, 0.0F, 0.0F, 1.0F});
sr::drawTriangles(smallColor, smallDepth,
                  vertexShader, effectFragmentShader,
                  {&pointSampler, nullptr}, {nullptr, nullptr},
                  sr::Rect<float>{0, 0, 64, 32}, // viewport in level pixels
                  sr::Rect<float>{0, 0, 1, 1},   // scissor stays normalized
                  sr::BlendState{}, depthState,
                  indices, vertices, modelViewProjection);
// Only level 2 and its descendants changed; levels 0 and 1 are untouched.

// 3. Full-screen triangle in another pass samples the texture at a fractional
//    LOD, interpolating between two adjacent levels with trilinear filtering.
sr::Sampler trilinearSampler;
trilinearSampler.filter     = sr::Sampler::Filter::linear; // bilinear in a level
trilinearSampler.mipFilter  = sr::Sampler::Filter::linear; // blend between levels
trilinearSampler.lod        = 1.5F;                        // 50% level 1 / level 2

sr::Color postFragmentShader(const sr::VertexShaderOutput& input,
                             const std::array<const sr::Sampler*, 2>& samplers,
                             const std::array<const sr::Texture*, 2>& textures)
{
    return textures[0]->sample(samplers[0], input.texCoords[0]);
}
```

If a draw uses its own render target as an input, the shader automatically
reads a snapshot of the whole level chain taken when the draw started, so
fragments never see colors produced earlier in the same draw. Sampling only
requires finite LOD values and UVs in `[0, 1]` with clamp addressing.


# Showcase

The demonstration app is in the demo directory and it can be built for macOS/iOS/tvOS (Xcode project or GNU makefile), Linux/Solaris/BSD (GNU makefile), Windows (Visual Studio project or GNU makefile) and Haiku (GNU makefile). This is a sample output of the renderer (a box with one side transparent and another colored):
![SR sample](https://elviss.lv/files/sr_sample_filtered.png)