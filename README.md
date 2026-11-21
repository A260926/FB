# MyTinyGL

A lightweight software OpenGL 1.x renderer. No GPU required.

![Suzanne rendered with MyTinyGL](example.png)

## About the Development

I want to be transparent: this project was developed with the assistance of Claude (Anthropic's AI). I'm sorry if this wasn't clear from the start.

**What I'm doing about it:**
- I'm reviewing every single line of code myself
- The entire codebase passes a comprehensive test suite
- I'm committed to understanding every algorithm and design decision
- I will deliver a good and trustable contribution to the community

AI helped accelerate development, but the responsibility for this code is mine. I'm actively working to ensure this project meets the standards the community deserves.

## What is it?

MyTinyGL is a complete software implementation of the OpenGL 1.x fixed-function pipeline. It renders 3D graphics entirely on the CPU, producing pixel-perfect output without any GPU or hardware acceleration.

**~8,000 lines of clean C99. No dependencies.**

## Size Comparison

| Implementation | Lines of Code | Notes |
|----------------|---------------|-------|
| **MyTinyGL** | ~8,000 | Complete OpenGL 1.5 API |
| TinyGL | ~20,000 | Subset only, buggy, abandoned |
| Mesa (software) | 500,000+ | Full OpenGL, massive |

## Features

- Full OpenGL 1.5 immediate mode API
- Texturing with filtering and mipmaps
- Lighting (8 lights, Gouraud and Phong shading)
- Fog, blending, alpha test, depth test, stencil test
- Display lists, VBOs, vertex arrays
- Frustum clipping, perspective-correct interpolation
- GL_SELECT selection mode for off-screen picking
- Complete state query API (glGet*)

## Selection (GL_SELECT)

MyTinyGL implements OpenGL 1.x selection mode for picking geometry by name,
fully off-screen (no SDL, no display, no GPU):

- API: `glSelectBuffer`, `glRenderMode` (`GL_RENDER`/`GL_SELECT` only),
  `glInitNames`, `glPushName`, `glPopName`, `glLoadName`.
  `GL_FEEDBACK` is not supported.
- Works through immediate mode, `glDrawArrays`/`glDrawElements`, and display
  lists — they all share the same transform/clip chain.
- Supported primitives in selection mode: `GL_POINTS`, `GL_LINES`,
  `GL_LINE_STRIP`, `GL_LINE_LOOP`, `GL_TRIANGLES`, `GL_TRIANGLE_STRIP`,
  `GL_TRIANGLE_FAN`. Anything else raises `GL_INVALID_ENUM` and preserves the
  current mode and accumulated results.
- Hits are decided geometrically, never by color-key rendering or per-pixel
  depth readback: vertices are transformed by the current modelview/projection
  matrices and clipped against the six frustum planes (post-clip `w > 0`).
  A point inside the frustum, a line with a non-zero clipped segment, or a
  triangle with a positive-area clipped remnant counts as a hit — even if it
  would not cover any pixel center. A triangle clipped into a fan still counts
  as one hit.
- Triangles still honor `glFrontFace`/`glCullFace`. Depth test, stencil, alpha
  test, scissor, blending and color masks do not affect hits, and selection
  never modifies the color, depth or stencil buffers.
- Record depths are the min/max of the clipped geometry's `z/w` mapped through
  `glDepthRange` (reversed ranges included), clamped to `[0,1]` and converted
  with `floor(z * 4294967295 + 0.5)`.
- Hits under one name stack merge into a single record
  `[name count, min depth, max depth, names bottom-to-top]`; the record is
  settled when the name stack changes or when leaving selection mode
  (`glEnd` does not settle). An empty name stack produces records too.
- Records are written atomically: exactly filling the buffer is legal, but if a
  record does not fit, nothing more is written, the buffer locks, and
  `glRenderMode(GL_RENDER)` returns `-1`. Re-entering selection starts from
  empty results; the buffer binding persists until explicitly replaced
  (zero capacity is allowed and overflows on the first record).
- Name stack depth: 64. Failed `glPushName`/`glPopName`/`glLoadName`
  (`GL_STACK_OVERFLOW`/`GL_STACK_UNDERFLOW`/`GL_INVALID_OPERATION`) neither
  settle records nor change the stack. Name ops are no-ops outside selection
  mode but are still recorded into display lists; `glSelectBuffer` and
  `glRenderMode` are always immediate and never recorded.

## Building

```bash
make
```

That's it. Pure C99, no dependencies.

For the testbed examples (optional):
```bash
sudo apt install libsdl2-dev
cd testbed && make
```

## Usage

```c
#include <mytinygl/sdl.h>

int main() {
    mtgl_init("My App", 800, 600);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_DEPTH_TEST);

    while (running) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // Draw your scene...
        mtgl_swap();
    }

    mtgl_destroy();
    return 0;
}
```

Link with: `-lMyTinyGL -lSDL2 -lm`

## License

MIT
