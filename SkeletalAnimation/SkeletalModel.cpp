#include "SkeletalModel.h"

#include <cmath>

//////////////////////////////////////////////////////////////////////////
// Based on: http://ogldev.atspace.co.uk/www/tutorial38/tutorial38.html //
//////////////////////////////////////////////////////////////////////////

namespace SA
{
	SkeletalModel::SkeletalModel()
		: m_AnimationTime(0.0f)
		, m_MirrorEnabled(false)
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

		// The mirror configuration is runtime state and is dropped here.
		m_MirrorEnabled = false;
		m_MirrorNormal = glm::vec3(0.0f, 0.0f, 1.0f);
		m_MirrorReflection = glm::mat4x4(1);
		m_MirrorInvGlobalInverse = glm::mat4x4(1);
		m_MirrorPartners.clear();
		m_MirrorWeights.clear();
		m_MirrorBindC.clear();
		m_MirrorBindInvC.clear();
		m_MirrorParents.clear();
		m_MirrorOrder.clear();
	}





	void SkeletalModel::Update(float a_Dt)
	{
		m_AnimationTime = fmodf(m_AnimationTime + a_Dt * m_Animation.TicksPerSecond, m_Animation.Duration);
		//
		if (m_MirrorEnabled)
		{
			UpdateMirrored();
		}
		else
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
			pBone->FinalTransformation = m_GlobalInverseTransform * GlobalTransformation * pBone->OffsetMatrix;
		}

		for (unsigned int i = 0; i < a_Bone.NumChildren; i++)
		{
			ReadNodeHierarchy(AnimationTime, a_Animation, a_Skeleton, a_Skeleton.Bones[a_Bone.pChildren[i]], GlobalTransformation);
		}
	}





	void SkeletalModel::TransformVertices(const sSkeleton& a_Skeleton)
	{
		for (unsigned int i = 0; i < m_Meshes.size(); ++i)
		{
			// Reset mesh vertices and normals
			sAnimatedMesh& AnimMesh = m_Meshes[i];
			memset(AnimMesh.pTransformedVertices, 0, AnimMesh.NumVertices*sizeof(glm::vec3));
			memset(AnimMesh.pTransformedNormals, 0, AnimMesh.NumVertices*sizeof(glm::vec3));

			//
			for (unsigned int i = 0; i < a_Skeleton.Bones.size(); ++i)
			{
				const sBone& Bone = a_Skeleton.Bones[i];
				//
				glm::mat4x4 Transformation = Bone.FinalTransformation;
				glm::mat3x3 Rotation = glm::mat3x3(Transformation);
				//
				for (unsigned int i = 0; i < Bone.NumWeights; ++i)
				{
					sWeight Weight = Bone.pWeights[i];
					//
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

		// Normalize normals
		for (unsigned int i = 0; i < m_Meshes.size(); ++i)
		{
			sAnimatedMesh& AnimMesh = m_Meshes[i];
			for (unsigned int i = 0; i < AnimMesh.NumVertices; ++i)
			{
				AnimMesh.pTransformedNormals[i] = glm::normalize(AnimMesh.pTransformedNormals[i]);
			}
		}
	}





	//////////////////////////////////////////////////////////////////////////
	// Motion mirroring
	//
	// Conventions (column vectors):
	//   B_i  bind global pose, accumulated from NodeTransform at configure time
	//   G_i  original motion global pose sampled this update
	//   I    GetGlobalInverseTransform()
	//   C_i  I * B_i, K_i = I * G_i
	//   H    homogeneous reflection through the model origin (unit normal n)
	// The full mirror target is K'_i = H * (K_p * inverse(C_p)) * H * C_i with
	// p = partners[i]. K' is converted back to skeleton global space, turned
	// into locals against each target's own mirrored parent (root: identity),
	// blended per bone against the original locals (translation lerp, rotation
	// shortest-arc slerp) and finally re-evaluated with the regular FK,
	// globalInverse, OffsetMatrix and CPU skinning, each used once.
	//////////////////////////////////////////////////////////////////////////

	static bool Mirror_IsFinite(const glm::mat4x4& a_Matrix)
	{
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
				if (!std::isfinite(a_Matrix[c][r]))
					return false;
		return true;
	}





	static bool Mirror_IsRigid(const glm::mat4x4& a_Matrix)
	{
		// Homogeneous bottom row (0, 0, 0, 1).
		if (fabsf(a_Matrix[0][3]) > 1e-4f || fabsf(a_Matrix[1][3]) > 1e-4f ||
			fabsf(a_Matrix[2][3]) > 1e-4f || fabsf(a_Matrix[3][3] - 1.0f) > 1e-4f)
			return false;
		// Orthonormal basis with positive unit determinant (no scale, no reflection).
		glm::vec3 X(a_Matrix[0][0], a_Matrix[0][1], a_Matrix[0][2]);
		glm::vec3 Y(a_Matrix[1][0], a_Matrix[1][1], a_Matrix[1][2]);
		glm::vec3 Z(a_Matrix[2][0], a_Matrix[2][1], a_Matrix[2][2]);
		if (fabsf(glm::length(X) - 1.0f) > 1e-3f) return false;
		if (fabsf(glm::length(Y) - 1.0f) > 1e-3f) return false;
		if (fabsf(glm::length(Z) - 1.0f) > 1e-3f) return false;
		if (fabsf(glm::dot(X, Y)) > 1e-3f) return false;
		if (fabsf(glm::dot(X, Z)) > 1e-3f) return false;
		if (fabsf(glm::dot(Y, Z)) > 1e-3f) return false;
		if (fabsf(glm::dot(glm::cross(X, Y), Z) - 1.0f) > 1e-2f) return false;
		return true;
	}





	static glm::mat4x4 Mirror_Reflection(const glm::vec3& a_Normal)
	{
		// H = I - 2 * n * n^T on the rotation part, no translation.
		glm::mat4x4 H(1.0f);
		for (int c = 0; c < 3; ++c)
			for (int r = 0; r < 3; ++r)
				H[c][r] = (c == r ? 1.0f : 0.0f) - 2.0f * a_Normal[c] * a_Normal[r];
		return H;
	}





	static void Mirror_ExtractTR(const glm::mat4x4& a_Matrix, glm::vec3& a_OutT, glm::quat& a_OutR)
	{
		a_OutT = glm::vec3(a_Matrix[3]);
		a_OutR = glm::normalize(glm::quat_cast(glm::mat3x3(a_Matrix)));
	}





	static glm::quat Mirror_SlerpShortest(const glm::quat& a_A, const glm::quat& a_B, float a_Factor)
	{
		// Both inputs are expected to be normalized; always walk the short arc.
		glm::quat B = a_B;
		float CosTheta = glm::dot(a_A, a_B);
		if (CosTheta < 0.0f)
		{
			B = -a_B;
			CosTheta = -CosTheta;
		}
		if (CosTheta > 0.9995f)
		{
			// Nearly parallel: normalized lerp avoids the degenerate sine denominator.
			return glm::normalize(a_A * (1.0f - a_Factor) + B * a_Factor);
		}
		CosTheta = glm::clamp(CosTheta, -1.0f, 1.0f);
		float Theta = acosf(CosTheta);
		float SinTheta = sinf(Theta);
		float WA = sinf((1.0f - a_Factor) * Theta) / SinTheta;
		float WB = sinf(a_Factor * Theta) / SinTheta;
		return WA * a_A + WB * B;
	}





	bool SkeletalModel::ConfigureMirror(const std::vector<unsigned int>& a_Partners, const glm::vec3& a_Normal)
	{
		const unsigned int NumBones = (unsigned int)m_Skeleton.Bones.size();
		if (NumBones == 0)
			return false;
		if (a_Partners.size() != NumBones)
			return false;

		// The mirror plane passes through the model origin with a finite unit normal.
		if (!std::isfinite(a_Normal.x) || !std::isfinite(a_Normal.y) || !std::isfinite(a_Normal.z))
			return false;
		if (fabsf(glm::length(a_Normal) - 1.0f) > 1e-4f)
			return false;

		// Partners must be an involutive permutation over all bones (self pairs allowed).
		{
			std::vector<unsigned char> Seen(NumBones, 0);
			for (unsigned int i = 0; i < NumBones; ++i)
			{
				unsigned int Partner = a_Partners[i];
				if (Partner >= NumBones || Seen[Partner])
					return false;
				Seen[Partner] = 1;
			}
			for (unsigned int i = 0; i < NumBones; ++i)
			{
				if (a_Partners[a_Partners[i]] != i)
					return false;
			}
		}

		// The skeleton must be a single rooted, acyclic hierarchy reaching every bone.
		std::vector<int> Parents(NumBones, -1);
		for (unsigned int i = 0; i < NumBones; ++i)
		{
			const sBone& Bone = m_Skeleton.Bones[i];
			if (Bone.NumChildren > 0 && Bone.pChildren == NULL)
				return false;
			for (unsigned int c = 0; c < Bone.NumChildren; ++c)
			{
				unsigned int Child = Bone.pChildren[c];
				if (Child >= NumBones || Child == i)
					return false;
				if (Parents[Child] != -1)
					return false;
				Parents[Child] = (int)i;
			}
		}
		int Root = -1;
		for (unsigned int i = 0; i < NumBones; ++i)
		{
			if (Parents[i] < 0)
			{
				if (Root != -1)
					return false;
				Root = (int)i;
			}
		}
		if (Root < 0)
			return false;

		std::vector<unsigned int> Order;
		Order.reserve(NumBones);
		{
			std::vector<unsigned int> Stack(1, (unsigned int)Root);
			while (!Stack.empty())
			{
				unsigned int Current = Stack.back();
				Stack.pop_back();
				Order.push_back(Current);
				const sBone& Bone = m_Skeleton.Bones[Current];
				for (unsigned int c = 0; c < Bone.NumChildren; ++c)
					Stack.push_back(Bone.pChildren[c]);
			}
		}
		if (Order.size() != NumBones)
			return false;

		// The captured reference (bind locals and the global inverse) must be finite and rigid.
		for (unsigned int i = 0; i < NumBones; ++i)
		{
			if (!Mirror_IsFinite(m_Skeleton.Bones[i].NodeTransform) || !Mirror_IsRigid(m_Skeleton.Bones[i].NodeTransform))
				return false;
		}
		if (!Mirror_IsFinite(m_GlobalInverseTransform) || !Mirror_IsRigid(m_GlobalInverseTransform))
			return false;

		// Capture the bind reference: B_i are the bind global poses, C_i = I * B_i.
		std::vector<glm::mat4x4> BindC(NumBones);
		std::vector<glm::mat4x4> BindInvC(NumBones);
		{
			std::vector<glm::mat4x4> BindGlobal(NumBones);
			for (unsigned int o = 0; o < NumBones; ++o)
			{
				unsigned int i = Order[o];
				const glm::mat4x4& Local = m_Skeleton.Bones[i].NodeTransform;
				BindGlobal[i] = (Parents[i] < 0) ? Local : BindGlobal[Parents[i]] * Local;
			}
			for (unsigned int i = 0; i < NumBones; ++i)
			{
				BindC[i] = m_GlobalInverseTransform * BindGlobal[i];
				BindInvC[i] = glm::inverse(BindC[i]);
			}
		}

		// Validation passed: commit the configuration (weights default to 1).
		m_MirrorPartners = a_Partners;
		m_MirrorWeights.assign(NumBones, 1.0f);
		m_MirrorNormal = a_Normal;
		m_MirrorReflection = Mirror_Reflection(a_Normal);
		m_MirrorInvGlobalInverse = glm::inverse(m_GlobalInverseTransform);
		m_MirrorBindC = BindC;
		m_MirrorBindInvC = BindInvC;
		m_MirrorParents = Parents;
		m_MirrorOrder = Order;
		m_MirrorEnabled = true;
		return true;
	}





	bool SkeletalModel::SetMirrorWeights(const std::vector<float>& a_Weights)
	{
		if (m_MirrorPartners.empty())
			return false;
		if (a_Weights.size() != m_MirrorPartners.size())
			return false;
		for (unsigned int i = 0; i < a_Weights.size(); ++i)
		{
			if (!std::isfinite(a_Weights[i]) || a_Weights[i] < 0.0f || a_Weights[i] > 1.0f)
				return false;
		}
		m_MirrorWeights = a_Weights;
		return true;
	}





	void SkeletalModel::DisableMirror()
	{
		m_MirrorEnabled = false;
	}





	void SkeletalModel::UpdateMirrored()
	{
		const unsigned int NumBones = (unsigned int)m_Skeleton.Bones.size();
		const glm::mat4x4& I = m_GlobalInverseTransform;

		// Sample the original motion once; every mirror source below reads from
		// this single snapshot. Bones without a channel keep their bind local pose.
		std::vector<glm::vec3> OriginalT(NumBones);
		std::vector<glm::quat> OriginalR(NumBones);
		std::vector<glm::mat4x4> OriginalGlobal(NumBones);
		for (unsigned int o = 0; o < NumBones; ++o)
		{
			unsigned int i = m_MirrorOrder[o];
			const sBone& Bone = m_Skeleton.Bones[i];
			glm::vec3 T;
			glm::quat R;
			const sNodeAnimation* pNodeAnim = FindNodeAnim(m_Animation, Bone.Name);
			if (pNodeAnim)
			{
				T = NodeAnimation_FindInterpolatedPosition(*pNodeAnim, m_AnimationTime);
				R = NodeAnimation_FindInterpolatedRotation(*pNodeAnim, m_AnimationTime);
			}
			else
			{
				Mirror_ExtractTR(Bone.NodeTransform, T, R);
			}
			OriginalT[i] = T;
			OriginalR[i] = glm::normalize(R);
			glm::mat4x4 Local = glm::translate(T) * glm::toMat4(R);
			OriginalGlobal[i] = (m_MirrorParents[i] < 0) ? Local : OriginalGlobal[m_MirrorParents[i]] * Local;
		}

		// Full mirror target in model space, converted back to skeleton global space.
		std::vector<glm::mat4x4> MirroredGlobal(NumBones);
		for (unsigned int i = 0; i < NumBones; ++i)
		{
			unsigned int Partner = m_MirrorPartners[i];
			glm::mat4x4 K = I * OriginalGlobal[Partner];
			glm::mat4x4 MirroredK = m_MirrorReflection * (K * m_MirrorBindInvC[Partner]) * m_MirrorReflection * m_MirrorBindC[i];
			MirroredGlobal[i] = m_MirrorInvGlobalInverse * MirroredK;
		}

		// Mirrored locals, each relative to its own parent's mirrored global pose
		// (the root is taken relative to the identity).
		std::vector<glm::vec3> MirroredT(NumBones);
		std::vector<glm::quat> MirroredR(NumBones);
		{
			std::vector<glm::mat4x4> InvMirroredGlobal(NumBones);
			for (unsigned int i = 0; i < NumBones; ++i)
				InvMirroredGlobal[i] = glm::inverse(MirroredGlobal[i]);
			for (unsigned int i = 0; i < NumBones; ++i)
			{
				glm::mat4x4 Local = (m_MirrorParents[i] < 0) ? MirroredGlobal[i] : InvMirroredGlobal[m_MirrorParents[i]] * MirroredGlobal[i];
				Mirror_ExtractTR(Local, MirroredT[i], MirroredR[i]);
			}
		}

		// Blend original and mirrored locals per bone, then re-run the full FK;
		// globalInverse, OffsetMatrix and the CPU skinning are each used once.
		std::vector<glm::mat4x4> BlendedGlobal(NumBones);
		for (unsigned int o = 0; o < NumBones; ++o)
		{
			unsigned int i = m_MirrorOrder[o];
			float Weight = m_MirrorWeights[i];
			glm::vec3 T = glm::mix(OriginalT[i], MirroredT[i], Weight);
			glm::quat R = Mirror_SlerpShortest(OriginalR[i], MirroredR[i], Weight);
			glm::mat4x4 Local = glm::translate(T) * glm::toMat4(R);
			BlendedGlobal[i] = (m_MirrorParents[i] < 0) ? Local : BlendedGlobal[m_MirrorParents[i]] * Local;
			sBone& Bone = m_Skeleton.Bones[i];
			Bone.FinalTransformation = I * BlendedGlobal[i] * Bone.OffsetMatrix;
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