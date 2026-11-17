# Mesh-local bone bindings (API)

This document describes the per-mesh binding data added to `SkeletalAnimation`,
how it flows through `SkeletalModel::Update`, CPU skinning and
`ModelToData`/`DataToModel`, and what stays compatible with the original
single-mesh API.

## Why

The original model stored weights and an inverse bind matrix directly on
`sBone`:

```cpp
struct sBone
{
    ...
    glm::mat4x4 OffsetMatrix; // inverse bind
    unsigned int NumWeights;
    sWeight* pWeights;        // VertexID indexes the one global mesh list
    ...
};
```

That works when a character is a single mesh. When a body and its equipment
share one skeleton, both have their own vertex numbering (each starts at 0) and
their own inverse bind matrices, so bone-level weights cannot express them.

## New types

```cpp
struct sMeshBoneBinding
{
    unsigned int BoneIndex;          // index into sSkeleton::Bones
    glm::mat4x4 OffsetMatrix;        // inverse bind, in this mesh's local space
    std::vector<sWeight> Weights;    // VertexID indexes this mesh only
};

struct sAnimatedMesh
{
    ...
    bool UseMeshBindings = false;               // opt-in, defaults to false
    std::vector<sMeshBoneBinding> MeshBindings; // empty table == static mesh
};
```

`sAnimatedMesh::MeshBindings` is owned by the mesh (an owning
`std::vector<sMeshBoneBinding>`). `sMeshBoneBinding::Weights` is likewise an
owning `std::vector<sWeight>`.

## Skinning rules

When `UseMeshBindings` is `true`, `Update` / CPU skinning use **only** the
mesh's own binding table, and `sWeight::VertexID` is interpreted strictly within
that mesh (`0 <= VertexID < NumVertices`).

For each binding the skinning matrix is

```
globalInverse * currentBoneGlobalPose * binding.OffsetMatrix
```

Vertices and normals are accumulated with the binding's weights, then every
transformed normal is normalized per vertex. A vertex with no weights at all
keeps its original position and its (unit) original normal; the normal is still
normalized, so a non-unit input normal becomes unit.

If `MeshBindings` is empty, the mesh is static: transformed positions and
unit normals are copies of the source data.

When `UseMeshBindings` is `false` (the default), the mesh uses the legacy path:
weights from `sBone::pWeights` and `sBone::OffsetMatrix`, exactly as before.
The legacy `sBone::FinalTransformation` still means
`globalInverse * boneGlobalPose * sBone::OffsetMatrix`.

### Hierarchy is independent of bindings

Bone global poses are computed from node local transforms and their parents
only:

```
boneGlobal = parentGlobal * boneLocal
```

A mesh's inverse bind matrix is never multiplied into the hierarchy and never
propagated to children. Consequently, multiple meshes cannot affect each
other's poses, and the last mesh processed cannot overwrite an earlier one's
results.

## Building a model in code

Programmatic construction of a single-mesh model is unchanged: fill
`sBone::pWeights` / `sBone::OffsetMatrix` and leave `UseMeshBindings` at its
default `false`.

For mesh-local bindings:

```cpp
SA::sAnimatedMesh Mesh = /* vertices, normals, indices */;

SA::sMeshBoneBinding Binding;
Binding.BoneIndex = 1;                 // skeleton bone
Binding.OffsetMatrix = InverseBind;    // mesh-local inverse bind
Binding.Weights.push_back({ /*VertexID*/ 0, /*Weight*/ 0.4f });
Binding.Weights.push_back({ /*VertexID*/ 0, /*Weight*/ 0.6f }); // same vertex, another bone

Mesh.UseMeshBindings = true;
Mesh.MeshBindings.push_back(Binding);
Model.AddMesh(Mesh);
```

A windowless, runnable version of this is in
[`examples/mesh_binding_example.cpp`](../examples/mesh_binding_example.cpp).

## Serialization

All three `ModelToData` overloads and `DataToModel` were updated together.

### New format (explicit version)

New output begins with an explicit marker:

```
u32 magic    = 0x534D4142 ("BAMS" in little-endian)
u32 version  = 2
```

Each mesh then writes, after its vertices/normals/indices:

```
u32 useMeshBindings      (0 or 1)
u32 numBindings          (may be 0: static mesh state is preserved)
per binding:
    u32 boneIndex
    mat4 offsetMatrix
    u32 numWeights
    per weight:
        u32 vertexId
        float weight
```

The rest (skeleton, animation, global inverse transform) follows the original
layout. `ModelToData(const SkeletalModel&, char*)` still returns `0` on error
(for example, a count too large to represent).

### Loading

`DataToModel(const char* data, SkeletalModel& outModel)` accepts both layouts:

* **New format** — detected by the magic/version header. Mesh bindings and the
  empty-table state are restored. All data is copied into the model; the model
  does not borrow the input bytes or any caller-owned container.
* **Legacy format** — detected by the original `1, 2, 3` header words.
  * A single-mesh legacy asset is unambiguous: its bone weights and offset
    matrices are converted into mesh-local bindings for that one mesh. This
    includes `task_support/legacy.sam`, which loads and animates.
  * A multi-mesh legacy asset is ambiguous — the old format cannot say which
    mesh a bone's weights belonged to. Loading returns `false` rather than
    guessing.

Strings are copied with their exact stored length, so names never gain trailing
characters.

### Ownership and reuse

Loading is transactional: the input is parsed into detached storage first, then
the target model is replaced via `Clear()` + commit. On success no state from a
previous asset survives; on failure the target model is left untouched.
`Clear()` frees meshes, bone arrays and the cached bone poses, so the same
model object can be reused for a different asset or toggled between old and new
assets without leaking bindings.

## Notes

* The Assimp converter was not extended: it still produces the legacy
  bone-level representation (single mesh per character).
* `MeshBindings` indexing is by `BoneIndex`, not by bone name, so a shared
  skeleton can be referenced by two meshes with different binding layouts.
