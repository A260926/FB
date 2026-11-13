# FauxGL

3D software rendering in pure Go. No OpenGL, no C extensions, no nothin'.

<br>

![Dragon](http://i.imgur.com/uwehodr.png)

### About

It's like OpenGL, but it's not. It's FauxGL.

It doesn't use your graphics card, only your CPU. So it's slow and unsuitable for realtime rendering. But it's still pretty fast. It works the same way OpenGL works - rasterizing.

### Features

- STL, OBJ, PLY, 3DS file formats
- triangle rasterization
- vertex and fragment "shaders"
- view volume clipping
- face culling
- alpha blending
- textures
- triangle & line meshes
- depth biasing
- wireframe rendering
- built-in shapes (plane, sphere, cube, cylinder, cone)
- anti-aliasing (via supersampling)
- analytic, sample-free anti-aliasing for orthographic vertex-color previews
- voxel rendering
- parallel processing

### Analytic anti-aliasing

The normal rasterizer resolves each pixel at its center, so two thin,
overlapping triangles that share a pixel can make the silhouette jump whole
pixels. `Context` also offers an exact analytic path for opaque, double-sided,
vertex-colored models under an affine (e.g. orthographic) transform:

- `BeginAnalytic()` snapshots the current color and depth buffers as the base
  image. Every base pixel is treated as one square of constant color and depth
  and must be fully opaque with depth in `[0,1]` or `math.MaxFloat64`.
- `DrawAnalyticTriangles(triangles, matrix)` stages batches. Only each
  triangle's `Position` and `Color` and the finite affine matrix are used; the
  shader, culling, blending and depth bias settings are ignored. Coordinates
  are clipped to the `-1..1` NDC cube and projected through the existing
  `Screen` mapping.
- `ResolveAnalytic()` publishes the result and returns the caller-owned
  float colors; `CancelAnalytic()` discards the staged geometry. Buffers are
  only changed at resolve time.

The path never raises the resolution and never uses sample points. Each pixel
square is split into its exact visible regions: polygon boundaries and
depth-equality lines form a planar arrangement, and the face with the smallest
window-z (later submission wins ties; the base loses ties) is integrated over
each region. Contact along edges or points contributes no area, and the
regions plus the remaining base tile the pixel exactly.

There are three distinct outputs:

1. **Float colors** — the `[]Color` returned by `ResolveAnalytic`, holding the
   area-integrated, affine vertex colors (alpha 1) before quantization. The
   slice is independent of the context.
2. **Display colors** — the same values written to `ColorBuffer` after
   `Color.NRGBA` clamping and 8-bit truncation. No gamma transform is applied.
3. **Depth** — `DepthBuffer` stores the winner at the pixel *center* only; it
   is not an area average and pixels whose center no new face covers keep the
   base depth.

See `examples/analytic` for a side-by-side comparison with center sampling.

### Performance

FauxGL uses all of your CPU cores. But none of your GPU.

Rendering the Stanford Dragon shown above (871306 triangles) at 1920x1080px takes about 150 milliseconds on my machine. With 4x4=16x supersampling, it takes about 950 milliseconds. This is the time to render a frame and does not include loading the mesh from disk.

### Go Get

    go get -u github.com/fogleman/fauxgl

### Go Run

    cd go/src/github.com/fogleman/fauxgl
    go run examples/hello.go

### Go Doc

https://godoc.org/github.com/fogleman/fauxgl

### Complete Example

```go
package main

import (
	. "github.com/fogleman/fauxgl"
	"github.com/nfnt/resize"
)

const (
	scale  = 1    // optional supersampling
	width  = 1920 // output width in pixels
	height = 1080 // output height in pixels
	fovy   = 30   // vertical field of view in degrees
	near   = 1    // near clipping plane
	far    = 10   // far clipping plane
)

var (
	eye    = V(-3, 1, -0.75)               // camera position
	center = V(0, -0.07, 0)                // view center position
	up     = V(0, 1, 0)                    // up vector
	light  = V(-0.75, 1, 0.25).Normalize() // light direction
	color  = HexColor("#468966")           // object color
)

func main() {
	// load a mesh
	mesh, err := LoadOBJ("examples/dragon.obj")
	if err != nil {
		panic(err)
	}

	// fit mesh in a bi-unit cube centered at the origin
	mesh.BiUnitCube()

	// smooth the normals
	mesh.SmoothNormalsThreshold(Radians(30))

	// create a rendering context
	context := NewContext(width*scale, height*scale)
	context.ClearColorBufferWith(HexColor("#FFF8E3"))

	// create transformation matrix and light direction
	aspect := float64(width) / float64(height)
	matrix := LookAt(eye, center, up).Perspective(fovy, aspect, near, far)

	// use builtin phong shader
	shader := NewPhongShader(matrix, light, eye)
	shader.ObjectColor = color
	context.Shader = shader

	// render
	context.DrawMesh(mesh)

	// downsample image for antialiasing
	image := context.Image()
	image = resize.Resize(width, height, image, resize.Bilinear)

	// save image
	SavePNG("out.png", image)
}
```

![Teapot](http://i.imgur.com/DaqbkLR.png)
