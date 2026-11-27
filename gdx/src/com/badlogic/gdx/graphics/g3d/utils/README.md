# PoseSpaceDeformer

`PoseSpaceDeformer` adds pose-space corrective deformation on top of the normal
libGDX animation and skinning pipeline. Art ships a handful of joint poses with
sparse per-vertex displacements; at runtime the corrective deltas are
interpolated from the current pose by an exact kernel solve, applied **before**
linear skinning, and the resulting world positions are written to a caller
owned array. The GPU mesh, the animation system and the default shaders are not
modified.

The class is pure Java: no OpenGL, no JNI/native libraries and no additional
dependencies. All kernel and linear algebra work runs in `double`; only the
output positions are `float`.

## Minimal Java usage

```java
import com.badlogic.gdx.graphics.g3d.utils.PoseSpaceDeformer;
import com.badlogic.gdx.graphics.g3d.utils.PoseSpaceDeformer.Prepared;
import com.badlogic.gdx.graphics.g3d.utils.PoseSpaceDeformer.Sample;
import com.badlogic.gdx.math.Quaternion;

// One-time, offline preparation (thread-safe result, private copies of all inputs).
Quaternion[] bindRotations = {new Quaternion(), new Quaternion()};
double[] driverWeights = {1.0, 0.7};
Sample neutral = new Sample(
    new Quaternion[] {new Quaternion(), new Quaternion()},
    new int[0], new float[0]);                 // required: bind-equivalent, zero deltas
Sample flex = new Sample(
    new Quaternion[] {new Quaternion().setFromAxis(0, 0, 1, 50), new Quaternion()},
    new int[] {2, 3},
    new float[] {0.12f, -0.08f, 0.05f, -0.04f, 0.10f, 0.02f});
Prepared deformer = PoseSpaceDeformer.prepare(
    vertexCount,
    new String[] {"elbow", "wrist"},
    bindRotations,
    driverWeights,
    4.0,                                      // positive beta
    new Sample[] {neutral, flex /* , ... */});

// Per frame, after AnimationController.update(...) and instance.calculateTransforms():
float[] outWorld = new float[vertexCount * 3];
deformer.evaluate(instance, nodePart, bindPositions, boneIndices, boneWeights, outWorld);
```

`boneIndices` and `boneWeights` hold four palette slots per vertex
(`vertexCount * 4` entries each). Indices address `NodePart.bones`; weights are
non-negative and sum to one per vertex, and zero-weight slots still need legal
indices. `outWorld` may be the same array as `bindPositions` (in-place output).

## Coordinate spaces

1. **Deltas and bind positions are in bind mesh space.** A sample delta is the
   displacement added to the un-posed bind position of that vertex.
2. For every driver bone the relative rotation is
   `r_b = bindRotation_b^-1 * localRotation_b`, where the current local rotation
   is read from the node's **animated local matrix** (`Node.localTransform`),
   not from `Node.rotation`, which the animation controller leaves untouched.
   Only the bone's own local rotation participates; parent motion does not.
3. The pose distance between two poses is
   `sum_b weight_b * (1 - dot(r_b, s_b)^2)` with quaternion dots clamped to
   `[-1, 1]`, and the kernel is `K = exp(-beta * distance)`.
4. Per vertex, the interpolated delta is added to the bind position first, then
   the corrected point is transformed with the part's already updated
   `NodePart.bones` palette using four-weight linear blending, and finally
   multiplied by `ModelInstance.transform`. The mesh node's
   `globalTransform` is therefore not applied a second time, and bone global
   transforms never replace the inverse-bind-aware palette.
5. Only positions are produced. The `Mesh`, GPU buffers and normals are left
   alone, which makes the same `Prepared` object usable for offline deformation
   previews and collision-point sampling alike. A single `Prepared` can be
   evaluated against several instances, including interleaved calls.

Driver local matrices must be rotation plus positive uniform scale and
translation: shear, reflection, singular matrices and non-uniform scale are
rejected. The instance world matrix may be any finite, invertible affine
transform.

## Ill-conditioned and duplicate samples

The sample kernel matrix `K` is LU decomposed with partial pivoting once during
`prepare`. If any pivoted element has absolute value below `1e-10`,
`prepare` throws `IllegalArgumentException`. No regularization is added and no
sample is silently discarded.

Equivalent poses are rejected up front, including poses that only differ by the
sign of any driver quaternion (`q` and `-q` describe the same orientation). A
missing neutral sample, duplicate driver ids, non-unit or non-finite rotations,
non-positive `beta`/weights, out-of-range vertices, repeated vertex indices and
non-finite deltas are likewise rejected with `IllegalArgumentException`.

To fix a rejected data set:

* remove the redundant (or numerically near-coincident) pose and fold its
  deltas into the surviving sample, or
* move the two poses further apart in orientation so the kernel matrix becomes
  well conditioned.

Do not "fix" it by shrinking deltas or adding a diagonal ridge — the solve is
deliberately exact (`delta = k^T K^-1 D`), keeps negative interpolation
coefficients, and never falls back to a normalized weighted average.

## Validation contract of `evaluate`

All inputs are validated before any output component is written, so a failed
call leaves `outWorld` unchanged. Failures include a `NodePart` owned by a
different instance, a missing/non-finite palette, wrong array lengths, illegal
bone indices or weights, a missing driver node, an illegal driver local
transform, or a non-invertible instance transform — all reported as
`IllegalArgumentException`. The instance, its bones/palette, the animation
state and the `Prepared` object are never mutated by an evaluation.
