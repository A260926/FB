// Regression tests for per-mesh bone bindings.
//
// Builds a small character by hand: a parent bone and a child bone, plus two
// meshes whose vertex indices overlap but whose bindings differ. Reference
// values are accumulated independently from the implementation, i.e. with an
// explicit globalInverse * parentGlobal * childLocal * inverseBind product,
// so a bug that leaks one mesh's inverse bind into the hierarchy, or that lets
// one mesh overwrite another, fails the numeric checks.

#include "task_support/check.h"

#include "SkeletalAnimation/SkeletalModel.h"
#include "SkeletalAnimation/SkeletalModelSerializer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace SA;

namespace
{
	const float kPi = 3.14159265358979323846f;
	const float kEps = 1e-4f;

	bool Near(float a_A, float a_B, float a_Eps = kEps)
	{
		return std::fabs(a_A - a_B) <= a_Eps;
	}

	bool NearVec(const glm::vec3& a_A, const glm::vec3& a_B, float a_Eps = kEps)
	{
		return Near(a_A.x, a_B.x, a_Eps) && Near(a_A.y, a_B.y, a_Eps) && Near(a_A.z, a_B.z, a_Eps);
	}

	bool NearMat(const glm::mat4x4& a_A, const glm::mat4x4& a_B, float a_Eps = kEps)
	{
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
				if (!Near(a_A[c][r], a_B[c][r], a_Eps))
					return false;
		return true;
	}

	// Rotation helper using the bundled (old) GLM: degrees about an axis.
	glm::mat4x4 RotationDegrees(float a_Degrees, const glm::vec3& a_Axis)
	{
		return glm::rotate(a_Degrees, a_Axis);
	}

	glm::vec3 XformPoint(const glm::mat4x4& a_M, const glm::vec3& a_P)
	{
		glm::vec4 Result = a_M * glm::vec4(a_P, 1.0f);
		return glm::vec3(Result);
	}

	glm::vec3 XformNormal(const glm::mat4x4& a_M, const glm::vec3& a_N)
	{
		return glm::mat3x3(a_M) * a_N;
	}

	glm::vec3 SafeNormalize(const glm::vec3& a_V)
	{
		float LengthSq = glm::dot(a_V, a_V);
		if (LengthSq <= 0.0f)
			return a_V;
		return a_V / std::sqrt(LengthSq);
	}

	//////////////////////////////////////////////////////////////////////////
	// Shared fixture: parent "root" at a non-trivial rotation/translation,
	// child "child" with its own rotation and translation. Poses are built so
	// parent * child is not commutative with anything the tests reorder.

	struct Fixture
	{
		glm::mat4x4 ParentLocal;
		glm::mat4x4 ChildLocal;
		glm::mat4x4 GlobalInverse;

		Fixture()
		{
			ParentLocal = glm::translate(glm::vec3(3.0f, -1.0f, 2.0f)) * RotationDegrees(37.0f, glm::vec3(0.0f, 1.0f, 0.0f));
			ChildLocal = glm::translate(glm::vec3(-2.0f, 4.0f, 1.0f)) * RotationDegrees(53.0f, glm::vec3(1.0f, 0.0f, 0.0f));
			GlobalInverse = glm::translate(glm::vec3(0.5f, -0.25f, 0.125f)) * RotationDegrees(-11.0f, glm::vec3(0.0f, 0.0f, 1.0f));
		}

		glm::mat4x4 ParentGlobal() const { return ParentLocal; }
		glm::mat4x4 ChildGlobal() const { return ParentLocal * ChildLocal; }

		// Reference skinning matrix for a mesh binding on the given bone.
		glm::mat4x4 SkinMatrix(bool a_Child, const glm::mat4x4& a_InverseBind) const
		{
			return GlobalInverse * (a_Child ? ChildGlobal() : ParentGlobal()) * a_InverseBind;
		}
	};

	sBone MakeBone(const char* a_Name, const glm::mat4x4& a_NodeTransform, unsigned int a_Child = 0xFFFFFFFFu)
	{
		sBone Bone;
		Bone.Name = a_Name;
		Bone.NodeTransform = a_NodeTransform;
		Bone.OffsetMatrix = glm::mat4x4(1.0f);
		Bone.FinalTransformation = glm::mat4x4(1.0f);
		Bone.NumWeights = 0;
		Bone.pWeights = NULL;
		if (a_Child == 0xFFFFFFFFu)
		{
			Bone.NumChildren = 0;
			Bone.pChildren = NULL;
		}
		else
		{
			Bone.NumChildren = 1;
			Bone.pChildren = new unsigned int[1];
			Bone.pChildren[0] = a_Child;
		}
		return Bone;
	}

	void AddBone(SkeletalModel& a_Model, const sBone& a_Bone)
	{
		a_Model.GetSkeleton().Bones.push_back(a_Bone);
	}

	// Takes ownership of the arrays it allocates.
	sAnimatedMesh MakeMesh(const std::vector<glm::vec3>& a_Vertices, const std::vector<glm::vec3>& a_Normals)
	{
		sAnimatedMesh Mesh;
		Mesh.NumVertices = (unsigned int)a_Vertices.size();
		Mesh.pVertices = new glm::vec3[Mesh.NumVertices];
		Mesh.pNormals = new glm::vec3[Mesh.NumVertices];
		Mesh.pTransformedVertices = new glm::vec3[Mesh.NumVertices];
		Mesh.pTransformedNormals = new glm::vec3[Mesh.NumVertices];
		for (unsigned int i = 0; i < Mesh.NumVertices; ++i)
		{
			Mesh.pVertices[i] = a_Vertices[i];
			Mesh.pNormals[i] = a_Normals[i];
			Mesh.pTransformedVertices[i] = glm::vec3(0.0f);
			Mesh.pTransformedNormals[i] = glm::vec3(0.0f);
		}
		Mesh.NumIndices = 0;
		Mesh.pIndices = NULL;
		Mesh.UseMeshBindings = false;
		return Mesh;
	}

	sMeshBoneBinding MakeBinding(unsigned int a_BoneIndex, const glm::mat4x4& a_InverseBind,
		const std::vector<unsigned int>& a_VertexIds, const std::vector<float>& a_Weights)
	{
		sMeshBoneBinding Binding;
		Binding.BoneIndex = a_BoneIndex;
		Binding.OffsetMatrix = a_InverseBind;
		for (unsigned int i = 0; i < a_VertexIds.size(); ++i)
		{
			sWeight Weight;
			Weight.VertexID = a_VertexIds[i];
			Weight.Weight = a_Weights[i];
			Binding.Weights.push_back(Weight);
		}
		return Binding;
	}

	void SetAnimation(SkeletalModel& a_Model)
	{
		a_Model.GetAnimation().TicksPerSecond = 1.0f;
		a_Model.GetAnimation().Duration = 4.0f;
	}
}

//////////////////////////////////////////////////////////////////////////////
// palettes: numeric skinning with mesh-local bindings
//////////////////////////////////////////////////////////////////////////////

// Two meshes, overlapping vertex ids, different inverse binds. Checks that
// each mesh uses only its own binding and its own vertex numbering, and that
// the two meshes do not overwrite each other.
static int Palettes_TwoMeshesOverlappingIds()
{
	Fixture F;
	SkeletalModel Model;
	AddBone(Model, MakeBone("root", F.ParentLocal, 1));
	AddBone(Model, MakeBone("child", F.ChildLocal));
	Model.SetGlobalInverseTransform(F.GlobalInverse);
	SetAnimation(Model);

	// Mesh A: 3 vertices. Vertex 0 is influenced by both bones (split weights),
	// vertices 1 and 2 by one bone each. Its inverse binds differ from Mesh B's.
	const std::vector<glm::vec3> VertsA = {
		glm::vec3(1.0f, 2.0f, -1.0f),
		glm::vec3(-2.0f, 0.5f, 3.0f),
		glm::vec3(0.25f, -3.0f, 1.5f)
	};
	const std::vector<glm::vec3> NormsA = {
		glm::vec3(0.0f, 1.0f, 0.0f),
		glm::vec3(1.0f, 0.0f, 1.0f),
		glm::vec3(-1.0f, 1.0f, 0.0f)
	};
	const glm::mat4x4 InvBindA0 = glm::translate(glm::vec3(0.75f, 0.0f, -0.5f)) * RotationDegrees(21.0f, glm::vec3(0.0f, 0.0f, 1.0f));
	const glm::mat4x4 InvBindA1 = glm::translate(glm::vec3(-0.5f, 1.25f, 0.0f)) * RotationDegrees(-47.0f, glm::vec3(1.0f, 0.0f, 0.0f));

	// Mesh B: 2 vertices (different count), ids 0 and 1 collide with Mesh A.
	const std::vector<glm::vec3> VertsB = {
		glm::vec3(-1.5f, 1.0f, 0.75f),
		glm::vec3(2.0f, -0.25f, -2.0f)
	};
	const std::vector<glm::vec3> NormsB = {
		glm::vec3(0.0f, 0.0f, 1.0f),
		glm::vec3(0.6f, 0.8f, 0.0f)
	};
	const glm::mat4x4 InvBindB0 = glm::translate(glm::vec3(1.0f, -0.75f, 2.0f)) * RotationDegrees(-63.0f, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4x4 InvBindB1 = glm::translate(glm::vec3(-0.25f, 0.5f, -1.0f)) * RotationDegrees(29.0f, glm::vec3(1.0f, 1.0f, 0.0f));

	sAnimatedMesh MeshA = MakeMesh(VertsA, NormsA);
	MeshA.UseMeshBindings = true;
	MeshA.MeshBindings.push_back(MakeBinding(0, InvBindA0, { 0, 1 }, { 0.4f, 1.0f }));
	MeshA.MeshBindings.push_back(MakeBinding(1, InvBindA1, { 0, 2 }, { 0.6f, 1.0f }));
	Model.AddMesh(MeshA);

	sAnimatedMesh MeshB = MakeMesh(VertsB, NormsB);
	MeshB.UseMeshBindings = true;
	MeshB.MeshBindings.push_back(MakeBinding(0, InvBindB0, { 0 }, { 1.0f }));
	MeshB.MeshBindings.push_back(MakeBinding(1, InvBindB1, { 1 }, { 1.0f }));
	Model.AddMesh(MeshB);

	TASK_CHECK(Model.GetNumMeshes() == 2);
	TASK_CHECK(Model.GetMesh(0).NumVertices == 3);
	TASK_CHECK(Model.GetMesh(1).NumVertices == 2);

	Model.Update(0.0f);

	const sAnimatedMesh& OutA = Model.GetMesh(0);
	const sAnimatedMesh& OutB = Model.GetMesh(1);

	// Reference: independent accumulation of the two bone matrices.
	const glm::mat4x4 SkinA0 = F.SkinMatrix(false, InvBindA0);
	const glm::mat4x4 SkinA1 = F.SkinMatrix(true, InvBindA1);
	const glm::mat4x4 SkinB0 = F.SkinMatrix(false, InvBindB0);
	const glm::mat4x4 SkinB1 = F.SkinMatrix(true, InvBindB1);

	glm::vec3 RefA0 = XformPoint(SkinA0, VertsA[0]) * 0.4f + XformPoint(SkinA1, VertsA[0]) * 0.6f;
	glm::vec3 RefN0 = XformNormal(SkinA0, NormsA[0]) * 0.4f + XformNormal(SkinA1, NormsA[0]) * 0.6f;
	TASK_CHECK(NearVec(OutA.pTransformedVertices[0], RefA0));
	TASK_CHECK(NearVec(OutA.pTransformedNormals[0], SafeNormalize(RefN0)));

	TASK_CHECK(NearVec(OutA.pTransformedVertices[1], XformPoint(SkinA0, VertsA[1])));
	TASK_CHECK(NearVec(OutA.pTransformedNormals[1], SafeNormalize(XformNormal(SkinA0, NormsA[1]))));
	TASK_CHECK(NearVec(OutA.pTransformedVertices[2], XformPoint(SkinA1, VertsA[2])));
	TASK_CHECK(NearVec(OutA.pTransformedNormals[2], SafeNormalize(XformNormal(SkinA1, NormsA[2]))));

	TASK_CHECK(NearVec(OutB.pTransformedVertices[0], XformPoint(SkinB0, VertsB[0])));
	TASK_CHECK(NearVec(OutB.pTransformedNormals[0], SafeNormalize(XformNormal(SkinB0, NormsB[0]))));
	TASK_CHECK(NearVec(OutB.pTransformedVertices[1], XformPoint(SkinB1, VertsB[1])));
	TASK_CHECK(NearVec(OutB.pTransformedNormals[1], SafeNormalize(XformNormal(SkinB1, NormsB[1]))));

	// Order matters: A and B's results differ, so a "last mesh wins" or
	// "first mesh wins" implementation cannot pass both.
	TASK_CHECK(!NearVec(OutA.pTransformedVertices[0], OutB.pTransformedVertices[0]));
	TASK_CHECK(!NearVec(OutA.pTransformedVertices[1], OutB.pTransformedVertices[1]));

	return 0;
}

// The hierarchy pose must not include any mesh's inverse bind. A child of the
// root whose own child binding is arbitrary still lands at parent * child.
static int Palettes_HierarchyIndependentOfBindings()
{
	Fixture F;
	SkeletalModel Model;
	AddBone(Model, MakeBone("root", F.ParentLocal, 1));
	AddBone(Model, MakeBone("child", F.ChildLocal));
	Model.SetGlobalInverseTransform(F.GlobalInverse);
	SetAnimation(Model);

	const glm::mat4x4 WeirdInvBind = glm::translate(glm::vec3(9.0f, -4.0f, 7.0f)) * RotationDegrees(83.0f, glm::vec3(0.0f, 1.0f, 0.0f));

	sAnimatedMesh Mesh = MakeMesh({ glm::vec3(1.0f, 0.0f, 0.0f) }, { glm::vec3(0.0f, 1.0f, 0.0f) });
	Mesh.UseMeshBindings = true;
	Mesh.MeshBindings.push_back(MakeBinding(1, WeirdInvBind, { 0 }, { 1.0f }));
	Model.AddMesh(Mesh);

	Model.Update(0.0f);

	// The expected position uses inverseBind exactly once, at the mesh level.
	glm::vec3 Expected = XformPoint(F.SkinMatrix(true, WeirdInvBind), glm::vec3(1.0f, 0.0f, 0.0f));
	TASK_CHECK(NearVec(Model.GetMesh(0).pTransformedVertices[0], Expected));

	// The bone final transform (legacy field) is based on hierarchy only and
	// its own OffsetMatrix, identical regardless of any mesh binding.
	TASK_CHECK(NearMat(Model.GetSkeleton().Bones[1].FinalTransformation,
		F.GlobalInverse * F.ChildGlobal() * Model.GetSkeleton().Bones[1].OffsetMatrix));

	return 0;
}

// Weights are non-negative and sum to 1; normals are normalized even when the
// blended normal is long or short.
static int Palettes_WeightedNormalNormalization()
{
	Fixture F;
	SkeletalModel Model;
	AddBone(Model, MakeBone("root", F.ParentLocal, 1));
	AddBone(Model, MakeBone("child", F.ChildLocal));
	Model.SetGlobalInverseTransform(F.GlobalInverse);
	SetAnimation(Model);

	glm::vec3 Vertex(2.0f, -1.0f, 0.5f);
	glm::vec3 Normal(0.0f, 0.0f, 1.0f);

	sAnimatedMesh Mesh = MakeMesh({ Vertex }, { Normal });
	Mesh.UseMeshBindings = true;
	Mesh.MeshBindings.push_back(MakeBinding(0, glm::mat4x4(1.0f), { 0 }, { 0.25f }));
	Mesh.MeshBindings.push_back(MakeBinding(1, glm::mat4x4(1.0f), { 0 }, { 0.75f }));
	Model.AddMesh(Mesh);

	Model.Update(0.0f);

	const glm::vec3& OutNormal = Model.GetMesh(0).pTransformedNormals[0];
	TASK_CHECK(Near(glm::length(OutNormal), 1.0f));

	glm::vec3 Expected = XformPoint(F.SkinMatrix(false, glm::mat4x4(1.0f)), Vertex) * 0.25f
		+ XformPoint(F.SkinMatrix(true, glm::mat4x4(1.0f)), Vertex) * 0.75f;
	TASK_CHECK(NearVec(Model.GetMesh(0).pTransformedVertices[0], Expected));

	return 0;
}

//////////////////////////////////////////////////////////////////////////////
// assets: static meshes, unweighted vertices, legacy single mesh
//////////////////////////////////////////////////////////////////////////////

// An empty binding table means a static mesh: positions and unit normals are
// copied through untouched.
static int Assets_EmptyBindingsAreStatic()
{
	SkeletalModel Model;
	AddBone(Model, MakeBone("root", glm::translate(glm::vec3(5.0f, 0.0f, 0.0f))));
	Model.SetGlobalInverseTransform(glm::mat4x4(1.0f));
	SetAnimation(Model);

	const std::vector<glm::vec3> Verts = {
		glm::vec3(1.0f, 2.0f, 3.0f),
		glm::vec3(-4.0f, 0.5f, 2.0f)
	};
	// Second normal is deliberately not unit length; the output must be unit.
	const std::vector<glm::vec3> Norms = {
		glm::vec3(0.0f, 1.0f, 0.0f),
		glm::vec3(0.0f, 3.0f, 4.0f)
	};

	sAnimatedMesh Mesh = MakeMesh(Verts, Norms);
	Mesh.UseMeshBindings = true; // table empty
	Model.AddMesh(Mesh);

	Model.Update(0.0f);

	TASK_CHECK(Model.GetMesh(0).MeshBindings.empty());
	TASK_CHECK(NearVec(Model.GetMesh(0).pTransformedVertices[0], Verts[0]));
	TASK_CHECK(NearVec(Model.GetMesh(0).pTransformedVertices[1], Verts[1]));
	TASK_CHECK(NearVec(Model.GetMesh(0).pTransformedNormals[0], glm::vec3(0.0f, 1.0f, 0.0f)));
	TASK_CHECK(NearVec(Model.GetMesh(0).pTransformedNormals[1], glm::vec3(0.0f, 0.6f, 0.8f)));
	TASK_CHECK(Near(glm::length(Model.GetMesh(0).pTransformedNormals[1]), 1.0f));

	return 0;
}

// Vertices with no weights at all keep their original position and unit normal.
static int Assets_UnweightedVerticesKeepValues()
{
	Fixture F;
	SkeletalModel Model;
	AddBone(Model, MakeBone("root", F.ParentLocal));
	Model.SetGlobalInverseTransform(F.GlobalInverse);
	SetAnimation(Model);

	const std::vector<glm::vec3> Verts = {
		glm::vec3(1.0f, 1.0f, 1.0f),
		glm::vec3(-3.0f, 2.0f, 0.25f),
		glm::vec3(0.5f, -1.0f, 4.0f)
	};
	const std::vector<glm::vec3> Norms = {
		glm::vec3(0.0f, 1.0f, 0.0f),
		glm::vec3(0.0f, 0.0f, 2.0f),
		glm::vec3(1.0f, 0.0f, 0.0f)
	};

	sAnimatedMesh Mesh = MakeMesh(Verts, Norms);
	Mesh.UseMeshBindings = true;
	// Only vertex 0 is weighted.
	Mesh.MeshBindings.push_back(MakeBinding(0, glm::mat4x4(1.0f), { 0 }, { 1.0f }));
	Model.AddMesh(Mesh);

	Model.Update(0.0f);

	const sAnimatedMesh& Out = Model.GetMesh(0);
	TASK_CHECK(NearVec(Out.pTransformedVertices[0], XformPoint(F.SkinMatrix(false, glm::mat4x4(1.0f)), Verts[0])));

	TASK_CHECK(NearVec(Out.pTransformedVertices[1], Verts[1]));
	TASK_CHECK(NearVec(Out.pTransformedNormals[1], glm::vec3(0.0f, 0.0f, 1.0f)));
	TASK_CHECK(NearVec(Out.pTransformedVertices[2], Verts[2]));
	TASK_CHECK(NearVec(Out.pTransformedNormals[2], glm::vec3(1.0f, 0.0f, 0.0f)));

	return 0;
}

// The original single-mesh API (weights and OffsetMatrix on sBone, no
// UseMeshBindings) keeps its exact behaviour, including FinalTransformation.
static int Assets_LegacySingleMeshStillWorks()
{
	SkeletalModel Model;

	sBone Root = MakeBone("root", glm::translate(glm::vec3(2.0f, 0.0f, 0.0f)));
	Root.NumWeights = 1;
	Root.pWeights = new sWeight[1];
	Root.pWeights[0].VertexID = 0;
	Root.pWeights[0].Weight = 1.0f;
	AddBone(Model, Root);

	Model.SetGlobalInverseTransform(glm::mat4x4(1.0f));
	SetAnimation(Model);

	sAnimatedMesh Mesh = MakeMesh({ glm::vec3(1.0f, 0.0f, 0.0f) }, { glm::vec3(0.0f, 1.0f, 0.0f) });
	Mesh.NumIndices = 0;
	Mesh.pIndices = NULL;
	Model.AddMesh(Mesh);

	TASK_CHECK(!Model.GetMesh(0).UseMeshBindings);
	Model.Update(0.25f);

	const sAnimatedMesh& Out = Model.GetMesh(0);
	TASK_CHECK(NearVec(Out.pTransformedVertices[0], glm::vec3(3.0f, 0.0f, 0.0f)));
	TASK_CHECK(NearVec(Out.pTransformedNormals[0], glm::vec3(0.0f, 1.0f, 0.0f)));
	TASK_CHECK(NearMat(Model.GetSkeleton().Bones[0].FinalTransformation,
		Model.GetGlobalInverseTransform() * glm::translate(glm::vec3(2.0f, 0.0f, 0.0f)) * Model.GetSkeleton().Bones[0].OffsetMatrix));

	return 0;
}

// The legacy asset written by the old writer must still load and animate, with
// no extra characters appended to bone or animation names.
static int Assets_LoadLegacyAssetAndAnimate()
{
	FILE* pFile = std::fopen("task_support/legacy.sam", "rb");
	TASK_CHECK(pFile != NULL);
	std::fseek(pFile, 0, SEEK_END);
	long Size = std::ftell(pFile);
	std::fseek(pFile, 0, SEEK_SET);
	TASK_CHECK(Size > 0);

	std::vector<char> Blob((size_t)Size);
	TASK_CHECK(std::fread(&Blob[0], 1, (size_t)Size, pFile) == (size_t)Size);
	std::fclose(pFile);

	SkeletalModel Model;
	TASK_CHECK(DataToModel(&Blob[0], Model));

	TASK_CHECK(Model.GetSkeleton().Bones.size() == 1);
	TASK_CHECK(Model.GetSkeleton().Bones[0].Name == "root");
	TASK_CHECK(Model.GetSkeleton().Bones[0].Name.size() == 4);
	TASK_CHECK(Model.GetNumMeshes() == 1);
	TASK_CHECK(Model.GetAnimation().NodeAnimations.empty());

	// Legacy single-mesh data is unambiguous and becomes mesh-local bindings.
	TASK_CHECK(Model.GetMesh(0).UseMeshBindings);
	TASK_CHECK(Model.GetMesh(0).MeshBindings.size() == 1);

	Model.Update(0.25f);
	TASK_CHECK(NearVec(Model.GetMesh(0).pTransformedVertices[0], glm::vec3(3.0f, 0.0f, 0.0f)));
	TASK_CHECK(NearVec(Model.GetMesh(0).pTransformedNormals[0], glm::vec3(0.0f, 1.0f, 0.0f)));

	return 0;
}

// Round-tripping the legacy single-mesh representation through the new format:
// the bone-level weights survive untouched and the asset keeps animating.
static int Assets_LegacyRepresentationRoundTrips()
{
	SkeletalModel Model;
	sBone Root = MakeBone("tool_bone", glm::translate(glm::vec3(0.0f, 3.0f, 0.0f)));
	Root.OffsetMatrix = glm::translate(glm::vec3(0.0f, -1.0f, 0.0f));
	Root.NumWeights = 1;
	Root.pWeights = new sWeight[1];
	Root.pWeights[0].VertexID = 0;
	Root.pWeights[0].Weight = 1.0f;
	AddBone(Model, Root);
	Model.SetGlobalInverseTransform(glm::mat4x4(1.0f));
	SetAnimation(Model);

	sAnimatedMesh Mesh = MakeMesh({ glm::vec3(1.0f, 0.0f, 0.0f) }, { glm::vec3(0.0f, 1.0f, 0.0f) });
	Model.AddMesh(Mesh);

	Model.Update(0.0f);
	glm::vec3 Expected = Model.GetMesh(0).pTransformedVertices[0];
	TASK_CHECK(NearVec(Expected, glm::vec3(1.0f, 2.0f, 0.0f)));

	std::string Blob = ModelToData(Model);
	SkeletalModel Loaded;
	TASK_CHECK(DataToModel(Blob.data(), Loaded));
	TASK_CHECK(!Loaded.GetMesh(0).UseMeshBindings);
	TASK_CHECK(Loaded.GetSkeleton().Bones[0].NumWeights == 1);
	TASK_CHECK(Loaded.GetSkeleton().Bones[0].pWeights[0].VertexID == 0);
	TASK_CHECK(Loaded.GetSkeleton().Bones[0].Name == "tool_bone");
	Loaded.Update(0.0f);
	TASK_CHECK(NearVec(Loaded.GetMesh(0).pTransformedVertices[0], Expected));

	return 0;
}

// Node animation names round-trip exactly, with no trailing characters.
static int Assets_AnimationNamesAreExact()
{
	SkeletalModel Model;
	AddBone(Model, MakeBone("root", glm::mat4x4(1.0f)));
	SetAnimation(Model);

	sNodeAnimation Anim;
	Anim.Name = "swing";
	sNodeAnimationKey<glm::vec3> PosKey;
	PosKey.Time = 0.0f;
	PosKey.Value = glm::vec3(1.0f, 2.0f, 3.0f);
	Anim.PositionKeys.push_back(PosKey);
	sNodeAnimationKey<glm::quat> RotKey;
	RotKey.Time = 0.0f;
	RotKey.Value = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	Anim.RotationKeys.push_back(RotKey);
	Model.GetAnimation().NodeAnimations.push_back(Anim);

	std::string Blob = ModelToData(Model);
	SkeletalModel Loaded;
	TASK_CHECK(DataToModel(Blob.data(), Loaded));
	TASK_CHECK(Loaded.GetAnimation().NodeAnimations.size() == 1);
	TASK_CHECK(Loaded.GetAnimation().NodeAnimations[0].Name == "swing");
	TASK_CHECK(Loaded.GetAnimation().NodeAnimations[0].Name.size() == 5);
	TASK_CHECK(Loaded.GetAnimation().NodeAnimations[0].PositionKeys.size() == 1);
	TASK_CHECK(Loaded.GetAnimation().NodeAnimations[0].RotationKeys.size() == 1);

	return 0;
}

//////////////////////////////////////////////////////////////////////////////
// lifecycle: serialization round-trips, clearing and isolation
//////////////////////////////////////////////////////////////////////////////

// Builds a two-mesh model with mesh-local bindings, distinct vertex counts and
// a hierarchy with a child bone.
static void BuildTwoMeshModel(SkeletalModel& a_Model)
{
	Fixture F;
	AddBone(a_Model, MakeBone("root", F.ParentLocal, 1));
	AddBone(a_Model, MakeBone("child", F.ChildLocal));
	a_Model.SetGlobalInverseTransform(F.GlobalInverse);
	SetAnimation(a_Model);

	const glm::mat4x4 InvA = glm::translate(glm::vec3(0.25f, 0.5f, -0.75f)) * RotationDegrees(33.0f, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4x4 InvB = glm::translate(glm::vec3(-1.0f, 2.0f, 0.0f)) * RotationDegrees(-19.0f, glm::vec3(1.0f, 0.0f, 0.0f));

	sAnimatedMesh Mesh0 = MakeMesh({
		glm::vec3(1.0f, 0.0f, 0.0f),
		glm::vec3(0.0f, 2.0f, 0.0f),
		glm::vec3(0.0f, 0.0f, 3.0f)
	}, {
		glm::vec3(0.0f, 1.0f, 0.0f),
		glm::vec3(1.0f, 0.0f, 0.0f),
		glm::vec3(0.0f, 0.0f, -1.0f)
	});
	Mesh0.UseMeshBindings = true;
	Mesh0.MeshBindings.push_back(MakeBinding(0, InvA, { 0 }, { 1.0f }));
	Mesh0.MeshBindings.push_back(MakeBinding(1, InvB, { 1, 2 }, { 0.5f, 0.5f }));
	a_Model.AddMesh(Mesh0);

	// Second mesh has a different vertex count and an empty binding table.
	sAnimatedMesh Mesh1 = MakeMesh({
		glm::vec3(-1.0f, 1.0f, 2.0f),
		glm::vec3(4.0f, -2.0f, 0.5f)
	}, {
		glm::vec3(0.0f, 0.0f, 1.0f),
		glm::vec3(1.0f, 0.0f, 0.0f)
	});
	Mesh1.UseMeshBindings = true;
	a_Model.AddMesh(Mesh1);
}

static int Lifecycle_ByteRoundTripContinuesAnimation()
{
	SkeletalModel Model;
	BuildTwoMeshModel(Model);

	Model.Update(0.0f);
	// Snapshot expectations before serializing.
	const glm::vec3 Expected0 = Model.GetMesh(0).pTransformedVertices[1];
	const glm::vec3 Expected0N = Model.GetMesh(0).pTransformedNormals[2];
	const glm::vec3 Expected1 = Model.GetMesh(1).pTransformedVertices[0];

	std::string Blob = ModelToData(Model);
	TASK_CHECK(Blob.size() > 0);
	// New writes carry an explicit magic/version marker.
	TASK_CHECK(Blob.size() >= 8);
	TASK_CHECK((unsigned char)Blob[4] == 2); // version word, little-endian

	SkeletalModel Loaded;
	TASK_CHECK(DataToModel(Blob.data(), Loaded));

	TASK_CHECK(Loaded.GetNumMeshes() == 2);
	TASK_CHECK(Loaded.GetSkeleton().Bones.size() == 2);
	TASK_CHECK(Loaded.GetMesh(0).NumVertices == 3);
	TASK_CHECK(Loaded.GetMesh(1).NumVertices == 2);
	TASK_CHECK(Loaded.GetMesh(0).UseMeshBindings);
	TASK_CHECK(Loaded.GetMesh(0).MeshBindings.size() == 2);
	TASK_CHECK(Loaded.GetMesh(1).UseMeshBindings);
	TASK_CHECK(Loaded.GetMesh(1).MeshBindings.empty());

	Loaded.Update(0.0f);
	TASK_CHECK(NearVec(Loaded.GetMesh(0).pTransformedVertices[1], Expected0));
	TASK_CHECK(NearVec(Loaded.GetMesh(0).pTransformedNormals[2], Expected0N));
	TASK_CHECK(NearVec(Loaded.GetMesh(1).pTransformedVertices[0], Expected1));

	// The loaded model owns its data: mutate the source blob and keep animating.
	for (size_t i = 0; i < Blob.size(); ++i)
		Blob[i] = (char)0x7F;
	Loaded.Update(0.5f);
	TASK_CHECK(NearVec(Loaded.GetMesh(1).pTransformedVertices[0], Expected1)); // static mesh

	return 0;
}

// Loading must not borrow the caller's binding container: destroy the source
// model after serializing and load from an independent copy of the bytes.
static int Lifecycle_LoadDoesNotBorrowInputs()
{
	std::string Blob;
	{
		SkeletalModel Source;
		BuildTwoMeshModel(Source);
		Blob = ModelToData(Source);
	}

	SkeletalModel Loaded;
	TASK_CHECK(DataToModel(Blob.data(), Loaded));

	// Clear the model the bytes came from is already gone; verify the loaded
	// bindings are usable and self-contained.
	TASK_CHECK(Loaded.GetMesh(0).MeshBindings.size() == 2);
	TASK_CHECK(Loaded.GetMesh(0).MeshBindings[0].Weights.size() == 1);
	TASK_CHECK(Loaded.GetMesh(0).MeshBindings[1].Weights.size() == 2);
	Loaded.Update(0.0f);
	TASK_CHECK(Near(glm::length(Loaded.GetMesh(0).pTransformedNormals[0]), 1.0f));

	return 0;
}

// Load, Clear, load a different asset, Clear, reuse: no stale bindings or
// bones survive.
static int Lifecycle_ClearThenReloadAndSwitchAssets()
{
	SkeletalModel Model;
	BuildTwoMeshModel(Model);
	std::string TwoMeshBlob = ModelToData(Model);
	TASK_CHECK(DataToModel(TwoMeshBlob.data(), Model));
	TASK_CHECK(Model.GetMesh(0).MeshBindings.size() == 2);

	Model.Clear();
	TASK_CHECK(Model.GetNumMeshes() == 0);
	TASK_CHECK(Model.GetSkeleton().Bones.empty());
	TASK_CHECK(Model.GetAnimation().NodeAnimations.empty());

	// Reuse the same object with a single-mesh legacy asset.
	FILE* pFile = std::fopen("task_support/legacy.sam", "rb");
	TASK_CHECK(pFile != NULL);
	std::fseek(pFile, 0, SEEK_END);
	long Size = std::ftell(pFile);
	std::fseek(pFile, 0, SEEK_SET);
	std::vector<char> Legacy((size_t)Size);
	TASK_CHECK(std::fread(&Legacy[0], 1, (size_t)Size, pFile) == (size_t)Size);
	std::fclose(pFile);

	TASK_CHECK(DataToModel(&Legacy[0], Model));
	TASK_CHECK(Model.GetNumMeshes() == 1);
	TASK_CHECK(Model.GetSkeleton().Bones.size() == 1);
	TASK_CHECK(Model.GetSkeleton().Bones[0].Name == "root");
	TASK_CHECK(Model.GetMesh(0).MeshBindings.size() == 1);
	Model.Update(0.0f);

	// Switch back to the two-mesh asset on the same object.
	TASK_CHECK(DataToModel(TwoMeshBlob.data(), Model));
	TASK_CHECK(Model.GetNumMeshes() == 2);
	TASK_CHECK(Model.GetSkeleton().Bones.size() == 2);
	TASK_CHECK(Model.GetMesh(0).MeshBindings.size() == 2);
	TASK_CHECK(Model.GetMesh(1).MeshBindings.empty());

	return 0;
}

// Updating one character must not affect another one's bindings.
static int Lifecycle_ModelsAreIndependent()
{
	SkeletalModel First;
	SkeletalModel Second;
	BuildTwoMeshModel(First);
	BuildTwoMeshModel(Second);

	// Give Second a different global inverse so its output differs.
	Second.SetGlobalInverseTransform(glm::translate(glm::vec3(10.0f, 0.0f, 0.0f)));

	First.Update(0.0f);
	glm::vec3 FirstBefore = First.GetMesh(0).pTransformedVertices[0];

	Second.Update(0.0f);
	TASK_CHECK(NearVec(First.GetMesh(0).pTransformedVertices[0], FirstBefore));

	// Replacing Second from data must not disturb First either.
	std::string Blob = ModelToData(First);
	TASK_CHECK(DataToModel(Blob.data(), Second));
	First.Update(0.0f);
	TASK_CHECK(NearVec(First.GetMesh(0).pTransformedVertices[0], FirstBefore));
	TASK_CHECK(Second.GetMesh(0).MeshBindings.size() == 2);

	return 0;
}

// Serializes a model in the original legacy layout (header words 1/2/3, no
// mesh-binding section). Used to prove that ambiguous old data is rejected.
namespace
{
	void PutBytes(std::string& a_Out, const void* a_pData, size_t a_Size)
	{
		a_Out.append((const char*)a_pData, a_Size);
	}

	template <typename _Ty>
	void Put(std::string& a_Out, const _Ty& a_Value)
	{
		PutBytes(a_Out, &a_Value, sizeof(_Ty));
	}

	std::string WriteLegacyBlob(const std::vector<glm::vec3>& a_MeshVerts, const glm::mat4x4& a_NodeTransform)
	{
		std::string Out;
		unsigned int Header[3] = { 1u, 2u, 3u };
		Put(Out, Header[0]);
		Put(Out, Header[1]);
		Put(Out, Header[2]);

		unsigned int NumMeshes = (unsigned int)a_MeshVerts.size();
		Put(Out, NumMeshes);
		for (unsigned int m = 0; m < NumMeshes; ++m)
		{
			unsigned int NumVertices = 1;
			Put(Out, NumVertices);
			glm::vec3 Vertex = a_MeshVerts[m];
			Put(Out, Vertex);
			glm::vec3 Normal(0.0f, 1.0f, 0.0f);
			Put(Out, Normal);
			unsigned int NumIndices = 0;
			Put(Out, NumIndices);
		}

		unsigned int NumBones = 1;
		Put(Out, NumBones);
		const char* Name = "root";
		unsigned int NameLength = 4;
		Put(Out, NameLength);
		PutBytes(Out, Name, NameLength);
		Put(Out, a_NodeTransform);
		glm::mat4x4 Offset(1.0f);
		Put(Out, Offset);
		unsigned int NumWeights = 1;
		Put(Out, NumWeights);
		unsigned int VertexId = 0;
		Put(Out, VertexId);
		float Weight = 1.0f;
		Put(Out, Weight);
		unsigned int NumChildren = 0;
		Put(Out, NumChildren);

		float TicksPerSecond = 1.0f;
		float Duration = 2.0f;
		Put(Out, TicksPerSecond);
		Put(Out, Duration);
		unsigned int NumNodeAnimations = 0;
		Put(Out, NumNodeAnimations);

		glm::mat4x4 GlobalInverse(1.0f);
		Put(Out, GlobalInverse);

		return Out;
	}
}

// A legacy multi-mesh blob has no way to say which mesh owned a bone's weights,
// so loading it must fail rather than guess, and must leave the target model
// untouched.
static int Lifecycle_AmbiguousLegacyMultiMeshRejected()
{
	std::string Blob = WriteLegacyBlob({ glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(2.0f, 0.0f, 0.0f) },
		glm::translate(glm::vec3(1.0f, 0.0f, 0.0f)));

	SkeletalModel Target;
	BuildTwoMeshModel(Target);
	unsigned int MeshesBefore = Target.GetNumMeshes();
	size_t BindingsBefore = Target.GetMesh(0).MeshBindings.size();

	TASK_CHECK(!DataToModel(Blob.data(), Target));

	// Failure must not corrupt the model that was already loaded.
	TASK_CHECK(Target.GetNumMeshes() == MeshesBefore);
	TASK_CHECK(Target.GetMesh(0).MeshBindings.size() == BindingsBefore);
	Target.Update(0.0f);

	return 0;
}

//////////////////////////////////////////////////////////////////////////////
// Registration
//////////////////////////////////////////////////////////////////////////////

const task_case task_cases[] = {
	{ "palettes",  "two_meshes_overlapping_ids",       Palettes_TwoMeshesOverlappingIds },
	{ "palettes",  "hierarchy_independent_of_bindings", Palettes_HierarchyIndependentOfBindings },
	{ "palettes",  "weighted_normal_normalization",    Palettes_WeightedNormalNormalization },
	{ "assets",    "empty_bindings_are_static",        Assets_EmptyBindingsAreStatic },
	{ "assets",    "unweighted_vertices_keep_values",  Assets_UnweightedVerticesKeepValues },
	{ "assets",    "legacy_single_mesh_still_works",   Assets_LegacySingleMeshStillWorks },
	{ "assets",    "load_legacy_asset_and_animate",    Assets_LoadLegacyAssetAndAnimate },
	{ "assets",    "legacy_representation_round_trips", Assets_LegacyRepresentationRoundTrips },
	{ "assets",    "animation_names_are_exact",        Assets_AnimationNamesAreExact },
	{ "lifecycle", "byte_round_trip_continues",        Lifecycle_ByteRoundTripContinuesAnimation },
	{ "lifecycle", "load_does_not_borrow_inputs",      Lifecycle_LoadDoesNotBorrowInputs },
	{ "lifecycle", "clear_then_reload_and_switch",     Lifecycle_ClearThenReloadAndSwitchAssets },
	{ "lifecycle", "models_are_independent",           Lifecycle_ModelsAreIndependent },
	{ "lifecycle", "ambiguous_legacy_multi_mesh_rejected", Lifecycle_AmbiguousLegacyMultiMeshRejected }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
