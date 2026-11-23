# Morph target frame completion — two-expression example

`morph_frames_two_expressions.gltf` is a self-contained glTF 2.0 asset
(data URI, no external buffers) with one inclined triangle and two
expression morph targets, `smile` (weight 0.25) and `frown` (weight 0.75).
The targets carry **positions only** — the case where Assimp's
`aiProcess_GenSmoothNormals` / `aiProcess_CalcTangentSpace` used to complete
the base mesh but left every `aiAnimMesh` without normals and tangents, so
normal-mapped expressions rendered with the base pose frame.

## Import: completing the morph target frames

Frame completion is an optional post-processing step. It is off by default
and enabled through the importer property
`AI_CONFIG_PP_MORPH_FRAME_COMPLETION` (`"PP_MORPH_FRAME_COMPLETION"`).
Which streams are completed is still selected by the usual process flags:

```cpp
Assimp::Importer importer;
importer.SetPropertyBool(AI_CONFIG_PP_MORPH_FRAME_COMPLETION, true);

const aiScene *scene = importer.ReadFile("morph_frames_two_expressions.gltf", 0);
scene = importer.ApplyPostProcessing(aiProcess_GenSmoothNormals |
                                     aiProcess_CalcTangentSpace);
// scene->mMeshes[0]->mAnimMeshes[i] now has mNormals, mTangents, mBitangents
```

Behavior with the option on:

- The base mesh streams are produced by the original steps under their
  original rules; the completion step never rebuilds a stream that already
  exists, on the base mesh or on any target (artist data is kept as-is).
- The normal pass fills missing target normals from the target's own
  positions (targets without positions use the base positions).
- The tangent pass fills missing target tangents/bitangents from the
  target's positions, the target's normals (inherited from the base mesh
  when the target has none) and the base mesh's UV channel 0.
- Smoothing angles follow `AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE` and
  `AI_CONFIG_PP_CT_MAX_SMOOTHING_ANGLE`; the same-position neighborhood is
  always rebuilt from the positions of the target being completed, never
  from the base pose.
- Each target owns its completed arrays; vertex/face/target order, target
  names and weights are unchanged, and re-running the post-processing does
  not rebuild existing frames.

## Export: writing the completed frames back to glTF

Morph target tangents are exported as diffs, like positions and normals,
behind a new export property (default off, non-sparse `FLOAT`/`VEC3`
`TANGENT` accessors in `targets[]`):

```cpp
Assimp::Exporter exporter;
Assimp::ExportProperties props;
props.SetPropertyBool("GLTF2_TARGET_TANGENT_EXP", true); // target TANGENT diffs
props.SetPropertyBool("GLTF2_TARGET_NORMAL_EXP", true);  // target NORMAL diffs
props.SetPropertyBool("GLTF2_TARGETNAMES_EXP", true);    // extras.targetNames
exporter.Export(scene, "gltf2", "out.gltf", 0, &props);
```

Each target `TANGENT` accessor stores `target_tangent.xyz - base_tangent.xyz`;
the base `TANGENT` keeps its `w` handedness, and target order is preserved.
On reimport the endpoint normals and tangents are restored per target and
the bitangents are rebuilt from the glTF representation as `w * (N x T)`
(the internal per-vertex bitangent array is not expected to round-trip
component-wise). With the switches off the export keeps the legacy
positions-only target layout.
