#include "SkeletalModel.h"

#include <cmath>
#include <cstring>

//////////////////////////////////////////////////////////////////////////
// Based on: http://ogldev.atspace.co.uk/www/tutorial38/tutorial38.html //
//////////////////////////////////////////////////////////////////////////

namespace SA
{
	SkeletalModel::SkeletalModel() 
		: m_AnimationTime(0.0f)
	{
		Clear();
	}





	SkeletalModel::~SkeletalModel()
	{
		Clear();
	}





	void SkeletalModel::Clear()
	{
		for (unsigned int i = 0; i < m_Meshes.size(); ++i)
		{
			delete[] m_Meshes[i].pVertices;
			delete[] m_Meshes[i].pNormals;
			delete[] m_Meshes[i].pTransformedVertices;
			delete[] m_Meshes[i].pTransformedNormals;
			delete[] m_Meshes[i].pIndices;
		}
		m_Meshes.clear();

		for (unsigned int i = 0; i < m_Skeleton.Bones.size(); ++i)
		{
			delete[] m_Skeleton.Bones[i].pWeights;
			delete[] m_Skeleton.Bones[i].pChildren;
		}
		m_Skeleton.Bones.clear();

		m_BoneGlobalTransforms.clear();

		m_Animation.NodeAnimations.clear();
		m_Animation.Duration = 0.0f;
		m_Animation.TicksPerSecond = 0.0f;

		m_GlobalInverseTransform = glm::mat4x4(1);
		m_AnimationTime = 0.0f;
	}





	void SkeletalModel::Update(float a_Dt)
	{
		if (m_Animation.Duration > 0.0f)
		{
			m_AnimationTime = fmodf(m_AnimationTime + a_Dt * m_Animation.TicksPerSecond, m_Animation.Duration);
		}
		else
		{
			m_AnimationTime = 0.0f;
		}

		m_BoneGlobalTransforms.assign(m_Skeleton.Bones.size(), glm::mat4x4(1));

		if (!m_Skeleton.Bones.empty())
		{
			ReadNodeHierarchy(m_AnimationTime, m_Animation, m_Skeleton, m_Skeleton.Bones[0], glm::mat4x4(1), 0);
		}

		TransformVertices(m_Skeleton);
	}





	void SkeletalModel::ReadNodeHierarchy(float AnimationTime, sAnimation& a_Animation, sSkeleton& a_Skeleton, sBone& a_Bone, const glm::mat4x4& ParentTransform, unsigned int a_BoneIndex)
	{
		std::string NodeName(a_Bone.Name);
		glm::mat4x4 NodeTransformation(a_Bone.NodeTransform);
		const sNodeAnimation* pNewNodeAnim = FindNodeAnim(a_Animation, NodeName);

		if (pNewNodeAnim)
		{
			glm::vec3 Translation = NodeAnimation_FindInterpolatedPosition(*pNewNodeAnim, AnimationTime);
			glm::quat RotationQ = NodeAnimation_FindInterpolatedRotation(*pNewNodeAnim, AnimationTime);

// 			glm::vec3 Scaling2(1, 1, 1);
// 			glm::mat4x4 ScalingM2 = glm::scale(Scaling2);

			glm::mat4x4 RotationM2 = glm::toMat4(RotationQ);

			glm::mat4x4 TranslationM2 = glm::translate(Translation);

			// Combine the above transformations
			NodeTransformation = TranslationM2 * RotationM2;// * ScalingM2;
		}

		// The hierarchy pose depends only on the node transforms. A mesh's
		// inverse bind matrix must never leak into the child transforms.
		glm::mat4x4 GlobalTransformation = ParentTransform * NodeTransformation;

		if (a_BoneIndex < a_Skeleton.Bones.size())
		{
			m_BoneGlobalTransforms[a_BoneIndex] = GlobalTransformation;

			// Legacy single-mesh behaviour: FinalTransformation still includes
			// the bone's own OffsetMatrix.
			sBone* pBone = &a_Skeleton.Bones[a_BoneIndex];
			pBone->FinalTransformation = m_GlobalInverseTransform * GlobalTransformation * pBone->OffsetMatrix;
		}

		for (unsigned int i = 0; i < a_Bone.NumChildren; i++)
		{
			unsigned int ChildIndex = a_Bone.pChildren[i];
			if (ChildIndex < a_Skeleton.Bones.size())
			{
				ReadNodeHierarchy(AnimationTime, a_Animation, a_Skeleton, a_Skeleton.Bones[ChildIndex], GlobalTransformation, ChildIndex);
			}
		}
	}





	void SkeletalModel::TransformVertices(const sSkeleton& a_Skeleton)
	{
		for (unsigned int i = 0; i < m_Meshes.size(); ++i)
		{
			sAnimatedMesh& AnimMesh = m_Meshes[i];

			if (AnimMesh.UseMeshBindings)
			{
				TransformMeshWithBindings(AnimMesh, a_Skeleton);
			}
			else
			{
				TransformMeshWithBoneWeights(AnimMesh, a_Skeleton);
			}

			NormalizeTransformedNormals(AnimMesh);
		}
	}





	void SkeletalModel::TransformMeshWithBindings(sAnimatedMesh& a_Mesh, const sSkeleton& a_Skeleton) const
	{
		if (a_Mesh.NumVertices == 0)
			return;

		// Vertices that receive no weight keep their original position and
		// unit normal.
		for (unsigned int v = 0; v < a_Mesh.NumVertices; ++v)
		{
			a_Mesh.pTransformedVertices[v] = a_Mesh.pVertices[v];
			a_Mesh.pTransformedNormals[v] = a_Mesh.pNormals[v];
		}

		std::vector<bool> Touched(a_Mesh.NumVertices, false);

		for (unsigned int b = 0; b < a_Mesh.MeshBindings.size(); ++b)
		{
			const sMeshBoneBinding& Binding = a_Mesh.MeshBindings[b];
			if (Binding.BoneIndex >= a_Skeleton.Bones.size() || Binding.BoneIndex >= m_BoneGlobalTransforms.size())
				continue;

			glm::mat4x4 Transformation = m_GlobalInverseTransform * m_BoneGlobalTransforms[Binding.BoneIndex] * Binding.OffsetMatrix;
			glm::mat3x3 Rotation = glm::mat3x3(Transformation);

			for (unsigned int w = 0; w < Binding.Weights.size(); ++w)
			{
				const sWeight& Weight = Binding.Weights[w];
				if (Weight.VertexID >= a_Mesh.NumVertices)
					continue;

				if (!Touched[Weight.VertexID])
				{
					Touched[Weight.VertexID] = true;
					a_Mesh.pTransformedVertices[Weight.VertexID] = glm::vec3(0.0f);
					a_Mesh.pTransformedNormals[Weight.VertexID] = glm::vec3(0.0f);
				}

				const glm::vec3& inVertex = a_Mesh.pVertices[Weight.VertexID];
				a_Mesh.pTransformedVertices[Weight.VertexID] += glm::vec3(Transformation * glm::vec4(inVertex, 1.0f)) * Weight.Weight;

				const glm::vec3& inNormal = a_Mesh.pNormals[Weight.VertexID];
				a_Mesh.pTransformedNormals[Weight.VertexID] += (Rotation * inNormal) * Weight.Weight;
			}
		}
	}





	void SkeletalModel::TransformMeshWithBoneWeights(sAnimatedMesh& a_Mesh, const sSkeleton& a_Skeleton) const
	{
		if (a_Mesh.NumVertices == 0)
			return;

		// Reset mesh vertices and normals
		memset(a_Mesh.pTransformedVertices, 0, a_Mesh.NumVertices*sizeof(glm::vec3));
		memset(a_Mesh.pTransformedNormals, 0, a_Mesh.NumVertices*sizeof(glm::vec3));

		for (unsigned int b = 0; b < a_Skeleton.Bones.size(); ++b)
		{
			const sBone& Bone = a_Skeleton.Bones[b];
			//
			glm::mat4x4 Transformation = Bone.FinalTransformation;
			glm::mat3x3 Rotation = glm::mat3x3(Transformation);
			//
			for (unsigned int w = 0; w < Bone.NumWeights; ++w)
			{
				const sWeight& Weight = Bone.pWeights[w];
				if (Weight.VertexID >= a_Mesh.NumVertices)
					continue;

				glm::vec3 inVertex = a_Mesh.pVertices[Weight.VertexID];
				glm::vec3& outVertex = a_Mesh.pTransformedVertices[Weight.VertexID];
				outVertex += glm::vec3((Transformation * glm::vec4(inVertex, 1)) * Weight.Weight);
				//
				glm::vec3 inNormal = a_Mesh.pNormals[Weight.VertexID];
				glm::vec3& outNormal = a_Mesh.pTransformedNormals[Weight.VertexID];
				outNormal += (Rotation * inNormal) * Weight.Weight;
			}
		}
	}





	void SkeletalModel::NormalizeTransformedNormals(sAnimatedMesh& a_Mesh) const
	{
		for (unsigned int v = 0; v < a_Mesh.NumVertices; ++v)
		{
			glm::vec3& Normal = a_Mesh.pTransformedNormals[v];
			float LengthSq = glm::dot(Normal, Normal);
			if (LengthSq > 0.0f)
				Normal = Normal / sqrtf(LengthSq);
		}
	}





	const sNodeAnimation* FindNodeAnim(const sAnimation& a_Animation, const std::string& a_NodeName)
	{
		for (unsigned int i = 0; i < a_Animation.NodeAnimations.size(); ++i)
		{
			const sNodeAnimation& NodeAnim = a_Animation.NodeAnimations[i];
			if (NodeAnim.Name == a_NodeName)
			{
				return &NodeAnim;
			}
		}

		return NULL;
	}





	glm::vec3 InterpolateValue(const glm::vec3& a_Start, const glm::vec3& a_End, float a_Factor)
	{
		return glm::mix(a_Start, a_End, a_Factor);
	}





	glm::quat InterpolateValue(const glm::quat& a_Start, const glm::quat& a_End, float a_Factor)
	{
		return glm::slerp(a_Start, a_End, a_Factor);
	}





	unsigned int Skeleton_FindBoneIndex(const sSkeleton& a_Skeleton, const std::string& a_Name)
	{
		for (unsigned int i = 0; i < a_Skeleton.Bones.size(); ++i)
		{
			if (a_Skeleton.Bones[i].Name == a_Name)
				return i;
		}
		return -1;
	}





	sBone* Skeleton_FindBone(sSkeleton& a_Skeleton, const std::string& a_Name)
	{
		return &a_Skeleton.Bones[Skeleton_FindBoneIndex(a_Skeleton, a_Name)];
	}
}