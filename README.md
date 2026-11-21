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

Height Fields
-------------

A height field is an editable bilinear surface traced directly as a curved
patch - it is never tessellated into triangles. Construction takes `N*N`
cells from `(N+1)*(N+1)` row-major finite heights, where `N` is a power of
two >= 2, over a local rectangle of positive finite width `W` and depth `D`.
Each cell interpolates its four corners as `y = h(x,z)`; the front-facing
geometric normal is `(-dh/dx, 1, -dh/dz)`.

Call order:

```cpp
// 1. construct the editable field from row-major heights
HeightField field(N, W, D, heights);

// 2. capture an immutable, self-owning snapshot
HeightFieldSnapshot snapshot = field.Snapshot();

// 3. attach the snapshot to a primitive (many primitives may share one)
Primitive primitive;
primitive.type = eHeightField;
primitive.heightfield.snapshot = std::make_shared<HeightFieldSnapshot>(snapshot);
primitive.startTransform = primitive.endTransform =
    Transform(position, rotationQuat, uniformPositiveScale); // static instances only

// 4. the scene BVH, bounds and intersection paths pick it up automatically
scene.AddPrimitive(primitive);
scene.Build();
Ray ray(origin, dir, time);
float t; Vec3 normal;
PrimitiveIntersect(scene.primitives[0], ray, t, &normal);
```

Continuous editing uses batch vertex updates over a fixed quadtree:

```cpp
HeightEdit edits[] = { HeightEdit(x, z, newHeight), /* ... */ };
HeightUpdateStats stats;
if (!field.Update(edits, numEdits, stats))   // whole batch validated first
    ;                                         // nothing changed on failure
// stats.leavesRecomputed / stats.nodesRecomputed report the real local work;
// an empty or net-unchanged batch creates no version and reports zero work

HeightFieldSnapshot next = field.Snapshot(); // new primitives see the new version
```

Rules worth knowing:

- A batch is validated before any change; repeated coordinates resolve to the
  last entry. On failure the heights, tree, version and work stats are untouched.
- A real change refreshes only the leaf cells touching edited vertices and
  their quadtree ancestors. For a 257x257 vertex field a single interior edit
  recomputes four leaves; no full-field scan or full rebuild is performed.
- Snapshots keep their own version alive through shared ownership. Old scenes
  and old primitives keep the old heights, bounds, normals and densities;
  releasing a snapshot frees its unreferenced history - full height arrays and
  whole trees are never copied per edit.
- Intersection solves the per-cell quadratic in double precision, examines
  every valid root and returns the nearest positive one (rays need not be unit
  length). Shared grid edges never miss; edge derivatives use the cell with the
  smaller row, then smaller column, with the outer boundary using its unique
  cell.
- Light sampling is uniform over the local `(u,v)` parameter rectangle, not
  over triangle area. `Sample`/`Inverse` share that convention and the local
  area density is `1/(W*D*sqrt(1+hx*hx+hz*hz))`; the world density divides by
  the squared instance scale and the solid-angle density multiplies by the
  squared connection distance and divides by the absolute cosine. Direct light
  sampling and BSDF-continuation balance MIS both use this pointwise density
  with the primitive's existing `lightSamples` count. Height field lights emit
  from both sides; an exactly zero-cosine connection contributes nothing.

Tests and example:

```
bash task_support/run.sh target           # builds everything, runs the three
                                          # test groups, the render example and
                                          # the original production baseline
./.task-build/example <output directory>  # writes before.ppm, after.ppm,
                                          # retained.ppm via the CpuRenderer
```

`tests/heightfield_target.cpp` registers `intersections`, `updates` and
`lighting` groups through `task_support/check.h`; `examples/heightfield_render.cpp`
renders the field before an edit, after an edit that lifts a vertex outside
the original bounds, and a retained primitive still pinned to the old snapshot.


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

License
-------

Tinsel is licensed under the ZLib license, see LICENSE.txt.

Author
------

Miles Macklin - http://mmacklin.com
