// Regression tests for mesh-local bone bindings, CPU skinning and serialization.
//
// The numeric expectations below are computed independently of SkeletalModel:
// we re-derive every global pose and skinning matrix by multiplying the local
// node transforms ourselves, then compare against the buffers the model fills.

#include "task_support/check.h"

#include "SkeletalAnimation/SkeletalModel.h"
#include "SkeletalAnimation/SkeletalModelSerializer.h"

#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	using SA::sAnimatedMesh;
	using SA::sBone;
	using SA::sMeshBoneBinding;
	using SA::sNodeAnimation;
	using SA::sNodeAnimationKey;
	using SA::sWeight;
	using SA::SkeletalModel;

	// ---------------------------------------------------------------------
	// Small helpers
	// ---------------------------------------------------------------------

	static bool Near(float a_A, float a_B, float a_Epsilon = 1e-4f)
	{
		return std::fabs(a_A - a_B) <= a_Epsilon;
	}

	static bool Near(const glm::vec3& a_A, const glm::vec3& a_B, float a_Epsilon = 1e-4f)
	{
		return Near(a_A.x, a_B.x, a_Epsilon) &&
		       Near(a_A.y, a_B.y, a_Epsilon) &&
		       Near(a_A.z, a_B.z, a_Epsilon);
	}

	static bool Near(const glm::vec4& a_A, const glm::vec4& a_B, float a_Epsilon = 1e-4f)
	{
		return Near(a_A.x, a_B.x, a_Epsilon) &&
		       Near(a_A.y, a_B.y, a_Epsilon) &&
		       Near(a_A.z, a_B.z, a_Epsilon) &&
		       Near(a_A.w, a_B.w, a_Epsilon);
	}

	static float Length(const glm::vec3& a_V)
	{
		return std::sqrt(glm::dot(a_V, a_V));
	}

	// This GLM build takes angles in degrees unless GLM_FORCE_RADIANS is set.
	static glm::mat4 Rotate(float a_Degrees, const glm::vec3& a_Axis)
	{
		return glm::rotate(glm::mat4(1.0f), a_Degrees, glm::normalize(a_Axis));
	}

	static glm::mat4 Translate(const glm::vec3& a_Offset)
	{
		return glm::translate(a_Offset);
	}

	// ---------------------------------------------------------------------
	// Rig description: two bones whose rotations do not commute, and two
	// meshes with overlapping vertex IDs but different bind matrices.
	// ---------------------------------------------------------------------

	struct Rig
	{
		glm::mat4 GlobalInverse;
		glm::mat4 RootLocal;
		glm::mat4 ChildLocal;

		glm::mat4 Mesh0Bone0Offset;
		glm::mat4 Mesh0Bone1Offset;
		glm::mat4 Mesh1Bone0Offset;
		glm::mat4 Mesh1Bone1Offset;
	};

	static Rig MakeRig()
	{
		Rig R;
		R.GlobalInverse = Translate(glm::vec3(-0.3f, 1.2f, 0.7f)) * Rotate(17.0f, glm::vec3(0.3f, 0.8f, 0.2f));
		R.RootLocal     = Translate(glm::vec3(1.0f, 0.5f, -2.0f)) * Rotate(35.0f, glm::vec3(0.0f, 1.0f, 0.0f));
		R.ChildLocal    = Rotate(-50.0f, glm::vec3(1.0f, 0.0f, 0.0f)) * Translate(glm::vec3(0.0f, 2.0f, 0.0f));

		R.Mesh0Bone0Offset = Translate(glm::vec3(-0.25f, 0.1f, 0.4f)) * Rotate(12.0f, glm::vec3(0.0f, 0.0f, 1.0f));
		R.Mesh0Bone1Offset = Translate(glm::vec3(0.6f, -0.2f, 0.0f)) * Rotate(-25.0f, glm::vec3(1.0f, 0.0f, 0.0f));
		R.Mesh1Bone0Offset = Translate(glm::vec3(1.1f, 0.3f, -0.5f)) * Rotate(80.0f, glm::vec3(0.0f, 1.0f, 0.0f));
		R.Mesh1Bone1Offset = Translate(glm::vec3(-0.4f, 0.7f, 0.2f)) * Rotate(5.0f, glm::vec3(1.0f, 1.0f, 0.0f));
		return R;
	}

	static void MakeVertexData(glm::vec3* a_pOut, const glm::vec3* a_pIn, unsigned int a_Count)
	{
		for (unsigned int i = 0; i < a_Count; ++i)
			a_pOut[i] = a_pIn[i];
	}

	static void AllocateMesh(sAnimatedMesh& a_Mesh, unsigned int a_NumVertices,
	                         const glm::vec3* a_pVertices, const glm::vec3* a_pNormals,
	                         unsigned int a_NumIndices = 0, const unsigned int* a_pIndices = NULL)
	{
		a_Mesh.NumVertices = a_NumVertices;
		a_Mesh.pVertices = new glm::vec3[a_NumVertices];
		a_Mesh.pNormals = new glm::vec3[a_NumVertices];
		a_Mesh.pTransformedVertices = new glm::vec3[a_NumVertices];
		a_Mesh.pTransformedNormals = new glm::vec3[a_NumVertices];
		MakeVertexData(a_Mesh.pVertices, a_pVertices, a_NumVertices);
		MakeVertexData(a_Mesh.pNormals, a_pNormals, a_NumVertices);

		a_Mesh.NumIndices = a_NumIndices;
		if (a_NumIndices > 0)
		{
			a_Mesh.pIndices = new unsigned int[a_NumIndices];
			for (unsigned int i = 0; i < a_NumIndices; ++i)
				a_Mesh.pIndices[i] = a_pIndices[i];
		}
		else
		{
			a_Mesh.pIndices = NULL;
		}
	}

	static sMeshBoneBinding MakeBinding(unsigned int a_BoneIndex, const glm::mat4& a_Offset,
	                                    const std::vector<sWeight>& a_Weights)
	{
		sMeshBoneBinding Binding;
		Binding.BoneIndex = a_BoneIndex;
		Binding.OffsetMatrix = a_Offset;
		Binding.Weights = a_Weights;
		return Binding;
	}

	static sWeight W(unsigned int a_Vertex, float a_Weight)
	{
		sWeight Weight;
		Weight.VertexID = a_Vertex;
		Weight.Weight = a_Weight;
		return Weight;
	}

	static void AddRootAndChild(SkeletalModel& a_Model, const Rig& a_Rig)
	{
		sBone Root;
		Root.Name = "root";
		Root.NodeTransform = a_Rig.RootLocal;
		Root.NumChildren = 1;
		Root.pChildren = new unsigned int[1];
		Root.pChildren[0] = 1;

		sBone Child;
		Child.Name = "child";
		Child.NodeTransform = a_Rig.ChildLocal;
		Child.NumChildren = 0;
		Child.pChildren = NULL;

		a_Model.GetSkeleton().Bones.push_back(Root);
		a_Model.GetSkeleton().Bones.push_back(Child);
		a_Model.SetGlobalInverseTransform(a_Rig.GlobalInverse);
	}

	// Builds three meshes:
	//   mesh 0: 3 vertices, mesh-local bindings, vertex 2 unweighted
	//   mesh 1: 2 vertices, overlapping IDs but different offsets
	//   mesh 2: 4 vertices, static (UseMeshBindings with an empty table)
	static void BuildBoundModel(SkeletalModel& a_Model, const Rig& a_Rig)
	{
		AddRootAndChild(a_Model, a_Rig);

		static const glm::vec3 V0[3] = {
			glm::vec3(0.5f, 1.0f, -0.25f),
			glm::vec3(-1.0f, 0.3f, 0.75f),
			glm::vec3(0.25f, -0.5f, 1.5f)
		};
		static const glm::vec3 N0[3] = {
			glm::vec3(0.6f, 0.8f, 0.0f),
			glm::vec3(0.0f, 0.6f, 0.8f),
			glm::vec3(0.0f, 0.0f, 1.0f)
		};

		sAnimatedMesh Mesh0;
		AllocateMesh(Mesh0, 3, V0, N0);
		Mesh0.UseMeshBindings = true;
		Mesh0.MeshBindings.push_back(MakeBinding(0, a_Rig.Mesh0Bone0Offset,
			std::vector<sWeight>{ W(0, 0.7f), W(1, 1.0f) }));
		Mesh0.MeshBindings.push_back(MakeBinding(1, a_Rig.Mesh0Bone1Offset,
			std::vector<sWeight>{ W(0, 0.3f) }));
		a_Model.AddMesh(Mesh0);

		static const glm::vec3 V1[2] = {
			glm::vec3(1.0f, -0.5f, 0.3f),
			glm::vec3(-0.6f, 0.9f, 0.2f)
		};
		static const glm::vec3 N1[2] = {
			glm::vec3(0.8f, 0.0f, 0.6f),
			glm::vec3(0.0f, 1.0f, 0.0f)
		};

		sAnimatedMesh Mesh1;
		AllocateMesh(Mesh1, 2, V1, N1);
		Mesh1.UseMeshBindings = true;
		Mesh1.MeshBindings.push_back(MakeBinding(0, a_Rig.Mesh1Bone0Offset,
			std::vector<sWeight>{ W(0, 0.5f), W(1, 1.0f) }));
		Mesh1.MeshBindings.push_back(MakeBinding(1, a_Rig.Mesh1Bone1Offset,
			std::vector<sWeight>{ W(0, 0.5f) }));
		a_Model.AddMesh(Mesh1);

		static const glm::vec3 V2[4] = {
			glm::vec3(2.0f, 1.0f, 0.0f),
			glm::vec3(2.5f, 1.0f, 0.0f),
			glm::vec3(2.0f, 2.0f, 0.0f),
			glm::vec3(2.0f, 1.0f, 1.0f)
		};
		static const glm::vec3 N2[4] = {
			glm::vec3(0.0f, 0.0f, 1.0f),
			glm::vec3(0.0f, 0.0f, 1.0f),
			glm::vec3(0.0f, 0.0f, 1.0f),
			glm::vec3(0.0f, 1.0f, 0.0f)
		};

		sAnimatedMesh Mesh2;
		AllocateMesh(Mesh2, 4, V2, N2);
		Mesh2.UseMeshBindings = true; // static: table intentionally empty
		a_Model.AddMesh(Mesh2);

		a_Model.GetAnimation().TicksPerSecond = 1.0f;
		a_Model.GetAnimation().Duration = 2.0f;
	}

	// Adds animation tracks to the two-bone rig so serialization round trips
	// can be checked while animating.
	static void AddRigAnimation(SkeletalModel& a_Model)
	{
		sNodeAnimation RootTrack;
		RootTrack.Name = "root";
		RootTrack.PositionKeys.push_back(sNodeAnimationKey<glm::vec3>{ glm::vec3(1.0f, 0.5f, -2.0f), 0.0f });
		RootTrack.PositionKeys.push_back(sNodeAnimationKey<glm::vec3>{ glm::vec3(1.0f, 1.5f, -2.0f), 2.0f });
		RootTrack.RotationKeys.push_back(sNodeAnimationKey<glm::quat>{ glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 0.0f });
		RootTrack.RotationKeys.push_back(sNodeAnimationKey<glm::quat>{ glm::angleAxis(35.0f, glm::vec3(0.0f, 1.0f, 0.0f)), 2.0f });

		sNodeAnimation ChildTrack;
		ChildTrack.Name = "child";
		ChildTrack.PositionKeys.push_back(sNodeAnimationKey<glm::vec3>{ glm::vec3(0.0f, 2.0f, 0.0f), 0.0f });
		ChildTrack.PositionKeys.push_back(sNodeAnimationKey<glm::vec3>{ glm::vec3(0.0f, 1.0f, 0.0f), 2.0f });
		ChildTrack.RotationKeys.push_back(sNodeAnimationKey<glm::quat>{ glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 0.0f });
		ChildTrack.RotationKeys.push_back(sNodeAnimationKey<glm::quat>{ glm::angleAxis(-50.0f, glm::vec3(1.0f, 0.0f, 0.0f)), 2.0f });

		a_Model.GetAnimation().NodeAnimations.push_back(RootTrack);
		a_Model.GetAnimation().NodeAnimations.push_back(ChildTrack);
	}

	// Independently evaluate the expected skinning for a mesh: derive the
	// global poses by multiplying local transforms, apply each binding's own
	// offset, accumulate positions/normals and normalize touched normals.
	static void EvaluateExpectedWithPoses(const Rig& a_Rig, const glm::mat4* a_Poses, unsigned int a_NumPoses,
	                                      const sAnimatedMesh& a_Mesh,
	                                      std::vector<glm::vec3>& a_OutPositions,
	                                      std::vector<glm::vec3>& a_OutNormals)
	{
		a_OutPositions.resize(a_Mesh.NumVertices);
		a_OutNormals.resize(a_Mesh.NumVertices);
		std::vector<unsigned char> Touched(a_Mesh.NumVertices, 0);
		for (unsigned int v = 0; v < a_Mesh.NumVertices; ++v)
		{
			a_OutPositions[v] = a_Mesh.pVertices[v];
			a_OutNormals[v] = a_Mesh.pNormals[v];
		}

		for (unsigned int b = 0; b < a_Mesh.MeshBindings.size(); ++b)
		{
			const sMeshBoneBinding& Binding = a_Mesh.MeshBindings[b];
			if (Binding.BoneIndex >= a_NumPoses)
				continue;
			glm::mat4 Transform = a_Rig.GlobalInverse * a_Poses[Binding.BoneIndex] * Binding.OffsetMatrix;
			glm::mat3x3 Rotation(Transform);

			for (unsigned int w = 0; w < Binding.Weights.size(); ++w)
			{
				const sWeight& Weight = Binding.Weights[w];
				if (!Touched[Weight.VertexID])
				{
					Touched[Weight.VertexID] = 1;
					a_OutPositions[Weight.VertexID] = glm::vec3(0.0f);
					a_OutNormals[Weight.VertexID] = glm::vec3(0.0f);
				}

				glm::vec3 Vertex = a_Mesh.pVertices[Weight.VertexID];
				glm::vec3 Normal = a_Mesh.pNormals[Weight.VertexID];
				a_OutPositions[Weight.VertexID] += glm::vec3(Transform * glm::vec4(Vertex, 1.0f)) * Weight.Weight;
				a_OutNormals[Weight.VertexID] += (Rotation * Normal) * Weight.Weight;
			}
		}

		for (unsigned int v = 0; v < a_Mesh.NumVertices; ++v)
		{
			if (Touched[v])
			{
				float NormalLength = Length(a_OutNormals[v]);
				if (NormalLength > 0.0f)
					a_OutNormals[v] /= NormalLength;
			}
		}
	}

	static void EvaluateExpected(const Rig& a_Rig, const sAnimatedMesh& a_Mesh,
	                             std::vector<glm::vec3>& a_OutPositions,
	                             std::vector<glm::vec3>& a_OutNormals)
	{
		const glm::mat4 Poses[2] = {
			a_Rig.RootLocal,
			a_Rig.RootLocal * a_Rig.ChildLocal
		};
		EvaluateExpectedWithPoses(a_Rig, Poses, 2, a_Mesh, a_OutPositions, a_OutNormals);
	}

	static bool CompareMesh(const Rig& a_Rig, const sAnimatedMesh& a_Mesh)
	{
		std::vector<glm::vec3> ExpectedPositions;
		std::vector<glm::vec3> ExpectedNormals;
		EvaluateExpected(a_Rig, a_Mesh, ExpectedPositions, ExpectedNormals);

		for (unsigned int v = 0; v < a_Mesh.NumVertices; ++v)
		{
			if (!Near(a_Mesh.pTransformedVertices[v], ExpectedPositions[v]))
				return false;
			if (!Near(a_Mesh.pTransformedNormals[v], ExpectedNormals[v]))
				return false;
		}
		return true;
	}

	static bool ReadFile(const char* a_Path, std::string& a_Out)
	{
		FILE* pFile = std::fopen(a_Path, "rb");
		if (!pFile)
			return false;
		std::fseek(pFile, 0, SEEK_END);
		long Size = std::ftell(pFile);
		std::fseek(pFile, 0, SEEK_SET);
		if (Size < 0)
		{
			std::fclose(pFile);
			return false;
		}
		a_Out.resize((size_t)Size);
		size_t Read = Size > 0 ? std::fread(&a_Out[0], 1, (size_t)Size, pFile) : 0;
		std::fclose(pFile);
		return Read == (size_t)Size;
	}

	// ---------------------------------------------------------------------
	// A tiny legacy-format writer used to exercise the old entry point with
	// names and an animation track.
	// ---------------------------------------------------------------------

	template <typename _Ty>
	static void Put(std::vector<char>& a_Out, const _Ty& a_Value)
	{
		const char* pBytes = reinterpret_cast<const char*>(&a_Value);
		a_Out.insert(a_Out.end(), pBytes, pBytes + sizeof(_Ty));
	}

	static void PutString(std::vector<char>& a_Out, const char* a_pText)
	{
		unsigned int Length = (unsigned int)std::strlen(a_pText);
		Put(a_Out, Length);
		a_Out.insert(a_Out.end(), a_pText, a_pText + Length);
	}

	static std::string BuildLegacySingleMesh()
	{
		std::vector<char> Bytes;

		// Legacy header
		Put<int>(Bytes, 1);
		Put<int>(Bytes, 2);
		Put<int>(Bytes, 3);

		// One mesh: one vertex, one normal, no indices
		Put<unsigned int>(Bytes, 1u);
		Put<unsigned int>(Bytes, 1u);
		glm::vec3 Vertex(1.0f, 0.0f, 0.0f);
		glm::vec3 Normal(0.0f, 1.0f, 0.0f);
		Put(Bytes, Vertex);
		Put(Bytes, Normal);
		Put<unsigned int>(Bytes, 0u); // NumIndices

		// One bone with a single weight, no children
		Put<unsigned int>(Bytes, 1u);
		PutString(Bytes, "root");
		glm::mat4 NodeTransform = Translate(glm::vec3(2.0f, 0.0f, 0.0f));
		glm::mat4 OffsetMatrix(1.0f);
		Put(Bytes, NodeTransform);
		Put(Bytes, OffsetMatrix);
		Put<unsigned int>(Bytes, 1u); // NumWeights
		Put<unsigned int>(Bytes, 0u); // VertexID
		Put<float>(Bytes, 1.0f);      // Weight
		Put<unsigned int>(Bytes, 0u); // NumChildren

		// Animation: one track named "track" that yields a translation of (2,0,0)
		Put<float>(Bytes, 1.0f); // TicksPerSecond
		Put<float>(Bytes, 2.0f); // Duration
		Put<unsigned int>(Bytes, 1u);
		PutString(Bytes, "track");
		Put<unsigned int>(Bytes, 2u); // PositionKeys
		Put<float>(Bytes, 0.0f);
		Put(Bytes, glm::vec3(2.0f, 0.0f, 0.0f));
		Put<float>(Bytes, 2.0f);
		Put(Bytes, glm::vec3(2.0f, 0.0f, 0.0f));
		Put<unsigned int>(Bytes, 1u); // RotationKeys
		Put<float>(Bytes, 0.0f);
		Put(Bytes, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

		Put(Bytes, glm::mat4(1.0f)); // GlobalInverseTransform

		return std::string(Bytes.begin(), Bytes.end());
	}

	// ---------------------------------------------------------------------
	// palettes
	// ---------------------------------------------------------------------

	static int CasePalettesTwoMeshes(void)
	{
		Rig R = MakeRig();
		SkeletalModel Model;
		BuildBoundModel(Model, R);
		Model.Update(0.0f);

		TASK_CHECK(Model.GetNumMeshes() == 3);
		TASK_CHECK(Model.GetMesh(0).NumVertices == 3);
		TASK_CHECK(Model.GetMesh(1).NumVertices == 2);
		TASK_CHECK(Model.GetMesh(0).NumVertices != Model.GetMesh(1).NumVertices);

		// Both skinned meshes must match our independent accumulation.
		TASK_CHECK(CompareMesh(R, Model.GetMesh(0)));
		TASK_CHECK(CompareMesh(R, Model.GetMesh(1)));

		// Overlapping vertex ID 0 in the two meshes must produce different
		// results because the meshes carry different bind matrices.
		TASK_CHECK(!Near(Model.GetMesh(0).pTransformedVertices[0],
		                 Model.GetMesh(1).pTransformedVertices[0]));

		// The blended normal must be unit length.
		TASK_CHECK(Near(Length(Model.GetMesh(0).pTransformedNormals[0]), 1.0f));

		// Vertex 2 of mesh 0 has no weights and keeps its original values.
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedVertices[2], Model.GetMesh(0).pVertices[2]));
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedNormals[2], Model.GetMesh(0).pNormals[2]));

		// The static mesh ignores the skeleton entirely.
		for (unsigned int v = 0; v < Model.GetMesh(2).NumVertices; ++v)
		{
			TASK_CHECK(Near(Model.GetMesh(2).pTransformedVertices[v], Model.GetMesh(2).pVertices[v]));
			TASK_CHECK(Near(Model.GetMesh(2).pTransformedNormals[v], Model.GetMesh(2).pNormals[v]));
			TASK_CHECK(Near(Length(Model.GetMesh(2).pTransformedNormals[v]), 1.0f));
		}

		return 0;
	}

	static int CasePalettesNonCommutingPoses(void)
	{
		Rig R = MakeRig();
		SkeletalModel Model;
		BuildBoundModel(Model, R);
		Model.Update(0.0f);

		// Chain order matters: swapping the two rotations must give a
		// different pose, which proves the model is not collapsing them.
		glm::mat4 Correct = R.RootLocal * R.ChildLocal;
		glm::mat4 Swapped = R.ChildLocal * R.RootLocal;
		TASK_CHECK(!Near(Correct[3], Swapped[3]));

		// Re-derive the child transform by hand for mesh 0, vertex 1 (bone 0)
		// and verify against the produced buffer.
		glm::mat4 T0 = R.GlobalInverse * R.RootLocal * R.Mesh0Bone0Offset;
		glm::vec3 Expected = glm::vec3(T0 * glm::vec4(Model.GetMesh(0).pVertices[1], 1.0f));
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedVertices[1], Expected));

		// Mesh 1, vertex 1 is fully weighted to binding 0, whose BoneIndex is 0
		// (the root), so only the root pose applies.
		glm::mat4 T1 = R.GlobalInverse * R.RootLocal * R.Mesh1Bone0Offset;
		glm::vec3 Expected1 = glm::vec3(T1 * glm::vec4(Model.GetMesh(1).pVertices[1], 1.0f));
		TASK_CHECK(Near(Model.GetMesh(1).pTransformedVertices[1], Expected1));

		// Mesh 0, vertex 0 blends bone 0 and bone 1. Verify the blend against
		// a hand-built mix (positions weighted, normals normalized).
		{
			glm::mat4 A = R.GlobalInverse * R.RootLocal * R.Mesh0Bone0Offset;
			glm::mat4 B = R.GlobalInverse * (R.RootLocal * R.ChildLocal) * R.Mesh0Bone1Offset;
			glm::vec3 V = Model.GetMesh(0).pVertices[0];
			glm::vec3 N = Model.GetMesh(0).pNormals[0];
			glm::vec3 P = glm::vec3(A * glm::vec4(V, 1.0f)) * 0.7f +
			              glm::vec3(B * glm::vec4(V, 1.0f)) * 0.3f;
			glm::vec3 Nn = glm::vec3(glm::mat3x3(A) * N) * 0.7f +
			               glm::vec3(glm::mat3x3(B) * N) * 0.3f;
			Nn /= Length(Nn);
			TASK_CHECK(Near(Model.GetMesh(0).pTransformedVertices[0], P));
			TASK_CHECK(Near(Model.GetMesh(0).pTransformedNormals[0], Nn));
		}

		return 0;
	}

	static int CasePalettesLegacySingleMesh(void)
	{
		// Old-style programmatic single mesh: weights live on the bone.
		Rig R = MakeRig();
		SkeletalModel Model;
		AddRootAndChild(Model, R);

		sBone& Root = Model.GetSkeleton().Bones[0];
		Root.OffsetMatrix = R.Mesh0Bone0Offset;
		Root.NumWeights = 1;
		Root.pWeights = new sWeight[1];
		Root.pWeights[0] = W(0, 1.0f);

		static const glm::vec3 V[1] = { glm::vec3(0.5f, 1.0f, -0.25f) };
		static const glm::vec3 N[1] = { glm::vec3(0.6f, 0.8f, 0.0f) };

		sAnimatedMesh Mesh;
		AllocateMesh(Mesh, 1, V, N);
		Mesh.UseMeshBindings = false;
		Model.AddMesh(Mesh);

		Model.Update(0.0f);

		// FinalTransformation keeps its legacy meaning:
		// globalInverse * globalPose * bone.OffsetMatrix.
		glm::mat4 Final = R.GlobalInverse * R.RootLocal * R.Mesh0Bone0Offset;
		glm::vec3 Expected = glm::vec3(Final * glm::vec4(V[0], 1.0f));
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedVertices[0], Expected));
		TASK_CHECK(Near(Model.GetSkeleton().Bones[0].FinalTransformation[3], Final[3]));

		// A new-format round trip must preserve the legacy flag/weights and
		// keep producing the same result.
		std::string Blob = SA::ModelToData(Model);
		SkeletalModel Reloaded;
		TASK_CHECK(SA::DataToModel(Blob.data(), Reloaded));
		TASK_CHECK(!Reloaded.GetMesh(0).UseMeshBindings);
		TASK_CHECK(Reloaded.GetMesh(0).MeshBindings.empty());
		TASK_CHECK(Reloaded.GetSkeleton().Bones[0].NumWeights == 1);
		Reloaded.Update(0.0f);
		TASK_CHECK(Near(Reloaded.GetMesh(0).pTransformedVertices[0], Expected));
		TASK_CHECK(Near(Reloaded.GetSkeleton().Bones[0].FinalTransformation[3], Final[3]));

		return 0;
	}

	static int CasePalettesLegacyMultiMeshIndexGuard(void)
	{
		// Legacy weights use model-global vertex IDs. A weight that references
		// vertex 1 must not touch a mesh that only has vertex 0.
		Rig R = MakeRig();
		SkeletalModel Model;
		AddRootAndChild(Model, R);
		Model.GetSkeleton().Bones[0].OffsetMatrix = glm::mat4(1.0f);
		Model.GetSkeleton().Bones[0].NumWeights = 1;
		Model.GetSkeleton().Bones[0].pWeights = new sWeight[1];
		Model.GetSkeleton().Bones[0].pWeights[0] = W(1, 1.0f);

		static const glm::vec3 V0[1] = { glm::vec3(1.0f, 2.0f, 3.0f) };
		static const glm::vec3 N0[1] = { glm::vec3(0.0f, 1.0f, 0.0f) };
		static const glm::vec3 V1[2] = { glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(4.0f, 5.0f, 6.0f) };
		static const glm::vec3 N1[2] = { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f) };

		sAnimatedMesh Mesh0;
		AllocateMesh(Mesh0, 1, V0, N0);
		Model.AddMesh(Mesh0);
		sAnimatedMesh Mesh1;
		AllocateMesh(Mesh1, 2, V1, N1);
		Model.AddMesh(Mesh1);

		Model.Update(0.0f);

		// Mesh 0 has no vertex 1; its only vertex stays at the bind pose.
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedVertices[0], V0[0]));
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedNormals[0], N0[0]));

		// Mesh 1 vertex 1 is transformed by the root's legacy matrix.
		glm::mat4 Final = R.GlobalInverse * R.RootLocal;
		TASK_CHECK(Near(Model.GetMesh(1).pTransformedVertices[1],
			glm::vec3(Final * glm::vec4(V1[1], 1.0f))));
		TASK_CHECK(Near(Length(Model.GetMesh(1).pTransformedNormals[1]), 1.0f));

		return 0;
	}

	// ---------------------------------------------------------------------
	// assets
	// ---------------------------------------------------------------------

	static int CaseAssetsRoundTripBindings(void)
	{
		Rig R = MakeRig();
		SkeletalModel Original;
		BuildBoundModel(Original, R);
		AddRigAnimation(Original);

		// The new format announces its version in the first word.
		std::string Blob = SA::ModelToData(Original);
		TASK_CHECK(Blob.size() > 64);
		int Version = 0;
		std::memcpy(&Version, Blob.data(), sizeof(int));
		TASK_CHECK(Version == 2);

		// The three overloads agree on the size.
		std::string Blob2;
		TASK_CHECK(SA::ModelToData(Original, Blob2));
		TASK_CHECK(Blob2.size() == Blob.size());
		std::vector<char> Buffer(Blob.size() + 16, 0);
		unsigned int Written = SA::ModelToData(Original, &Buffer[0]);
		TASK_CHECK(Written == (unsigned int)Blob.size());
		TASK_CHECK(std::memcmp(&Buffer[0], Blob.data(), Blob.size()) == 0);

		SkeletalModel Loaded;
		TASK_CHECK(SA::DataToModel(Blob.data(), Loaded));
		TASK_CHECK(Loaded.GetNumMeshes() == 3);

		// Bindings, flags and static-empty state survive.
		TASK_CHECK(Loaded.GetMesh(0).UseMeshBindings);
		TASK_CHECK(Loaded.GetMesh(0).MeshBindings.size() == 2);
		TASK_CHECK(Loaded.GetMesh(1).UseMeshBindings);
		TASK_CHECK(Loaded.GetMesh(1).MeshBindings.size() == 2);
		TASK_CHECK(Loaded.GetMesh(2).UseMeshBindings);
		TASK_CHECK(Loaded.GetMesh(2).MeshBindings.empty());

		TASK_CHECK(Loaded.GetMesh(0).MeshBindings[0].BoneIndex == 0);
		TASK_CHECK(Loaded.GetMesh(0).MeshBindings[0].Weights.size() == 2);
		TASK_CHECK(Loaded.GetMesh(0).MeshBindings[0].Weights[0].VertexID == 0);
		TASK_CHECK(Near(Loaded.GetMesh(0).MeshBindings[0].Weights[0].Weight, 0.7f));
		TASK_CHECK(Loaded.GetMesh(0).MeshBindings[1].BoneIndex == 1);
		TASK_CHECK(Near(Loaded.GetMesh(0).MeshBindings[1].OffsetMatrix[3], R.Mesh0Bone1Offset[3]));

		// Names must be exact; no trailing characters may leak from the reader.
		TASK_CHECK(Loaded.GetSkeleton().Bones.size() == 2);
		TASK_CHECK(Loaded.GetSkeleton().Bones[0].Name == "root");
		TASK_CHECK(Loaded.GetSkeleton().Bones[0].Name.size() == 4);
		TASK_CHECK(Loaded.GetSkeleton().Bones[1].Name == "child");
		TASK_CHECK(Loaded.GetSkeleton().Bones[1].Name.size() == 5);
		TASK_CHECK(Loaded.GetAnimation().NodeAnimations.size() == 2);
		TASK_CHECK(Loaded.GetAnimation().NodeAnimations[0].Name == "root");
		TASK_CHECK(Loaded.GetAnimation().NodeAnimations[0].Name.size() == 4);
		TASK_CHECK(Loaded.GetAnimation().NodeAnimations[1].Name == "child");
		TASK_CHECK(Loaded.GetAnimation().NodeAnimations[1].Name.size() == 5);

		// Continue animating after the source bytes are destroyed.
		Original.Update(0.5f);
		Loaded.Update(0.5f);
		for (unsigned int m = 0; m < Loaded.GetNumMeshes(); ++m)
		{
			for (unsigned int v = 0; v < Loaded.GetMesh(m).NumVertices; ++v)
			{
				TASK_CHECK(Near(Loaded.GetMesh(m).pTransformedVertices[v],
					Original.GetMesh(m).pTransformedVertices[v]));
				TASK_CHECK(Near(Loaded.GetMesh(m).pTransformedNormals[v],
					Original.GetMesh(m).pTransformedNormals[v]));
			}
		}

		// The loaded model must not alias the caller's binding container.
		glm::vec3 Before = Loaded.GetMesh(0).pTransformedVertices[0];
		const std::vector<sMeshBoneBinding> Snapshot = Loaded.GetMesh(0).MeshBindings;
		Loaded.Update(0.0f);
		TASK_CHECK(Near(Loaded.GetMesh(0).pTransformedVertices[0], Before));
		TASK_CHECK(Loaded.GetMesh(0).MeshBindings.size() == Snapshot.size());
		TASK_CHECK(Loaded.GetMesh(0).MeshBindings[0].Weights[0].Weight == Snapshot[0].Weights[0].Weight);

		// Binding weights copied out of the blob must be independent of it.
		std::string Copy = Blob;
		SkeletalModel Independent;
		TASK_CHECK(SA::DataToModel(Copy.data(), Independent));
		std::memset(&Copy[0], 0, Copy.size());
		Independent.Update(0.0f);
		TASK_CHECK(Independent.GetMesh(0).MeshBindings.size() == 2);
		TASK_CHECK(Near(Independent.GetMesh(0).MeshBindings[1].OffsetMatrix[3],
			R.Mesh0Bone1Offset[3]));

		return 0;
	}

	static int CaseAssetsStaticEmptyTable(void)
	{
		Rig R = MakeRig();
		SkeletalModel Model;
		BuildBoundModel(Model, R);
		AddRigAnimation(Model);

		std::string Blob = SA::ModelToData(Model);
		SkeletalModel Loaded;
		TASK_CHECK(SA::DataToModel(Blob.data(), Loaded));

		// Move the loaded model; the static mesh must remain untouched.
		Loaded.Update(1.25f);
		for (unsigned int v = 0; v < Loaded.GetMesh(2).NumVertices; ++v)
		{
			TASK_CHECK(Near(Loaded.GetMesh(2).pTransformedVertices[v], Loaded.GetMesh(2).pVertices[v]));
			TASK_CHECK(Near(Loaded.GetMesh(2).pTransformedNormals[v], Loaded.GetMesh(2).pNormals[v]));
			TASK_CHECK(Near(Length(Loaded.GetMesh(2).pTransformedNormals[v]), 1.0f));
		}

		// A static model with no skeleton at all must also survive Update.
		SkeletalModel Static;
		static const glm::vec3 V[2] = { glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(-1.0f, 0.0f, 0.5f) };
		static const glm::vec3 N[2] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f) };
		sAnimatedMesh Mesh;
		AllocateMesh(Mesh, 2, V, N);
		Mesh.UseMeshBindings = true;
		Static.AddMesh(Mesh);
		Static.Update(0.5f);
		for (unsigned int v = 0; v < 2; ++v)
		{
			TASK_CHECK(Near(Static.GetMesh(0).pTransformedVertices[v], V[v]));
			TASK_CHECK(Near(Static.GetMesh(0).pTransformedNormals[v], N[v]));
		}

		return 0;
	}

	static int CaseAssetsLegacyFile(void)
	{
		std::string Bytes;
		TASK_CHECK(ReadFile("task_support/legacy.sam", Bytes));
		TASK_CHECK(Bytes.size() > 0);

		SkeletalModel Model;
		TASK_CHECK(SA::DataToModel(Bytes.data(), Model));
		TASK_CHECK(Model.GetNumMeshes() == 1);
		TASK_CHECK(Model.GetSkeleton().Bones.size() == 1);
		TASK_CHECK(Model.GetSkeleton().Bones[0].Name == "root");
		TASK_CHECK(Model.GetSkeleton().Bones[0].Name.size() == 4);
		TASK_CHECK(!Model.GetMesh(0).UseMeshBindings);
		TASK_CHECK(Model.GetMesh(0).MeshBindings.empty());

		Model.Update(0.25f);
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedVertices[0].x, 3.0f));
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedNormals[0].y, 1.0f));

		// Destroying the input bytes must not disturb the model.
		std::string().swap(Bytes);
		Model.Update(0.25f);
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedVertices[0].x, 3.0f));

		return 0;
	}

	static int CaseAssetsLegacyNamesAndAnimation(void)
	{
		std::string Bytes = BuildLegacySingleMesh();
		SkeletalModel Model;
		TASK_CHECK(SA::DataToModel(Bytes.data(), Model));

		TASK_CHECK(Model.GetSkeleton().Bones.size() == 1);
		TASK_CHECK(Model.GetSkeleton().Bones[0].Name == "root");
		TASK_CHECK(Model.GetSkeleton().Bones[0].Name.size() == 4);
		TASK_CHECK(Model.GetAnimation().NodeAnimations.size() == 1);
		TASK_CHECK(Model.GetAnimation().NodeAnimations[0].Name == "track");
		TASK_CHECK(Model.GetAnimation().NodeAnimations[0].Name.size() == 5);
		TASK_CHECK(Model.GetAnimation().NodeAnimations[0].PositionKeys.size() == 2);

		Model.Update(0.5f);
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedVertices[0].x, 3.0f));

		return 0;
	}

	static int CaseAssetsLegacyMultiMeshRejected(void)
	{
		std::vector<char> Bytes;
		Put<int>(Bytes, 1);
		Put<int>(Bytes, 2);
		Put<int>(Bytes, 3);
		Put<unsigned int>(Bytes, 2u); // two meshes, ambiguous weight ownership

		SkeletalModel Model;
		TASK_CHECK(!SA::DataToModel(&Bytes[0], Model));

		return 0;
	}

	// ---------------------------------------------------------------------
	// lifecycle
	// ---------------------------------------------------------------------

	static int CaseLifecycleClearThenReuse(void)
	{
		Rig R = MakeRig();
		SkeletalModel Model;
		BuildBoundModel(Model, R);
		Model.Update(0.0f);
		TASK_CHECK(Model.GetNumMeshes() == 3);
		TASK_CHECK(Model.GetSkeleton().Bones.size() == 2);

		Model.Clear();
		TASK_CHECK(Model.GetNumMeshes() == 0);
		TASK_CHECK(Model.GetSkeleton().Bones.size() == 0);
		TASK_CHECK(Model.GetAnimation().NodeAnimations.empty());

		// Load the legacy asset into the cleared model.
		std::string Bytes;
		TASK_CHECK(ReadFile("task_support/legacy.sam", Bytes));
		TASK_CHECK(SA::DataToModel(Bytes.data(), Model));
		TASK_CHECK(Model.GetNumMeshes() == 1);
		TASK_CHECK(!Model.GetMesh(0).UseMeshBindings);
		TASK_CHECK(Model.GetMesh(0).MeshBindings.empty());
		Model.Update(0.25f);
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedVertices[0].x, 3.0f));

		return 0;
	}

	static int CaseLifecycleSwitchAssets(void)
	{
		Rig R = MakeRig();
		SkeletalModel NewModel;
		BuildBoundModel(NewModel, R);
		AddRigAnimation(NewModel);
		std::string NewBlob = SA::ModelToData(NewModel);

		std::string LegacyBytes;
		TASK_CHECK(ReadFile("task_support/legacy.sam", LegacyBytes));

		SkeletalModel Model;

		// New -> legacy -> new on the same object.
		TASK_CHECK(SA::DataToModel(NewBlob.data(), Model));
		TASK_CHECK(Model.GetNumMeshes() == 3);
		TASK_CHECK(Model.GetMesh(0).UseMeshBindings);
		TASK_CHECK(Model.GetMesh(0).MeshBindings.size() == 2);

		TASK_CHECK(SA::DataToModel(LegacyBytes.data(), Model));
		TASK_CHECK(Model.GetNumMeshes() == 1);
		TASK_CHECK(!Model.GetMesh(0).UseMeshBindings);
		TASK_CHECK(Model.GetMesh(0).MeshBindings.empty());
		Model.Update(0.25f);
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedVertices[0].x, 3.0f));

		TASK_CHECK(SA::DataToModel(NewBlob.data(), Model));
		TASK_CHECK(Model.GetNumMeshes() == 3);
		TASK_CHECK(Model.GetMesh(0).UseMeshBindings);
		TASK_CHECK(Model.GetMesh(0).MeshBindings.size() == 2);
		TASK_CHECK(Model.GetMesh(2).MeshBindings.empty());
		Model.Update(0.5f);
		TASK_CHECK(Near(Length(Model.GetMesh(0).pTransformedNormals[0]), 1.0f));
		TASK_CHECK(Near(Length(Model.GetMesh(1).pTransformedNormals[0]), 1.0f));
		// Vertex 2 of mesh 0 is unweighted and still at the bind pose.
		TASK_CHECK(Near(Model.GetMesh(0).pTransformedVertices[2], Model.GetMesh(0).pVertices[2]));

		return 0;
	}

	static int CaseLifecycleIndependentModels(void)
	{
		Rig R = MakeRig();
		SkeletalModel Bound;
		BuildBoundModel(Bound, R);
		Bound.Update(0.0f);

		SkeletalModel Legacy;
		std::string Bytes;
		TASK_CHECK(ReadFile("task_support/legacy.sam", Bytes));
		TASK_CHECK(SA::DataToModel(Bytes.data(), Legacy));
		Legacy.Update(0.25f);

		glm::vec3 BoundBefore = Bound.GetMesh(0).pTransformedVertices[0];

		// Updating and reloading one character must not disturb the other.
		Legacy.Update(0.5f);
		std::string BoundBlob = SA::ModelToData(Bound);
		TASK_CHECK(SA::DataToModel(BoundBlob.data(), Legacy));

		TASK_CHECK(Near(Bound.GetMesh(0).pTransformedVertices[0], BoundBefore));
		TASK_CHECK(Legacy.GetNumMeshes() == 3);
		TASK_CHECK(Legacy.GetMesh(0).UseMeshBindings);
		TASK_CHECK(Legacy.GetMesh(0).MeshBindings.size() == 2);

		// Legacy now holds the same data as Bound. Bringing both to the same
		// animation time must yield identical skinned output.
		Legacy.Update(0.0f);
		for (unsigned int m = 0; m < Bound.GetNumMeshes(); ++m)
		{
			for (unsigned int v = 0; v < Bound.GetMesh(m).NumVertices; ++v)
			{
				TASK_CHECK(Near(Legacy.GetMesh(m).pTransformedVertices[v],
					Bound.GetMesh(m).pTransformedVertices[v]));
			}
		}

		// Reloading into Bound keeps its bindings and keeps producing results.
		TASK_CHECK(SA::DataToModel(BoundBlob.data(), Bound));
		Bound.Update(0.0f);
		TASK_CHECK(Near(Length(Bound.GetMesh(0).pTransformedNormals[0]), 1.0f));
		TASK_CHECK(Bound.GetMesh(0).MeshBindings.size() == 2);

		return 0;
	}

	// ---------------------------------------------------------------------
	// Registration
	// ---------------------------------------------------------------------
}

const struct task_case task_cases[] = {
	{ "palettes",  "two_meshes_shared_skeleton",    &CasePalettesTwoMeshes },
	{ "palettes",  "non_commuting_pose_chain",      &CasePalettesNonCommutingPoses },
	{ "palettes",  "legacy_single_mesh",            &CasePalettesLegacySingleMesh },
	{ "palettes",  "legacy_multi_mesh_index_guard", &CasePalettesLegacyMultiMeshIndexGuard },
	{ "assets",    "roundtrip_bindings",            &CaseAssetsRoundTripBindings },
	{ "assets",    "static_empty_table",            &CaseAssetsStaticEmptyTable },
	{ "assets",    "legacy_file",                   &CaseAssetsLegacyFile },
	{ "assets",    "legacy_names_and_animation",    &CaseAssetsLegacyNamesAndAnimation },
	{ "assets",    "legacy_multi_mesh_rejected",    &CaseAssetsLegacyMultiMeshRejected },
	{ "lifecycle", "clear_then_reuse",              &CaseLifecycleClearThenReuse },
	{ "lifecycle", "switch_assets",                 &CaseLifecycleSwitchAssets },
	{ "lifecycle", "independent_models",            &CaseLifecycleIndependentModels },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
