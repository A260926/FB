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
- Complete state query API (glGet*)

## Per-vertex materials and lighting

`glMaterial` (and `GL_COLOR_MATERIAL`) changes only affect vertices submitted
after the change. Each `glVertex` snapshots the current front and back
material (ambient, diffuse, specular, emission, shininess), so a material
change made between the last vertex and `glEnd` can never be applied
retroactively to the whole primitive. This works identically through
immediate mode, `glDrawArrays`/`glDrawElements`, and display list playback.

```c
/* Each vertex keeps its own emission: the triangle interpolates red->green->blue */
glEnable(GL_LIGHTING);
glBegin(GL_TRIANGLES);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, red);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, green);
    glVertex3f( 0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, blue);
    glVertex3f( 0.0f,  0.8f, 0.0f);
glEnd();
```

Shading rules for filled `GL_TRIANGLES`, `GL_TRIANGLE_STRIP` and
`GL_TRIANGLE_FAN`:

- **GL_SMOOTH (Gouraud):** front and back lighting are both evaluated at the
  original vertices with that vertex's own materials, then interpolated with
  the existing screen-affine color rule. Back faces are never silently
  re-lit per fragment.
- **GL_FLAT:** the whole primitive takes the lit color (front or back as
  appropriate) of its last submitted, i.e. provoking, vertex. The source
  survives frustum clipping, including when that vertex is itself clipped
  away or the clipped polygon is split into fan pieces.
- **GL_PHONG:** material coefficients, eye-space position and normal are
  perspective-correct interpolated (screen barycentrics × each vertex's
  1/w, renormalized), then the same lighting routine runs per fragment.
  Whether the interpolated normal is renormalized still follows
  `GL_NORMALIZE`.

With `GL_LIGHT_MODEL_TWO_SIDE` disabled, both faces use the front material
and normal. Enabled, back faces flip the normal and use the back material,
while `glFrontFace` keeps deciding which side is front. Clip-generated
vertices inherit all interpolated attributes from the clipping parameter and
never re-read the current material state.

`glColorMaterial` still tracks the selected face and property, and
`glGetMaterial` continues to report the current tracked state:

```c
glEnable(GL_COLOR_MATERIAL);
glColorMaterial(GL_FRONT, GL_DIFFUSE);
glColor3f(1.0f, 0.0f, 0.0f);   /* front diffuse of the next vertex becomes red */
glVertex3f(0.0f, 0.0f, 0.0f);
```

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

## Regression tests

`task_tests/materials.c` exercises the per-vertex material pipeline against
an independent reference implementation (its own vector, lighting,
barycentric and clipping math), covering immediate mode, vertex arrays and
display lists across capture, lighting and clipping scenarios:

```bash
bash task_support/run.sh baseline   # current production rendering smoke test
bash task_support/run.sh target     # builds and runs task_tests/materials.c
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
