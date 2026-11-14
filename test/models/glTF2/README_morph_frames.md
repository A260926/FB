# Morph frame completion example

`morph_frames_example.gltf` is a self-contained glTF 2.0 asset (data-URI
buffer, no external files, no download) with one triangle and two named
morph target expressions:

- `brow_raise` — moves the middle vertex up and back in z.
- `jaw_open` — moves two vertices to open the lower edge.

Both targets only replace POSITION, which is the case where Assimp's
`GenSmoothNormals` / `CalcTangentSpace` historically left the
`aiAnimMesh` normal and tangent streams empty.

## Configuration

Two independent opt-in switches are involved.

### Post processing: `AI_CONFIG_PP_MORPH_FRAME_COMPLETION`

Property name `PP_MORPH_FRAME_COMPLETION`, boolean, default `false`.

When enabled on an `Assimp::Importer`, the existing
`aiProcess_GenSmoothNormals` and `aiProcess_CalcTangentSpace` steps also
visit every morph target and write missing streams directly into each
`aiAnimMesh`:

- normals are computed from the target's own positions, with a spatial
  neighborhood rebuilt per target (the base-pose grouping is never
  reused); targets without positions inherit the base positions;
- tangent/bitangent pairs are computed from the target positions, the
  target normals (base normals are inherited when a target has none) and
  the base UV0 channel;
- existing artist-authored normals or tangent pairs are preserved, and
  base streams are never released by target processing.

### glTF2 export: `GLTF2_TARGET_TANGENT_EXP`

Boolean exporter property, default `false`.

When enabled, each morph target tangent is written as a plain
FLOAT/VEC3 `TANGENT` accessor holding `target.tangent.xyz -
base.tangent.xyz`, matching the existing `GLTF2_TARGET_NORMAL_EXP`
normal differences. The base tangent handedness (`w`) and target order
are preserved; the bitangent is reconstructed on import as
`w * (N x T)`. Only non-sparse accessors are emitted for target
tangents. Leave the switch off to keep the previous output.

## C++17 usage

```cpp
Assimp::Importer importer;
importer.SetPropertyBool(AI_CONFIG_PP_MORPH_FRAME_COMPLETION, true);
const aiScene *scene = importer.ReadFile("morph_frames_example.gltf", 0);
scene = importer.ApplyPostProcessing(
        aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace);

Assimp::ExportProperties properties;
properties.SetPropertyBool("GLTF2_TARGET_NORMAL_EXP", true);
properties.SetPropertyBool(AI_CONFIG_GLTF2_TARGET_TANGENT_EXP, true);
properties.SetPropertyBool("GLTF2_TARGETNAMES_EXP", true); // keep brow_raise/jaw_open

Assimp::Exporter exporter;
exporter.Export(scene, "gltf2", "completed.gltf", 0u, &properties);
```

After post processing each target reports `HasNormals()` and
`HasTangentsAndBitangents()` as true, and the exported file carries
POSITION/NORMAL/TANGENT differences for both expressions.
