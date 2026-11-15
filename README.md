# grayt

Go RAY Tracer

## Features

- [X] Path tracing via rendering equation simulation (Monte Carlo method).
- [X] Diffuse reflections (matte surfaces).
- [X] Specular reflections (mirror surfaces).
- [ ] Light transmission (transparent surfaces).
- [X] Depth of field effects.
- [X] Multithreading support.
- [X] Fast acceleration structure.
- [X] Web UI.
- [ ] Persistent storage of partial renders.

## TODO

- Ability to delete renders.
- Use fixed space font for data in UI.
- Default scene display ratio.
- Load passes statistics from accumulator.
- Calculate resolutions in backend.
- Allow to downsample resolution.
- Allow to choose exposure level.
- Try different lambda values for grid.
- Bounding Volume Hierarchy
- Use pointer to material instead of copying in each object.

## Live scene updates

A running `trace.Instance` can switch scenes without restarting. The call
order is:

1. Create the instance with `trace.NewInstance(dim, sceneFn, accumFile)` and
   call `SetWorkers` to start rendering the startup scene. The startup scene
   is generation 0.
2. Submit a `scene.LiveScene` with `inst.UpdateScene(live)`. The scene is
   validated and fully compiled before anything is published; on success the
   call returns the new generation number (one higher than the previously
   published generation) and the instance has already switched to it. On
   error nothing changes: the current generation, camera, primitives,
   accumulated image and worker settings are untouched.
3. Each `scene.Prototype` is compiled once per submission. Every visible
   `scene.Placement` shares those compiled primitives under its own row-major
   3x4 affine `Transform` (the identity matrix must be written out
   explicitly); hidden placements are never intersected. Placements can be
   moved, rescaled (including non-uniformly) or hidden between submissions.
4. Publishing resets the accumulated image and counters to zero. Passes that
   belong to a retired generation are finished off but never merged into the
   new generation.
5. `inst.GetStats().Generation` reports the current generation, and `Image`
   and `GetStats` always reflect a single generation. Wait for
   `GetStats().Passes >= 1` on the new generation before taking a picture.

See `examples/live_scene` for a complete program: it starts an instance,
commits two placements of one prototype, commits a second scene that moves
one copy and hides the other, waits for the new generation to finish a pass,
and writes the result to a PNG:

    go run ./examples/live_scene out.png

## Gallery

### Split Box

![Split Box](/gallery/splitbox[KQRdZO3e8KI]_1024x1024_q100000.png)

### Sphere Tree

![Sphere Tree](/gallery/sphere_tree[T35GCh3Lpj4]_1024x512_q100000.png)

### Cornell Box

![Cornell Box](/gallery/out_q100000.png)

## Scene Ideas

- 3D patchwork: https://mattdesl.svbtle.com/pen-plotter-2
