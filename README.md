# Skeletal Animation and Software Skinning
The aim of this project is to provide simple skeletal animation and software skinning (on the CPU) in C++.

This project can be used by developers who want to quickly add simple animations to their project. For commercial software this project may be... lacking... but can be used as a base to improve upon.

## Features
Feature       | Yes          | No     | Notes
------------- | :----------: | :----: | ----
Model Loading | V            |        | By using Assimp
Skeletal Animation | V       |        |
Vertex Animation |           | X      |
Rendering     |              | X      |
CPU Skinning  | V            |        |
GPU Skinning  |              | X      |
Skinned Vertices | V         |        |
Skinned Normals |            | X      | To do
Materials     |              | X      | To do
Textures      |              | X      | To do
Multi-threaded  |            | X      | Will maybe add
Highly Optimized    |        | X      | 

## Dependencies
The SkeletalAnimation project is only dependent on [GLM](http://glm.g-truc.net/).

The AssimpConverter project is dependent on [Assimp](http://assimp.sourceforge.net/index.html) and SkeletalAnimation.

In order to compile the projects, both project files need to be modified to use the include files, and link with their respective dependencies.

## Usage
Make sure your project links with `SkeletalAnimation.lib`, or compile `SkeletalModel.cpp` into your project.

### Loading
If you do not have your own model loader you can use Assimp to load a model and use AssimpConverter to convert it to a `SkeletalModel` that can be rendered and animated:

```
void LoadModel()
{
	Assimp::Importer Importer;
	const aiScene* pScene = Importer.ReadFile("some_animated_model.fbx",
		aiProcess_LimitBoneWeights |
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_SortByPType);

	AssimpConverter::Convert(pScene, g_AnimatedModel);
}
```

### Animating
After the `SkeletalModel` has been loaded it needs to be updated every frame to make it animate:
```
void Update(float a_Dt) // a_Dt is the elapsed time since last frame in seconds
{
  g_AnimatedModel.Update(a_Dt);
}
```

### Mesh-local bone bindings

By default weights and `OffsetMatrix` live on the bones (the legacy layout):
two meshes sharing that skeleton then share one vertex numbering and one
inverse bind pose.

When a body and its equipment share a skeleton but each mesh numbers its
vertices from zero and has its own bind pose, use mesh-local bindings instead.
Each `sAnimatedMesh` carries an array of `sMeshBoneBinding` and enables it with
`UseMeshBindings`:

```cpp
struct sMeshBoneBinding
{
    unsigned int             BoneIndex;    // index into sSkeleton::Bones
    glm::mat4x4              OffsetMatrix; // inverse bind matrix for this mesh
    std::vector<sWeight>     Weights;      // VertexIDs are local to this mesh
};
```

```cpp
SA::sAnimatedMesh Body;
Body.UseMeshBindings = true;
Body.pVertices = ...;             // NumVertices entries
Body.pNormals = ...;

SA::sMeshBoneBinding Binding;
Binding.BoneIndex = 0;            // "root"
Binding.OffsetMatrix = BindMatrix;
Binding.Weights.push_back(SA::sWeight{ 0, 0.75f }); // local vertex 0
Binding.Weights.push_back(SA::sWeight{ 1, 0.25f });
Body.MeshBindings.push_back(Binding);
g_AnimatedModel.AddMesh(Body);
```

Semantics when `UseMeshBindings` is true:

* Only this mesh's `MeshBindings` are used; vertex IDs are interpreted within
  the mesh. Bones' own weights/`OffsetMatrix` are ignored for it.
* An empty binding table marks the mesh as static: positions and unit normals
  keep their original values.
* For each binding the skinning matrix is
  `globalInverse * bone.GlobalTransformation * binding.OffsetMatrix`, where
  `GlobalTransformation` is the hierarchy pose derived only from node local
  transforms (it never includes an inverse bind matrix of its own).
* Positions and normals are accumulated with the weights and the resulting
  normal is normalized per vertex. Vertices with no weights keep their values.

The legacy single-mesh path keeps working unchanged: leave `UseMeshBindings`
false and put weights/`OffsetMatrix` on the bones. `FinalTransformation` keeps
its original meaning there (`globalInverse * hierarchyPose * bone.OffsetMatrix`).

When serializing, new data is written with an explicit format version. Mesh
bindings and the empty-table/static state are stored per mesh and loaded into
freshly owned storage. Legacy single-mesh files still load through the same
`DataToModel` entry point; legacy files with more than one mesh are rejected
because their weight ownership cannot be determined.

There is a windowless, buildable example in
`examples/mesh_binding_example.cpp`:

```
c++ -std=c++11 -I. -Ivendor/glm \
    examples/mesh_binding_example.cpp SkeletalAnimation/*.cpp \
    -o .task-build/mesh_binding_example
.task-build/mesh_binding_example
```

### Rendering
Here is some (very crude) example code to demonstrate how to render the animated model using OpenGL:
```
void Render()
{
	for (unsigned int i = 0; i < g_AnimatedModel.GetNumMeshes(); ++i)
	{
		const SA::sAnimatedMesh& AnimMesh = g_AnimatedModel.GetMesh(i);

		glBegin(GL_TRIANGLES);
		for (unsigned int i = 0; i < AnimMesh.NumIndices; ++i)
		{
			unsigned int Index = AnimMesh.pIndices[i];
			glm::vec3 n = AnimMesh.pNormals[Index];
			glm::vec3 v = AnimMesh.pTransformedVertices[Index];
			
			glColor4f(n.x, n.y, n.z, 1);
			glVertex3f(v.x, v.y, v.z);
		}
		glEnd();
	}
}
```

# References

http://ogldev.atspace.co.uk/www/tutorial38/tutorial38.html
