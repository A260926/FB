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
- voxel rendering
- parallel processing

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

### Off-screen volumetric capture, transport and adjoint

For colored, emissive participating media on closed, oriented triangle
meshes, the package adds a self-contained volume pipeline. It reuses the
existing `Matrix`, Sutherland–Hodgman triangle clipping and edge-function
raster coverage primitives, never touches the ordinary `Shader` /
`DrawTriangle` / `DrawMesh` / color and depth buffer behavior, and needs
no GPU, display or downloaded assets. The three production files are
`volume_capture.go`, `volume_transport.go` and `volume_adjoint.go`.

#### Inputs and conventions

- `VolumeScene` holds `Meshes` (closed, oriented triangle meshes with
  shared position indices), `Instances` referencing those meshes, per
  instance invertible affine `Matrix` plus a `Media` index, and one
  `VolumeMedia` per medium with non-negative per-channel RGB extinction
  `Sigma` and per-world-unit emission `J`. Non-uniform scale, shear and
  mirroring are supported.
- Outer shells point outward; cavity shells point inward. Same-oriented
  nested shells combine by nonzero integer winding number into one
  instance interior. Overlapping instances add their coefficients, even
  when they reference the same medium. Meshes may be non-convex.
- `ViewMatrix` is an ordinary finite near/far perspective or
  orthographic world-to-clip matrix, following the existing `Screen`
  y-down and window-z conventions. Transformed vertices are guaranteed to
  lie in front of the eye plane with `clip.w > 0`; triangles may cross
  the side, near and far planes.
- Closed oriented topology is the caller's responsibility. Everything
  else is validated up front and returned as an error: bad indices,
  non-finite values, singular view/instance matrices, negative
  coefficients, and frame size / context size mismatches.

#### Capture — `CaptureVolume(dc, scene) (*VolumeRecord, error)`

Every instance triangle is rasterized double-sided with the production
edge-function coverage rule after side-plane clipping. For each covered
pixel center, the true world ray is intersected with the original
triangle (Möller–Trumbore, unit direction so the parameter is a real
world distance), keeping the original face, source barycentric weights
and instance identity. There is no per-pixel scan of all meshes. Shared
edges and clipping fans contribute once; mirrored instances (negative
affine determinant) have their winding flipped so the solid interior
cannot invert.

Near/far are handled by world distance rather than by clipping faces
away: crossings behind the near plane establish each instance's opening
winding, so an interval cut by the near or far plane is retained.

The current `Context` color and depth buffers are read once as a fixed
backdrop snapshot and are never modified. Color is interpreted as byte
RGB divided by 255 and requires alpha `255`; depth must be a finite value
in `[0,1]` or `math.MaxFloat64` (background at the far plane). Window-z
is unprojected exactly to a world distance; window-z differences are
never used as lengths.

A `VolumeRecord` owns private copies of all geometry (shared source
vertices per mesh, per-instance world vertices and matrices), rays,
events and the backdrop. Mutating the source meshes, scene or context
after capture, interleaving two frames, or repeating an adjoint cannot
leak state between records.

#### Transport — `SolveVolume(record) (*VolumeResult, error)`

Each ray is integrated from the near plane to the backdrop depth or the
far plane, whichever is nearer. The winding events are swept into
constant-coefficient segments; for a segment of world length `l`, the
per-channel coefficients `sigma` and `j` are the sums over all active
instances, and

```
C += T * j * (1-exp(-sigma*l))/sigma
T  *= exp(-sigma*l)
```

with the `sigma -> 0` continuous limit `(1-exp(-sigma*l))/sigma -> l`
(and `T` unchanged). The final color adds `T * backdrop`. The result is
owning linear-RGB `Color` and per-channel `Transmission`, with no gamma,
byte quantization or negative clamping. With no active medium a pixel
reproduces the backdrop exactly and has transmittance exactly one.

#### Reverse interface — `AdjointVolume(record, colorAdj, transAdj) (*VolumeGradients, error)`

Given per-pixel RGB and transmittance adjoints (same row-major layout),
it returns the gradient of their weighted sum with respect to every
shared source vertex XYZ (per mesh) and every medium `Sigma` and `J`.
Camera, instance matrices and the backdrop are fixed. Face coverage,
event ordering and integer winding branches are held fixed, while the
real ray/original-face intersections, world segment lengths, per-segment
attenuation and downstream radiance are all differentiated analytically.
Moving a boundary differentiates both adjacent medium segments jointly;
barycentric weights and intersection distances are not frozen, and
per-instance alpha is never differentiated separately.

Boundaries fully outside the active depth range contribute zero vertex
gradient, and same-coefficient interior shells produce no false
gradient. Zero extinction uses its one-sided continuous derivative and
tiny positive extinctions use Taylor branches for stability. Shared
vertices are reduced across faces and instances, and medium parameters
across every place they are used. Cost grows linearly with record size:
no redraws, parameter perturbations or automatic-differentiation
dependencies.

#### Array layout and error conventions

- Pixel buffers are row-major, y-down, flat: index `y*Width + x`, length
  `Width*Height`. `Color` fields are ordered `R, G, B, A`; the volume
  pipeline uses the first three channels and sets `A = 1`.
- `VolumeGradients.Vertex[mesh][vertex]` is one `Vector` XYZ gradient per
  shared source vertex; `Sigma` and `J` are per-medium `Color` gradients.
- All entry points return Go `error` values rather than panicking for
  bad input; `nil` records and adjoint/solve buffer size mismatches are
  rejected. On success the returned slices/records are owned by the
  caller and do not alias the source scene or context.

#### Example

`go run ./examples/volume_adjoint <output-dir>` renders
`beauty.ppm` and writes `gradients.csv` (rows
`kind,mesh,index,component,value` for `vertex`/`sigma`/`j` gradients)
through the real production entry points, with both positive and
negative geometry and medium gradients.

