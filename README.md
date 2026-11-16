# Per-instance setup pose overrides

Two armatures can be built from the same `DragonBonesData` (shared `BoneData`,
animation timelines, packed mesh arrays and FFD keys). This feature lets one
armature replace the **setup pose** of selected bones without touching the
shared data, while the other armature keeps the original setup and continues to
play the same animations.

## Public API

```cpp
// Armature (DragonBones/src/dragonBones/armature/Armature.h)
bool setSetupPoseOverrides(const std::map<std::string, Transform>& overrides);
void clearSetupPoseOverrides();
bool hasSetupPoseOverrides() const;

// Slot (DragonBones/src/dragonBones/armature/Slot.h)
bool getDeformedVertices(std::vector<Point>& out);
```

* `setSetupPoseOverrides` installs a **complete** replacement mapping for this
  armature instance. Named bones use the supplied transform as their setup
  origin; bones absent from the map keep the shared `BoneData` origin. An empty
  mapping is equivalent to `clearSetupPoseOverrides()`.
* Unknown bone names, non-finite values, non-positive/non-finite scales and
  unsupported skeleton configurations make the call return `false`; existing
  overrides, the current pose, cached relations and outputs are left unchanged.
* After a successful set/clear, `advanceTime(0)` refreshes the pose immediately.
  No `AnimationState` is restarted, rebuilt or seeked, and animation time,
  weights, fades, slot display indices and FFD blend values are not modified.
* `getDeformedVertices` returns the slot's current armature-space control
  vertices from the real packed mesh data plus the current deform buffer. It
  does not advance time, never aliases shared packed data, and the returned
  vector is unaffected by later display switches, sampling or armature disposal.
  An empty or non-mesh current display returns `false` and leaves `out`
  untouched.

## Spatial boundary

Let `B0` be a bone's world matrix from the pure original setup hierarchy and
`B1` the same bone's world matrix recomputed from the overridden setup along
the **whole parent chain**. Neither contains offset, animation or flip.

DragonBones matrices use row-vector multiplication (a point is `p · M` and
`Matrix::concat(A, B)` computes `A · B`). The required column-notation relation

```
world = W · inverse(B1) · B0 · (p + d)
```

is therefore evaluated per bone as

```
R        = B0 · inverse(B1)          // full affine, applied to points
R_lin    = linear part of R          // translation removed, applied to FFD
world    = (R · p + R_lin · d) · W
```

where:

* `p` is the bone-local skinned vertex stored in the packed data;
* `d` is the current frame FFD direction in the same bone-local space;
* `W` is the bone's current world matrix, still produced by the existing
  combination of setup origin, `Bone.offset` and `animationPose`
  (Additive mode). The bind translation never reaches the FFD direction.

Consequences:

* In the neutral pose (no offset/animation) the skinned result is identical
  before and after overriding — the mesh is re-bound, not dragged.
* Ancestor setup changes propagate: even an unlisted descendant gets a new `B1`
  and its own `R`, while its origin pointer stays on the shared `BoneData`.
* Skinned output is not multiplied by the slot matrix. Unskinned meshes keep
  the original convention and are output through the slot transform.
* Switching the current mesh to another mesh of the same `ArmatureData`
  re-binds automatically.

## Cache boundary

The animation cache (`cacheFrameRate`, cached frames and per-bone/per-slot
cached frame indices) lives on the shared `ArmatureData` / `DragonBonesData`
and must keep serving every other armature:

* An armature with active overrides never sets the shared `cacheFrameRate` to
  zero and never reads or writes shared cache frames; it evaluates uncached on
  its own (`AnimationState` receives an effective cache rate of 0 only for this
  instance, so timeline time is not quantized).
* Other armatures keep their cached path and continue populating/consuming the
  shared cache.
* Clearing the overrides re-arms the normal cached/uncached production path and
  forces one uncached evaluation, so no frame produced while overrides were
  active can ever be read (the override path never wrote any).
* Disposing an armature (or reusing it from the object pool) drops the
  per-instance binding state; the shared data is never modified.

## Supported configuration

The initial implementation supports skeletons with:

* no constraints;
* every bone using `OffsetMode::Additive`;
* `inheritTranslation`, `inheritRotation`, `inheritScale` and
  `inheritReflection` all enabled;
* finite, strictly positive setup and override scales (2D rotation and skew are
  allowed);
* `flipX == false` and `flipY == false` while overrides are active.

Anything outside this contract is rejected by `setSetupPoseOverrides`.

## Layout

* `DragonBones/src/dragonBones/armature/SetupPoseBinding.h/.cpp` — per-instance
  binding computation and storage.
* `Armature`, `Bone`, `Slot`/`DeformVertices` production paths — override
  installation, bone origin/bind pointers, the `getDeformedVertices` core entry
  and mesh-change re-binding.
* `SFML/src/dragonBones/SFMLSlot.cpp` — weighted vertex evaluation now calls the
  core entry; triangle expansion, render coordinates and the unskinned branch
  are unchanged (SFML is an adaptation reference and is not built here).
* `tests/setup_binding_target.cpp` — `basis`, `deform` and `cache` groups.
* `examples/setup_binding.cpp` — a shared-character before/after demonstration.

Build and run everything with:

```
bash task_support/run.sh target
```
