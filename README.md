Tinsel
======

A lightweight CPU/GPU path tracer focusing on speed and simplicity. It offers a single flexible BSDF based on Disney's physically principled BRDF extended to specular transmission / subsurface scattering.

Features
--------

- Uni-directional forward path tracer
- BSDF based on Disney's BRDF with specular transmission / thin-shell subsurface scattering
- HDR environment light probes
- Multiple importance sampling of BSDF /  probes / area lights
- CPU or GPU tracing and shading
- Interactive OpenGL progressive mode
- Affine and deformable motion blur
- Gaussian and box reconstruction filters
- Instanced triangle mesh primitives
- AABB tree with SAH and various splitting heuristics
- Batch mode for animation rendering
- Simple scene description .tin format
- Windows / macOS / Linux support
- Wavefront tracing mode
- Non-Local Means noise filtering
- Tungsten scene loader

Images
------

Instancing test (palette courtesy of https://twitter.com/paniq)

![Example Image](images/paniq_brain.png)

Ajax bust in gold (courtesy http://forum.jotero.com)

![Example Image](images/ajax.png)

Ajax bust in plaster (courtesy http://forum.jotero.com)

![Example Image](images/ajax_ss.png)

Spaceship model courtesy of Benedikt Bitterli (https://benedikt-bitterli.me/resources)

![Example Image](images/spaceship.png)

Coffee maker model courtesy of Benedikt Bitterli (https://benedikt-bitterli.me/resources/)

![Example Image](images/coffee.png)

Glass Buddha with specular transmission (courtesy Stanford 3D Scanning Respository http://graphics.stanford.edu/data/3Dscanrep/)

![Example Image](images/glass_buddha.png)

Jade Buddha with sub-surface scattering (courtesy Stanford 3D Scanning Respository http://graphics.stanford.edu/data/3Dscanrep/)

![Example Image](images/jade_buddha.png)

Recreation of the Disney Hyperion test scene, https://www.disneyanimation.com/technology/innovations/hyperion.

![Example Image](images/hyper_small.png)

Recreation of Eric Veach's multiple importance sampling test

![Example Image](images/veach_big.png)

Example Scene
-------------

The scene description in Tinsel is very simple, and loosely based off Arnold's .ass format,
here is an example:

```
# This is a comment

material gold
{
	color 1.0 0.71 0.29
	roughness 0.2
	metallic 1.0	
}

material plaster
{
	color 0.94 0.94 0.94
	roughness 0.5
	specular 0.1
}

material light
{
	emission 5.0 5.0 5.0
}

primitive
{
	type plane
	plane 0 1 0 0
	material plaster
}

primitive
{
	type sphere
	radius 0.5
	material light

	position 0.0 10.0 0.0
	rotation 0.0 0.0 0.0 1.0
	scale 1.0
}

primitive
{
	type mesh
	mesh octopus.obj
	material gold

	position 0.0 0.0 0.0
	rotation 0.0 0.0 0.0 1.0
	scale 2.0
}

```

Command Line
------------

Single Image:

```
tinsel -spp 100 scene.tin output.png
```

To convert an animation sequence, e.g.: frame_1.tin, frame_2.tin, ... use the following syntax:

```
tinsel -spp 100 frame_%d.tin
```


Todo List
---------

- [x] Multiple importance sampling
- [x] Mesh affine transformation support
- [ ] Disc primitive
- [x] Mesh sampling
- [ ] Multi material meshes or .obj conversion
- [x] SAH and median split BVH build heuristics
- [x] Clean up mesh allocations
- [x] Command line interface
- [x] Scene sky parameters
- [x] Scene camera parameters
- [x] Scene include files
- [x] Scene camera definition
- [x] Tone mapping
- [ ] Bloom filter
- [ ] Blackbody emitters
- [x] Output formats
- [x] Triangular noise dither
- [x] NLM noise filter
- [ ] Point sets primitive
- [x] Scene BVH
- [ ] FFmpeg encoding
- [x] Reconstruction filter
- [x] Correctness checks
- [ ] Volume rendering
- [x] Environment maps
- [ ] Lens modelling
- [x] Refraction
- [x] Examples

Supported Platforms
-------------------

Tinsel includes makefiles and Visual Studio projects for OSX and Windows respectively. Although not explicitly supported it should be relatively simple to build for Linux.

Editable Height Fields
----------------------

`src/heightfield.h` / `src/heightfield.cpp` add a height field primitive that is
traced directly as a smooth bilinear surface rather than a triangle mesh.

A field is an `N x N` grid of cells from `(N+1) x (N+1)` row-major finite
heights, where `N` is a power of two at least two. Each cell is a bilinear
Coons patch `y = h(x, z)`. A ray against a cell produces a quadratic in the
ray parameter; every valid root is solved and tested, so saddle cells with
multiple roots are handled exactly. A fixed quadtree of cell AABBs prunes the
ray traversal. No fixed-step marching, single Newton step or triangle proxy is
used.

Call order:

```cpp
// 1. Build the field once from the initial heights.
HeightField field;
field.Initialize(N, W, D, heights.data(), (int)heights.size());

// 2. Pin an immutable generation. The snapshot shares ownership of the data;
//    later edits never mutate it.
HeightField::Snapshot snapshot = field.TakeSnapshot();

// 3. Attach it to a primitive (multiple primitives may share a snapshot).
//    Instances use static rotation, translation and positive uniform scale.
Primitive primitive;
SetHeightField(primitive, snapshot);
primitive.startTransform = primitive.endTransform = Transform(position, rotation, scale);
primitive.material.emission = Vec3(4.0f);
primitive.lightSamples = 2;   // > 0 makes it an explicitly sampled area light

// 4. Add primitives and build as usual; Scene::Build, PrimitiveBounds and the
//    scene BVH now understand the new geometry type.
scene.AddPrimitive(primitive);
scene.Build();
// CpuRenderer path tracing, shadow rays, direct light sampling and the
// balance-heuristic MIS on BSDF-emission hits all handle height fields.

// 5. Edit later. The whole batch is validated first; invalid coordinates or
//    non-finite values leave heights, version and stats untouched. Duplicate
//    coordinates resolve to the last entry. Only the cells touching changed
//    vertices and their quadtree ancestors are recomputed.
hf::EditStats stats;
field.Update(edits.data(), (int)edits.size(), &stats);
// stats.leavesRecomputed / stats.nodesRecomputed expose the actual local work.

// 6. New primitives (or re-built scenes) see the new generation; primitives
//    holding older snapshots keep the old heights, bounds, normals and pdfs.
```

Light sampling draws `(u, v)` uniformly over the whole local xz rectangle, not
by triangle or surface area. The matching pointwise densities are
`Snapshot::LocalAreaPdf`, `Snapshot::WorldAreaPdf` (divided by the instance
scale squared) and `HeightFieldDirectionPdf` (world area density times the
true connection distance squared over the absolute cosine). The light emits
from both sides; an exact zero-cosine connection contributes zero. Edges use
the cell with the smaller row then smaller column, and the outer border uses
the single adjacent cell.

The example `examples/heightfield_render.cpp` renders a field before an edit,
after lifting a vertex beyond the old bounds, and through a snapshot retained
from before the edit (`before.ppm`, `after.ppm`, `retained.ppm`).

License
-------

Tinsel is licensed under the ZLib license, see LICENSE.txt.

Author
------

Miles Macklin - http://mmacklin.com
