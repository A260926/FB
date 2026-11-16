# Offline FBX → glTF 2.0 (GLB) export

`src/ofbx_gltf.h` / `src/ofbx_gltf.cpp` add an offline, frame-rate-independent
exporter that turns a single `ofbx::AnimationLayer` over a time interval into a
self-contained binary glTF (GLB) document built exclusively from plain
translation / rotation / scale node animation — no matrices, no extensions — so
players that only understand glTF TRS animation reproduce the full FBX local
transform semantics, including rotation/scaling pivots, offsets,
Pre/PostRotation, all six euler orders and multi-turn rotations.

## Calling the exporter

```cpp
#include "src/ofbx_gltf.h"

ofbx::IScene* scene = ofbx::load(data, size, /*flags*/ 0);
const ofbx::AnimationLayer* layer =
    scene->getAnimationStack(0)->getLayer(0);

ofbx::GltfResult result;
ofbx::GltfError err = ofbx::exportGltf(
    *scene, *layer,
    /*begin seconds*/ 0.0,
    /*end seconds*/   1.0,
    result);

if (err != ofbx::GltfError::OK) {
    // result.code / result.message describe the locateable failure.
    // A previous successful result.glb / result.node_map is left intact.
} else {
    // result.glb            -> complete GLB byte vector
    // result.node_map       -> source Object::id -> terminal output node
}
```

* `begin`/`end` are source-time seconds; both must be finite, non-negative,
  representable as finite `float`, and `begin < end`. Output animation time is
  rebased to start at zero (`out_t = source_t - begin`).
* On failure `exportGltf` returns a `GltfError`, fills `result.code` and a
  human-readable `result.message`, never overwrites the previous successful
  `glb`/`node_map`, and never mutates the source scene.
* `result.node_map[id]` points at the **terminal** node of the chain generated
  for source object `id`, i.e. the node whose world matrix equals the source
  object's complete animated world transform. Source children and skin joints
  all attach to / reference these terminal nodes.

## Node chain and spatial semantics

For every animated/static FBX node the full local formula

```
T(t) T(RotationOffset) T(RotationPivot)
    R(PreRotation, XYZ) R(euler, order) R(-PostRotation, ZYX)
    T(-RotationPivot)
    T(ScalingOffset) T(ScalingPivot) S(s) T(-ScalingPivot)
```

is lowered into a parent→child chain of single-purpose TRS nodes:

* three independent translation axis nodes,
* static translation nodes for rotation offset/pivot and scaling offset/pivot,
* static rotation nodes for PreRotation and the inverse PostRotation,
* **three independent euler axis rotation nodes**, ordered so parent→child
  multiplication reproduces the FBX euler order exactly (the three euler axes
  are never merged into one quaternion curve),
* three independent scale axis nodes and the pivot-inverse translations.

The factor definition is shared with the loader: `ofbx::getLocalTransformFactors`
and `ofbx::composeLocalTransform` (declared in `ofbx.h`) are the single source
of the pivot formula, and `Object::evalLocal` is now a thin wrapper over them,
so the production path and the exporter cannot drift apart.

### Animation rules

* Only LINEAR curves are emitted; no frame-rate baking takes place.
* Each euler/translation/scale axis keeps its own key times; channels are fully
  desynchronized and parent/child bones never share an assumed time table.
* The interval endpoints are always emitted; adjacent output keys must remain
  distinct float seconds or the export fails.
* An axis without a curve uses its curve-node default; an object without a
  curve node uses its static TRS.
* A rotation segment spanning 180° or more (e.g. 450°, negative turns, several
  full loops) is recursively split at midpoints into finite sub-segments, so
  glTF shortest-arc SLERP keeps the original turning direction instead of
  snapping to the short way.

### Meshes and skins

* Triangles, `POSITION`, and linear skinning with at most four positive
  influences are supported. Materials, UVs, normals, morph targets and dual
  quaternion skinning are not exported; any blend shape is rejected.
* Each mesh's geometric transform lives on its own child node affecting only
  that mesh.
* Within one skin, repeated clusters for the same bone are merged only when
  their `Transform` **and** `TransformLink` matrices are identical; otherwise
  the export fails with `INVALID_SKIN`. Per-vertex positive weights are
  normalized; a fifth distinct positive influence is an error, never silently
  truncated. A vertex with no positive weight gets a synthetic identity joint
  that follows the mesh terminal's complete world pose.
* The per-cluster inverse-bind matrix is

  ```
  inverse(TransformLink) * Transform * geometric
  ```

  (`Transform` excludes the geometric transform by convention). A skinned
  world point is therefore the weighted sum of

  ```
  currentBoneWorld * inverse(TransformLink) * Transform * geometric * controlPoint
  ```

  and no extra mesh-world transform is applied to an already skinned point.
  Two meshes may share bones while keeping distinct transforms, links and
  geometric matrices; they are never merged.

## GLB layout

The output follows glTF 2.0 node / animation / skin / buffer conventions with
no extensions: 4-byte aligned JSON and BIN chunks (MAT4 views are 16-byte
aligned), correct total length, and valid `min`/`max` on POSITION and animation
time accessors. Axes and units are passed through unchanged.

## Verification

`bash task_support/run.sh target` builds the library, the example and the test
target, regenerates `.task-build/example.glb`, parses it back with the bundled
`vendor/cgltf`, validates and samples it, then runs the `factor` / `motion` /
`skin` test groups and the existing FBX load/transform baseline. Nothing is
downloaded and no window or GPU is used.
