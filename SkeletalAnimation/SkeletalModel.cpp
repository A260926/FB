#include "SkeletalModel.h"

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

		m_Animation.NodeAnimations.clear();
		m_Animation.Duration = 0.0f;
		m_Animation.TicksPerSecond = 0.0f;

		m_GlobalInverseTransform = glm::mat4x4(1);
		m_AnimationTime = 0.0f;
	}





	void SkeletalModel::Update(float a_Dt)
	{
		// A static model (or one without animation data) has a zero duration;
		// taking a modulus by it would poison the clock with NaN.
		if (m_Animation.Duration > 0.0f)
			m_AnimationTime = fmodf(m_AnimationTime + a_Dt * m_Animation.TicksPerSecond, m_Animation.Duration);
		//

		// Static meshes (or an empty skeleton) still need their transformed
		// buffers populated.
		if (!m_Skeleton.Bones.empty())
		{
			ReadNodeHierarchy(m_AnimationTime, m_Animation, m_Skeleton, m_Skeleton.Bones[0], glm::mat4x4(1));
		}
		TransformVertices(m_Skeleton);
	}





	void SkeletalModel::ReadNodeHierarchy(float AnimationTime, sAnimation& a_Animation, sSkeleton& a_Skeleton, sBone& a_Bone, const glm::mat4x4& ParentTransform)
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

		glm::mat4x4 GlobalTransformation = ParentTransform * NodeTransformation;

		unsigned int BoneIndex = Skeleton_FindBoneIndex(a_Skeleton, NodeName);
		if (BoneIndex != -1)
		{
			sBone* pBone = &a_Skeleton.Bones[BoneIndex];
			// Record the pure hierarchy pose so mesh-local bindings can apply
			// their own inverse bind matrix without inheriting the bone's.
			pBone->GlobalTransformation = GlobalTransformation;
			pBone->FinalTransformation = m_GlobalInverseTransform * GlobalTransformation * pBone->OffsetMatrix;
		}

		for (unsigned int i = 0; i < a_Bone.NumChildren; i++)
		{
			ReadNodeHierarchy(AnimationTime, a_Animation, a_Skeleton, a_Skeleton.Bones[a_Bone.pChildren[i]], GlobalTransformation);
		}
	}





	void SkeletalModel::TransformVertices(const sSkeleton& a_Skeleton)
	{
		for (unsigned int MeshIndex = 0; MeshIndex < m_Meshes.size(); ++MeshIndex)
		{
			sAnimatedMesh& AnimMesh = m_Meshes[MeshIndex];

			// Start from the bind pose. Vertices that receive no weights keep
			// these values (the transformed buffers always hold usable data).
			for (unsigned int v = 0; v < AnimMesh.NumVertices; ++v)
			{
				AnimMesh.pTransformedVertices[v] = AnimMesh.pVertices[v];
				AnimMesh.pTransformedNormals[v] = AnimMesh.pNormals[v];
			}

			// Static mesh: an empty binding table leaves positions and unit
			// normals at their original values.
			if (AnimMesh.UseMeshBindings && AnimMesh.MeshBindings.empty())
				continue;

			// Vertices accumulate from zero (not from the bind pose), so track
			// which vertices have already been touched.
			std::vector<unsigned char> Touched(AnimMesh.NumVertices, 0);

			if (AnimMesh.UseMeshBindings)
			{
				// Mesh-local binding: vertex IDs and offset matrices are scoped
				// to this mesh only.
				for (unsigned int BindingIndex = 0; BindingIndex < AnimMesh.MeshBindings.size(); ++BindingIndex)
				{
					const sMeshBoneBinding& Binding = AnimMesh.MeshBindings[BindingIndex];
					if (Binding.BoneIndex >= a_Skeleton.Bones.size())
						continue;

					const sBone& Bone = a_Skeleton.Bones[Binding.BoneIndex];
					glm::mat4x4 Transformation = m_GlobalInverseTransform * Bone.GlobalTransformation * Binding.OffsetMatrix;
					glm::mat3x3 Rotation = glm::mat3x3(Transformation);

					for (unsigned int w = 0; w < Binding.Weights.size(); ++w)
					{
						const sWeight& Weight = Binding.Weights[w];
						if (Weight.VertexID >= AnimMesh.NumVertices)
							continue;

						if (!Touched[Weight.VertexID])
						{
							Touched[Weight.VertexID] = 1;
							AnimMesh.pTransformedVertices[Weight.VertexID] = glm::vec3(0.0f);
							AnimMesh.pTransformedNormals[Weight.VertexID] = glm::vec3(0.0f);
						}

						glm::vec3 inVertex = AnimMesh.pVertices[Weight.VertexID];
						glm::vec3& outVertex = AnimMesh.pTransformedVertices[Weight.VertexID];
						outVertex += glm::vec3((Transformation * glm::vec4(inVertex, 1)) * Weight.Weight);

						glm::vec3 inNormal = AnimMesh.pNormals[Weight.VertexID];
						glm::vec3& outNormal = AnimMesh.pTransformedNormals[Weight.VertexID];
						outNormal += (Rotation * inNormal) * Weight.Weight;
					}
				}
			}
			else
			{
				// Legacy path: weights and offset matrices live on the bones and
				// vertex IDs are global to the model.
				for (unsigned int BoneIndex = 0; BoneIndex < a_Skeleton.Bones.size(); ++BoneIndex)
				{
					const sBone& Bone = a_Skeleton.Bones[BoneIndex];
					//
					glm::mat4x4 Transformation = Bone.FinalTransformation;
					glm::mat3x3 Rotation = glm::mat3x3(Transformation);
					//
					for (unsigned int w = 0; w < Bone.NumWeights; ++w)
					{
						sWeight Weight = Bone.pWeights[w];
						if (Weight.VertexID >= AnimMesh.NumVertices)
							continue;

						if (!Touched[Weight.VertexID])
						{
							Touched[Weight.VertexID] = 1;
							AnimMesh.pTransformedVertices[Weight.VertexID] = glm::vec3(0.0f);
							AnimMesh.pTransformedNormals[Weight.VertexID] = glm::vec3(0.0f);
						}

						glm::vec3 inVertex = AnimMesh.pVertices[Weight.VertexID];
						glm::vec3& outVertex = AnimMesh.pTransformedVertices[Weight.VertexID];
						outVertex += glm::vec3((Transformation * glm::vec4(inVertex, 1)) * Weight.Weight);
						//
						glm::vec3 inNormal = AnimMesh.pNormals[Weight.VertexID];
						glm::vec3& outNormal = AnimMesh.pTransformedNormals[Weight.VertexID];
						outNormal += (Rotation * inNormal) * Weight.Weight;
					}
				}
			}

			// Normalize the normals of every vertex that received weights.
			// Unweighted vertices keep the bind-pose normal set above.
			for (unsigned int v = 0; v < AnimMesh.NumVertices; ++v)
			{
				if (!Touched[v])
					continue;
				glm::vec3 Normal = AnimMesh.pTransformedNormals[v];
				float LengthSq = glm::dot(Normal, Normal);
				if (LengthSq > 0.0f)
					AnimMesh.pTransformedNormals[v] = Normal / sqrtf(LengthSq);
			}
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