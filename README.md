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

Causal (finite light speed) rendering
-------------------------------------

The normal renderer takes a snapshot of the scene at the camera sample time.
The causal mode instead propagates light at a finite speed: along a backward
path a surface at distance `s` is seen at the retarded time `t - s/c`, so the
triangles intersected are the ones that were there when the photons passed,
not the ones present at the observation time.

Enable it on the `Scene` with a finite positive light speed:

```cpp
scene.EnableCausal(/* c = */ 6.0f);   // world-space distance per unit time
scene.Build();
```

Deforming geometry is described per `Mesh`: fill `endPositions` with the same
topology as `positions`. The two arrays are the vertex states at scene times
0 and 1; the mesh interpolates linearly, and an empty `endPositions` means a
static mesh. Every world-space vertex velocity must stay strictly below
`c/4` or the scene is rejected. The camera uses a single observation time in
`[0,1]` (set `camera.shutterStart = camera.shutterEnd = t`); light paths that
would have to start before time 0 are cut off to black.

Restrictions in the first version: triangle meshes only, static reversible
instance transforms, two-sided Lambert materials (`material.color` is the
albedo) and constant two-sided emissive meshes. Spheres, planes, moving
instance transforms, transmission and environment lighting are rejected.
Without `EnableCausal` the Disney BSDF, camera shutter and CPU renderer
behave exactly as before.

The production spacetime query is public in `src/causal.h`
(`CausalTrace`, returning distance, retarded time, original face id and
barycentric coordinates), together with the direct-light sampler
(`CausalSampleLight`) and its reverse density (`CausalLightPdf`).

A self-contained example builds a deforming emissive quad, a receiver and a
wall whose retarded visibility changes over time, and writes `causal.ppm`
through the real CPU renderer:

```
c++ -std=c++17 -O2 -I. src/mesh.cpp src/bvh.cpp src/scene.cpp src/render.cpp \
    src/perlin.cpp src/platform.cpp src/causal.cpp examples/causal_render.cpp -o causal_render
./causal_render <output directory>
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

License
-------

Tinsel is licensed under the ZLib license, see LICENSE.txt.

Author
------

Miles Macklin - http://mmacklin.com
