# Specular path replay with analytic adjoints

`src/integrators/replay.{h,cpp}` adds a differentiable mirror/matte path
replay to the existing pbrt-v2 intersection code.  It does **not** introduce a
second intersection engine: every forward ray goes through
`Scene::Intersect` / `Scene::IntersectP`, so the existing BVH,
`TriangleMesh` refinement and nested `TransformedPrimitive` instances are
reused unchanged.  The reverse pass is a hand-written adjoint — no automatic
differentiation library, no parameter perturbation and no re-tracing.

## Supported scene subset

Everything outside this subset is rejected explicitly with an error string
(no silent fallback):

- `TriangleMesh` shapes only, without alpha masks, explicit normals or
  tangents,
- `GeometricPrimitive` leaves and arbitrarily nested static
  `TransformedPrimitive` instances whose local-to-parent transform is a
  finite invertible affine,
- constant-Kr `MirrorMaterial` without a bump map,
- constant-Kd, `sigma == 0` `MatteMaterial` without a bump map,
- `PointLight` lights only; the background is black,
- no volume regions or area lights.

## Differentiated parameters

| Parameter | Binding call | Gradient layout |
|-----------|--------------|-----------------|
| source mesh constructor `P` array | `ReplayBindMesh(mesh, id)` | xyz triple per vertex |
| instance local-to-parent 3×4 affine, row major | `ReplayBindInstance(instance, id)` | 12 floats |

The fixed `ObjectToWorld` of a source shape is treated as constant and is
never differentiated.  Several meshes or instances may share one parameter
ID; their contributions accumulate into a single gradient block.  Unbound
geometry still renders but gets no gradient slot.

## Forward model

For each ray, at most `maxBounces` mirror reflections are followed.  A matte
hit terminates the path and accumulates, per point light,

```
RGB = (product of mirror Kr along the path) * Kd/pi * I * vis
      * max(0, N . L) / |lightPos - p|^2
```

using the real `Scene::IntersectP` for the visibility term.  Missing all
geometry or exhausting the bounce budget returns black.  No random sampling,
tonemapping or clamping is applied; the output is linear RGB.

Geometric values participating in the adjoint are the hit `t`, barycentric
coordinates, world hit position, the winding face normal (including the
negative-determinant instance handedness flip and `reverseOrientation`),
the normalized ideal reflection direction and the point-light distance,
cosine and shadow terms.  Ray epsilon and all branch decisions (face
selection, occlusion, bounce count) are held fixed in the adjoint.

## Reverse model

`ReplayBackward(record, seed)` differentiates
`sum_r (seed_r . RGB_r)`.  Each call starts from zero, so repeated calls with
different seeds are independent.  The record owns every value it needs
(vertices, barycentrics, per-level instance points, normals, transforms,
light terms) and stays valid after the `Scene` and all its objects are
destroyed or repositioned.

## Minimal usage

```cpp
ReplayBindMesh(mesh, /*id*/ 1);
ReplayBindInstance(instance, /*id*/ 2);

ReplayRecord *record = NULL;
string err;
if (!ReplayTrace(scene, rays, /*maxBounces*/ 4, &record, &err)) { /* err */ }

const ReplayColor *rgb = ReplayColors(record);

vector<ReplayColor> seed(rays.size());
for (size_t i = 0; i < rays.size(); ++i)
    seed[i].rgb[0] = seed[i].rgb[1] = seed[i].rgb[2] = 1.f;
ReplayGradient *grad = ReplayBackward(record, seed);
const ReplayGradientBlock *meshBlock = grad->Find(1);
const ReplayGradientBlock *matrixBlock = grad->Find(2);

ReplayFreeGradient(grad);
ReplayReleaseRecord(record);
```

`examples/replay_fit.cpp` is a windowless end-to-end example that fits an
instance translation by gradient descent and writes `fit.ppm`, `loss.txt`
and `parameters.txt`.

## Tests and build

- `task_tests/replay.cpp` registers `primal`, `adjoint` and `lifetime`
  groups driven by `task_support/driver.c`.  Primal colors are checked
  against an independent path built from the real pbrt BSDFs; adjoint
  components are checked against two central-difference step sizes.
- `bash task_support/run.sh target` builds the production library, runs all
  regression groups, builds and runs the fit example, then runs the original
  baseline.
