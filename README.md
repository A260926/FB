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

### Volume capture, transport and adjoint

FauxGL can render colored emissive media bounded by triangle meshes and
differentiate the image with respect to the mesh vertices and medium
coefficients. Three stages are involved, implemented in
`volume_capture.go`, `volume_transport.go` and `volume_adjoint.go`. The
regular `Shader`/`DrawTriangle`/`DrawMesh` pipeline and buffer behavior
are unchanged; the volume stages only read the context buffers. See
`examples/volume_adjoint` for an end-to-end program.

#### Scene input

```go
type VolumeMesh struct {
	Positions []Vector // shared source vertex positions
	Faces     [][3]int // triangle corners as indices into Positions
}

type VolumeInstance struct {
	Mesh   int    // index into VolumeScene.Meshes
	Matrix Matrix // fixed invertible affine object-to-world matrix
	Medium int    // index into VolumeScene.Media
}

type VolumeMedium struct {
	Sigma Vector // RGB extinction per unit world length, every component >= 0
	J     Vector // RGB emission per unit world length
}

type VolumeScene struct {
	Meshes    []VolumeMesh
	Instances []VolumeInstance
	Media     []VolumeMedium
}
```

Meshes are closed, oriented triangle meshes (non-convex is fine). Outer
shells wind counter-clockwise seen from outside, cavity shells wind the
other way, and same-direction nested shells form one interior through the
non-zero integer winding rule. Closedness and orientation are guaranteed
by the caller and are not validated. Instances may carry non-uniform
scale, shear and mirroring (negative determinant); mirroring flips the
sign of the winding number but the non-zero rule keeps the interior
intact. Where instances overlap, their medium coefficients are summed,
one contribution per active instance, even for equal medium indices.

#### Capture

```go
capture, err := CaptureVolume(ctx, scene, camera)
```

`camera` is a conventional finite-near/far perspective or orthographic
world-to-clip `Matrix`, used with the `Screen` y-down and window-z
conventions. Every transformed vertex must be in front of the eye plane
(`clip.w > 0`); triangles may cross the side, near and far planes.

For every pixel-center ray, the media intervals are derived from real
double-sided triangle rasterization events (no per-pixel ray/scene
intersection). A top-left fill rule makes shared edges and clip-fan
internal edges contribute exactly once. Crossings clipped by the near or
far plane still update the winding state, so intervals truncated by the
near/far planes inside a body are preserved. Each pixel stores its world
ray (`O` on the near plane, unit `D`), the integration limit `TEnd`
(`min(far plane, background depth)` as a world distance along `D`), the
background color `Bg`, the ordered `Intervals`, and every crossing
`Event` sorted by `(T, Inst, Face)`. Each event keeps the original face,
the global source vertex indices, the instance, the barycentric hit
coordinates, the world normal and `D.N` for the adjoint stage.

The context's current color/depth buffers are treated as a fixed opaque
background snapshot: color is byte RGB divided by 255 and must have
alpha 255; depth is a finite value in `[0, 1]` or `MaxFloat64`
(background at the far plane). Global vertex indexing concatenates the
meshes in scene order; `MeshVertexCounts` gives the per-mesh counts.

#### Transport

```go
img, rec := TransportVolume(capture)
```

Integrates the captured intervals along the actual world ray path, from
the near plane to the background depth or far plane (never window-z
differences). Each active constant-coefficient segment of length `l`
accumulates, per channel, `C += T * j * (1 - exp(-sigma*l)) / sigma` and
`T *= exp(-sigma*l)`, where `sigma` and `j` are the sums over the active
instances and `sigma == 0` uses the continuous limit (`j * l`). Finally
`C += T * Bg`. With no active media the background is reproduced exactly
and `T == 1`.

`VolumeImage` holds owned `Color` and `Transmittance` slices (linear
RGB, row-major `y*Width+x`), with no gamma, byte quantization or
negative clamping. The context buffers are not modified.

#### Adjoint

```go
grad, err := AdjointVolume(rec, dColor, dTransmittance)
```

`dColor` and `dTransmittance` are the per-pixel adjoints of the linear
RGB color and RGB transmittance (row-major, `Width*Height` each). The
result is the gradient of `sum(dColor*C + dTransmittance*T)` with
respect to every shared source vertex XYZ and every medium `Sigma`/`J`:

```go
type VolumeGradients struct {
	Positions [][]Vector // per mesh, per vertex XYZ
	Sigma     []Vector   // per medium RGB
	J         []Vector   // per medium RGB
}
```

The camera, instance matrices and background are fixed. Branch decisions
(face coverage, event order, integer winding) are frozen at the recorded
state, but ray/face intersections, world lengths, segment attenuation
and the downstream radiance are differentiated exactly: moving a
boundary changes both adjacent media segments. Boundaries fully outside
the valid depth range have zero vertex gradient, and winding-preserving
crossings inside a body produce no gradient. `sigma == 0` yields the
one-sided continuous derivative and tiny positive sigma is stable.
Geometry derivatives are valid away from coverage switches, tangential
or coincident-distance events; silhouette area derivatives are not
included. Gradients are reduced across faces, instances and pixels.

`VolumeRecord` owns all of its data: the scene, the capture and the
context buffers may be modified or released after `TransportVolume`
returns, frames may be interleaved, and `AdjointVolume` may be called
repeatedly on the same record. The adjoint work is proportional to the
record size; it never re-renders per parameter.

#### Errors

`CaptureVolume` returns a nil capture and a non-nil error for: out-of-range
mesh/face/medium indices, non-finite positions, matrices or coefficients,
singular or non-affine instance matrices, a singular or non-finite camera
matrix, negative `Sigma` components, vertices behind the eye plane, a
color buffer with alpha other than 255 or mismatched dimensions, and
depth values outside `[0, 1]` other than `MaxFloat64`.
`AdjointVolume` returns a non-nil error when the adjoint slices do not
match `Width*Height` or contain non-finite values. `Validate` on the
scene performs the input checks independently.

