// Windowless example: skinning two meshes (body + equipment) that share one
// skeleton but each carry their own vertex numbering and inverse bind matrix.
//
// Build (from the repository root):
//   c++ -std=c++11 -I. -Ivendor/glm \
//       examples/mesh_binding_example.cpp SkeletalAnimation/*.cpp \
//       -o .task-build/mesh_binding_example
//
// The example prints the skinned positions of both meshes and exits.

#include "SkeletalAnimation/SkeletalModel.h"
#include "SkeletalAnimation/SkeletalModelSerializer.h"

#include <glm/glm.hpp>
#include <glm/gtx/transform.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
	// Small helper to build a mesh whose transformed buffers are allocated.
	void SetupMesh(SA::sAnimatedMesh& a_Mesh, const std::vector<glm::vec3>& a_Vertices,
	               const std::vector<glm::vec3>& a_Normals)
	{
		a_Mesh.NumVertices = (unsigned int)a_Vertices.size();
		a_Mesh.pVertices = new glm::vec3[a_Mesh.NumVertices];
		a_Mesh.pNormals = new glm::vec3[a_Mesh.NumVertices];
		a_Mesh.pTransformedVertices = new glm::vec3[a_Mesh.NumVertices];
		a_Mesh.pTransformedNormals = new glm::vec3[a_Mesh.NumVertices];
		for (unsigned int i = 0; i < a_Mesh.NumVertices; ++i)
		{
			a_Mesh.pVertices[i] = a_Vertices[i];
			a_Mesh.pNormals[i] = a_Normals[i];
		}
		a_Mesh.NumIndices = 0;
		a_Mesh.pIndices = NULL;
		a_Mesh.UseMeshBindings = true;
	}

	SA::sMeshBoneBinding Binding(unsigned int a_BoneIndex, const glm::mat4& a_Offset,
	                             const std::vector<SA::sWeight>& a_Weights)
	{
		SA::sMeshBoneBinding Out;
		Out.BoneIndex = a_BoneIndex;
		Out.OffsetMatrix = a_Offset;
		Out.Weights = a_Weights;
		return Out;
	}
}

int main()
{
	SA::SkeletalModel Model;

	// A two-bone hierarchy: "root" (translated and rotated) with child "hand".
	{
		SA::sBone Root;
		Root.Name = "root";
		Root.NodeTransform = glm::translate(glm::vec3(1.0f, 0.0f, 0.0f)) *
		                     glm::rotate(glm::mat4(1.0f), 30.0f, glm::vec3(0.0f, 1.0f, 0.0f));
		Root.NumChildren = 1;
		Root.pChildren = new unsigned int[1];
		Root.pChildren[0] = 1;

		SA::sBone Hand;
		Hand.Name = "hand";
		Hand.NodeTransform = glm::translate(glm::vec3(0.0f, 0.0f, 2.0f));

		Model.GetSkeleton().Bones.push_back(Root);
		Model.GetSkeleton().Bones.push_back(Hand);
		Model.SetGlobalInverseTransform(glm::mat4(1.0f));
	}

	// Body mesh: 4 vertices, vertex IDs local to this mesh.
	{
		SA::sAnimatedMesh Body;
		SetupMesh(Body,
			{ glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.5f, 0.0f, 0.0f),
			  glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 0.0f, 0.5f) },
			{ glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f),
			  glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f) });

		Body.MeshBindings.push_back(Binding(0, glm::mat4(1.0f),
			{ { 0, 0.75f }, { 1, 1.0f } }));
		Body.MeshBindings.push_back(Binding(1, glm::mat4(1.0f),
			{ { 0, 0.25f }, { 2, 1.0f } }));
		// Vertex 3 has no weights and keeps its bind pose.
		Model.AddMesh(Body);
	}

	// Equipment mesh: 2 vertices starting again at ID 0, but a different bind
	// matrix for the same bones.
	{
		SA::sAnimatedMesh Sword;
		SetupMesh(Sword,
			{ glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
			{ glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f) });

		glm::mat4 SwordBind = glm::translate(glm::vec3(0.0f, 0.0f, 3.0f));
		Sword.MeshBindings.push_back(Binding(1, SwordBind,
			{ { 0, 1.0f }, { 1, 1.0f } }));
		Model.AddMesh(Sword);
	}

	Model.GetAnimation().TicksPerSecond = 1.0f;
	Model.GetAnimation().Duration = 2.0f;
	Model.Update(0.0f);

	std::printf("body vertex 0: %.3f %.3f %.3f\n",
		Model.GetMesh(0).pTransformedVertices[0].x,
		Model.GetMesh(0).pTransformedVertices[0].y,
		Model.GetMesh(0).pTransformedVertices[0].z);
	std::printf("body vertex 3 (unweighted): %.3f %.3f %.3f\n",
		Model.GetMesh(0).pTransformedVertices[3].x,
		Model.GetMesh(0).pTransformedVertices[3].y,
		Model.GetMesh(0).pTransformedVertices[3].z);
	std::printf("sword vertex 0: %.3f %.3f %.3f\n",
		Model.GetMesh(1).pTransformedVertices[0].x,
		Model.GetMesh(1).pTransformedVertices[0].y,
		Model.GetMesh(1).pTransformedVertices[0].z);

	// Round trip through the binary format; the loaded copy owns its bindings.
	std::string Blob = SA::ModelToData(Model);
	SA::SkeletalModel Loaded;
	if (SA::DataToModel(Blob.data(), Loaded))
		std::printf("reloaded %u meshes, %u bindings on mesh 0\n",
			Loaded.GetNumMeshes(),
			(unsigned)Loaded.GetMesh(0).MeshBindings.size());

	return 0;
}
