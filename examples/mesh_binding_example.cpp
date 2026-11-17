// Windowless example: per-mesh bone bindings (mesh-local skinning).
//
// Shows how to build a character in code where a body and a piece of equipment
// share one skeleton, but each mesh has its own vertex numbering and its own
// inverse bind matrices. No window, graphics API or input is used: only the
// CPU skinning results are printed.
//
// Build (from the repository root):
//   c++ -std=c++11 -I. -Ivendor/glm examples/mesh_binding_example.cpp \
//       SkeletalAnimation/SkeletalModel.cpp SkeletalAnimation/SkeletalModelSerializer.cpp \
//       -o mesh_binding_example
// Run:
//   ./mesh_binding_example

#include "SkeletalAnimation/SkeletalModel.h"
#include "SkeletalAnimation/SkeletalModelSerializer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace SA;

namespace
{
	glm::mat4x4 RotationDegrees(float a_Degrees, const glm::vec3& a_Axis)
	{
		return glm::rotate(a_Degrees, a_Axis);
	}

	glm::vec3 Normalize(const glm::vec3& a_V)
	{
		float LengthSq = glm::dot(a_V, a_V);
		return LengthSq > 0.0f ? a_V / std::sqrt(LengthSq) : a_V;
	}

	sBone MakeBone(const char* a_Name, const glm::mat4x4& a_Transform)
	{
		sBone Bone;
		Bone.Name = a_Name;
		Bone.NodeTransform = a_Transform;
		Bone.OffsetMatrix = glm::mat4x4(1.0f);
		Bone.FinalTransformation = glm::mat4x4(1.0f);
		Bone.NumWeights = 0;
		Bone.pWeights = NULL;
		Bone.NumChildren = 0;
		Bone.pChildren = NULL;
		return Bone;
	}

	// Adds a mesh. Takes the vertex data by value; the model owns its arrays.
	void AddMesh(SkeletalModel& a_Model,
		const std::vector<glm::vec3>& a_Vertices,
		const std::vector<glm::vec3>& a_Normals,
		const std::vector<sMeshBoneBinding>& a_Bindings)
	{
		sAnimatedMesh Mesh;
		Mesh.NumVertices = (unsigned int)a_Vertices.size();
		Mesh.pVertices = new glm::vec3[Mesh.NumVertices];
		Mesh.pNormals = new glm::vec3[Mesh.NumVertices];
		Mesh.pTransformedVertices = new glm::vec3[Mesh.NumVertices]();
		Mesh.pTransformedNormals = new glm::vec3[Mesh.NumVertices]();
		for (unsigned int i = 0; i < Mesh.NumVertices; ++i)
		{
			Mesh.pVertices[i] = a_Vertices[i];
			Mesh.pNormals[i] = a_Normals[i];
		}
		Mesh.NumIndices = 0;
		Mesh.pIndices = NULL;

		// Opt in to mesh-local bindings. The binding table is owned by the mesh;
		// an empty table marks a static mesh.
		Mesh.UseMeshBindings = true;
		Mesh.MeshBindings = a_Bindings;

		a_Model.AddMesh(Mesh);
	}

	sMeshBoneBinding MakeBinding(unsigned int a_BoneIndex, const glm::mat4x4& a_InverseBind,
		unsigned int a_VertexId, float a_Weight)
	{
		sMeshBoneBinding Binding;
		Binding.BoneIndex = a_BoneIndex;
		Binding.OffsetMatrix = a_InverseBind;
		sWeight Weight;
		Weight.VertexID = a_VertexId;
		Weight.Weight = a_Weight;
		Binding.Weights.push_back(Weight);
		return Binding;
	}

	void PrintMesh(const char* a_Label, const sAnimatedMesh& a_Mesh)
	{
		std::printf("%s\n", a_Label);
		for (unsigned int i = 0; i < a_Mesh.NumVertices; ++i)
		{
			const glm::vec3& V = a_Mesh.pTransformedVertices[i];
			const glm::vec3& N = a_Mesh.pTransformedNormals[i];
			std::printf("  vertex %u pos=(%.3f, %.3f, %.3f) normal=(%.3f, %.3f, %.3f)\n",
				i, V.x, V.y, V.z, N.x, N.y, N.z);
		}
	}
}

int main()
{
	SkeletalModel Model;

	// A two-bone chain built in code. The child's pose is the parent's global
	// transform times the child's local transform, independent of any mesh.
	sBone Root = MakeBone("root",
		glm::translate(glm::vec3(2.0f, 0.0f, 0.0f)) * RotationDegrees(30.0f, glm::vec3(0.0f, 1.0f, 0.0f)));
	Root.NumChildren = 1;
	Root.pChildren = new unsigned int[1];
	Root.pChildren[0] = 1;
	Model.GetSkeleton().Bones.push_back(Root);

	sBone Child = MakeBone("child",
		glm::translate(glm::vec3(0.0f, 1.5f, 0.0f)) * RotationDegrees(-45.0f, glm::vec3(1.0f, 0.0f, 0.0f)));
	Model.GetSkeleton().Bones.push_back(Child);

	Model.SetGlobalInverseTransform(glm::mat4x4(1.0f));
	Model.GetAnimation().TicksPerSecond = 1.0f;
	Model.GetAnimation().Duration = 2.0f;

	// Body: 3 vertices, weighted to the root.
	const glm::mat4x4 BodyInverseBind =
		glm::translate(glm::vec3(0.0f, -0.5f, 0.0f)) * RotationDegrees(-12.0f, glm::vec3(0.0f, 0.0f, 1.0f));
	std::vector<sMeshBoneBinding> BodyBindings;
	BodyBindings.push_back(MakeBinding(0, BodyInverseBind, 0, 0.5f));
	BodyBindings.push_back(MakeBinding(0, BodyInverseBind, 1, 1.0f));
	BodyBindings.push_back(MakeBinding(0, BodyInverseBind, 2, 1.0f));

	// Equipment: only 2 vertices, whose ids 0 and 1 overlap the body's, but it
	// binds them to the child bone with a different inverse bind matrix.
	const glm::mat4x4 GearInverseBind =
		glm::translate(glm::vec3(0.25f, -1.0f, 0.5f)) * RotationDegrees(70.0f, glm::vec3(1.0f, 0.0f, 0.0f));
	std::vector<sMeshBoneBinding> GearBindings;
	GearBindings.push_back(MakeBinding(1, GearInverseBind, 0, 1.0f));
	GearBindings.push_back(MakeBinding(1, GearInverseBind, 1, 1.0f));

	AddMesh(Model,
		{ glm::vec3(0.5f, 0.0f, 0.0f), glm::vec3(-0.5f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
		{ glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f) },
		BodyBindings);

	AddMesh(Model,
		{ glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 2.0f, 0.0f) },
		{ glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
		GearBindings);

	// A third mesh with no bindings is static: positions and unit normals pass
	// through unchanged.
	AddMesh(Model,
		{ glm::vec3(1.0f, 2.0f, 3.0f) },
		{ glm::vec3(0.0f, 4.0f, 3.0f) },
		std::vector<sMeshBoneBinding>());

	Model.Update(0.0f);

	PrintMesh("body (root binding)", Model.GetMesh(0));
	PrintMesh("equipment (child binding, overlapping vertex ids)", Model.GetMesh(1));
	PrintMesh("static mesh (empty binding table)", Model.GetMesh(2));

	// Serialize and load back. The loaded model owns its data and can be
	// animated further; the new format carries the mesh bindings and their
	// empty-table state.
	std::string Blob = ModelToData(Model);
	SkeletalModel Loaded;
	if (DataToModel(Blob.data(), Loaded))
	{
		Loaded.Update(0.5f);
		PrintMesh("equipment after byte round-trip and update", Loaded.GetMesh(1));
	}

	// Reuse the same object for a different asset; nothing from the previous
	// model survives.
	Loaded.Clear();
	std::printf("after Clear: meshes=%u bones=%u\n",
		Loaded.GetNumMeshes(), (unsigned int)Loaded.GetSkeleton().Bones.size());

	// Sanity line so the example can be checked by a script.
	std::printf("EXAMPLE_OK meshes=%u blob_bytes=%u\n",
		Model.GetNumMeshes(), (unsigned int)Blob.size());
	return 0;
}
