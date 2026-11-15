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

### Terrain (quadtree LOD height fields)

A `Terrain` owns a finite, square height array of size `2^k+1` (k >= 3) with
unit sample spacing and local positions `(x, height, z)`. Many
`TerrainInstance` placements can share one `Terrain`; each frame you prepare a
view-dependent `TerrainPlan` (quadtree selection, 2:1 seam closure, frustum
culling) and draw it through the indexed raster path - no per-frame expansion
into independent triangles.

Call order:

```go
// 1. build the shared resource from your own finite height samples
terrain, err := fauxgl.NewTerrain(size, heights) // size = 2^k+1, k >= 3

// 2. place it any number of times; world = Origin + Scale*local
instances := []fauxgl.TerrainInstance{
    {Terrain: terrain, Origin: origin, Scale: scale, Visible: true},
}

// 3. select leaves for the camera (k-3 non-increasing split distances)
matrix := fauxgl.Perspective(fovy, aspect, near, far).Mul(fauxgl.LookAt(eye, center, up))
plan, err := fauxgl.PrepareTerrain(instances, fauxgl.TerrainView{
    Eye: eye, Matrix: matrix, SplitDistances: splits,
})
for _, leaf := range plan.Leaves() { // independent leaf descriptions
    // leaf.Instance, X, Z, Span, Depth, Stitch (bits west/east/south/north)
}

// 4. draw with any shader whose Vertex only applies view.Matrix
context.Shader = fauxgl.NewPhongShader(matrix, lightDirection, eye)
info, err := fauxgl.DrawTerrain(plan, context)
_ = plan.DrawStats() // Vertices / Triangles actually submitted

// 5. between frames, edit the shared source data in validated batches
err = terrain.UpdateHeights([]fauxgl.HeightEdit{{X: x, Z: z, Height: h}})
// old plans now fail DrawTerrain; re-run PrepareTerrain to see the new heights
_ = terrain.Revision()       // bumps once per effective non-empty batch
_ = terrain.LastUpdateStats() // HeightReads / NormalUpdates / BoundsUpdates
```

Selection rules: a node subdivides while the eye's shortest Euclidean distance
to its world AABB is strictly less than the depth's split distance; closure
then only subdivides further so neighboring leaves differ by at most 2x in
grid spacing; finally a block is culled only when one clip half-space excludes
all eight AABB corners. Mixed LODs are stitched with shared index templates
(the odd boundary vertices of a fine block collapse onto the previous even
vertex; zero-area faces are skipped) - no skirts, and topology is shared
resource-wide instead of copied per block.

`Context` also exposes the underlying indexed entry point directly:

```go
info, err := context.DrawIndexed(vertices, indices) // [][3]int into one vertex pool
```

Indices and all vertex attributes are fully validated before any shader call
or buffer write; an empty index slice draws nothing, unreferenced vertices
never reach `Shader.Vertex`, and each referenced vertex is shaded exactly once
per call. The clipping, perspective interpolation, depth and fragment chain is
the same one `DrawMesh` uses.

A runnable example rendering two LODs and an edited field to PNG lives in
[`examples/terrain/main.go`](examples/terrain/main.go):

    go run examples/terrain/main.go terrain.png

