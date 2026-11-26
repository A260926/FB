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

# Mip levels and render targets

A `Texture` can allocate a mip chain (`mipMaps = true`), and `getWidth(level)` /
`getHeight(level)` report the size of a level. `setData`, `getPixel` and sampling
all address the selected level using its own dimensions. Writing to a level
through `setData`, `clear` or `drawTriangles` rebuilds its successor levels;
ancestors are left alone. Editing bytes via `getData` requires an explicit
`generateMipMaps(startLevel)` call.

`TextureView(Texture&, level)` selects a level for drawing without owning the
texture; it is resolved against the owner's current dimensions on every call.
Color targets must be `rgba8`, depth targets `float32`, and both views must
select levels of equal size.

```cpp
sr::Texture scene(sr::PixelFormat::rgba8, 512, 512, true);
sr::Texture sceneDepth(sr::PixelFormat::float32, 512, 512, true);

// Render the scene into selected mip levels...
sr::clear(sr::TextureView{scene, 3}, sr::Color{0.0F, 0.0F, 0.0F, 1.0F});
sr::clear(sr::TextureView{sceneDepth, 3}, 1.0F);
sr::drawTriangles(sr::TextureView{scene, 3},
                  sr::TextureView{sceneDepth, 3},
                  vertexShader, fragmentShader,
                  {&sceneSampler, nullptr}, {&sceneTexture, nullptr},
                  sr::Rect<float>{0.0F, 0.0F, 64.0F, 64.0F}, // selected level pixels
                  sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},   // normalized scissor
                  blendState, depthState, indices, vertices, mvp);

// ...then a full-screen post-process triangle samples that level by LOD.
sr::Sampler postSampler;
postSampler.filter = sr::Sampler::Filter::linear;
postSampler.mipFilter = sr::Sampler::Filter::linear;
postSampler.lod = 3.0F; // read the level rendered above

sr::Texture output(sr::PixelFormat::rgba8, 512, 512, false);
sr::Texture outputDepth(sr::PixelFormat::float32, 512, 512, false);
sr::drawTriangles(output, outputDepth,
                  vertexShader, postFragmentShader,
                  {&postSampler, nullptr}, {&scene, nullptr},
                  sr::Rect<float>{0.0F, 0.0F, 512.0F, 512.0F},
                  sr::Rect<float>{0.0F, 0.0F, 1.0F, 1.0F},
                  blendState, depthState, indices, vertices, mvp);
```

`drawTriangles` and `clear` still accept plain `Texture&` arguments, which
select level 0.

# Showcase

The demonstration app is in the demo directory and it can be built for macOS/iOS/tvOS (Xcode project or GNU makefile), Linux/Solaris/BSD (GNU makefile), Windows (Visual Studio project or GNU makefile) and Haiku (GNU makefile). This is a sample output of the renderer (a box with one side transparent and another colored):
![SR sample](https://elviss.lv/files/sr_sample_filtered.png)