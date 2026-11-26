#pragma once

// GLM
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/transform.hpp>

// STD
#include <vector>
#include <string>

namespace SA
{
	struct sWeight
	{
		unsigned int VertexID;
		float Weight;
	};

	struct sBone
	{
		sBone()
			: NodeTransform(1.0f)
			, OffsetMatrix(1.0f)
			, GlobalTransformation(1.0f)
			, FinalTransformation(1.0f)
			, NumWeights(0)
			, pWeights(NULL)
			, NumChildren(0)
			, pChildren(NULL)
		{
		}

		std::string Name;

		glm::mat4x4 NodeTransform;
		glm::mat4x4 OffsetMatrix; // T-Pose to local bone space
		// GlobalTransformation is the hierarchy pose built only from node local
		// transforms and parent poses; it never includes an inverse bind matrix.
		glm::mat4x4 GlobalTransformation;
		glm::mat4x4 FinalTransformation;

		unsigned int NumWeights;
		sWeight* pWeights;

		unsigned int NumChildren;
		unsigned int* pChildren;
	};

	// Mesh-local binding of a bone: the vertex IDs in Weights index into the
	// mesh that owns this binding, and OffsetMatrix is the inverse bind matrix
	// for that mesh. This allows meshes sharing a skeleton to carry their own
	// vertex numbering and bind pose.
	struct sMeshBoneBinding
	{
		sMeshBoneBinding()
			: BoneIndex(0)
			, OffsetMatrix(1.0f)
		{
		}

		unsigned int BoneIndex;
		glm::mat4x4 OffsetMatrix;
		std::vector<sWeight> Weights; // owning container
	};

	struct sAnimatedMesh
	{
		sAnimatedMesh()
			: NumVertices(0)
			, pVertices(NULL)
			, pNormals(NULL)
			, pTransformedVertices(NULL)
			, pTransformedNormals(NULL)
			, NumIndices(0)
			, pIndices(NULL)
			, UseMeshBindings(false)
		{
		}

		unsigned int NumVertices;

		glm::vec3* pVertices;
		glm::vec3* pNormals;

		glm::vec3* pTransformedVertices;
		glm::vec3* pTransformedNormals;

		unsigned int NumIndices;
		unsigned int* pIndices;

		// When true, skinning uses only MeshBindings; vertex IDs in those
		// bindings are local to this mesh. An empty array means a static mesh.
		std::vector<sMeshBoneBinding> MeshBindings;
		bool UseMeshBindings;
	};

	struct sSkeleton
	{
		std::vector<sBone> Bones;
	};

	template <typename _Ty>
	struct sNodeAnimationKey
	{
		_Ty Value;
		float Time;
	};

	struct sNodeAnimation
	{
		std::string Name;

		std::vector<sNodeAnimationKey<glm::vec3> > PositionKeys;
		std::vector<sNodeAnimationKey<glm::quat> > RotationKeys;
	};

	struct sAnimation
	{
		std::vector<sNodeAnimation> NodeAnimations;

		float TicksPerSecond;
		float Duration;
	};
}