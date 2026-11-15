// Motion mirroring tests. Registers cases through task_cases/task_case_count;
// the driver (task_support/driver.c) provides main().

#include "task_support/check.h"
#include "SkeletalAnimation/SkeletalModel.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace SA;

namespace
{
	const float kTol = 2e-4f;      // ordinary-scale numeric tolerance
	const float kPathTol = 1e-5f;  // same math through a different code path
	const float kTightTol = 1e-6f; // deterministic repeat of the same computation

	bool VecClose(const glm::vec3& a, const glm::vec3& b, float tol)
	{
		return glm::length(a - b) <= tol;
	}

	bool MatClose(const glm::mat4x4& a, const glm::mat4x4& b, float tol)
	{
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
				if (std::fabs(a[c][r] - b[c][r]) > tol)
					return false;
		return true;
	}

	glm::quat RotX(float deg) { return glm::angleAxis(deg, glm::vec3(1.0f, 0.0f, 0.0f)); }
	glm::quat RotY(float deg) { return glm::angleAxis(deg, glm::vec3(0.0f, 1.0f, 0.0f)); }
	glm::quat RotZ(float deg) { return glm::angleAxis(deg, glm::vec3(0.0f, 0.0f, 1.0f)); }

	// Homogeneous reflection through the model origin with unit normal n.
	glm::mat4x4 Reflection(const glm::vec3& n)
	{
		glm::mat4x4 H(1.0f);
		for (int c = 0; c < 3; ++c)
			for (int r = 0; r < 3; ++r)
				H[c][r] = (c == r ? 1.0f : 0.0f) - 2.0f * n[c] * n[r];
		return H;
	}

	void ExtractTR(const glm::mat4x4& m, glm::vec3& t, glm::quat& r)
	{
		t = glm::vec3(m[3]);
		r = glm::normalize(glm::quat_cast(glm::mat3x3(m)));
	}

	glm::quat SlerpShort(const glm::quat& a, const glm::quat& b, float t)
	{
		glm::quat bb = b;
		float cosTheta = glm::dot(a, b);
		if (cosTheta < 0.0f)
		{
			bb = -b;
			cosTheta = -cosTheta;
		}
		if (cosTheta > 0.9995f)
			return glm::normalize(a * (1.0f - t) + bb * t);
		cosTheta = glm::clamp(cosTheta, -1.0f, 1.0f);
		float theta = std::acos(cosTheta);
		float sinTheta = std::sin(theta);
		return (std::sin((1.0f - t) * theta) * a + std::sin(t * theta) * bb) / sinTheta;
	}

	// ------------------------------------------------------------------
	// Model construction helpers
	// ------------------------------------------------------------------

	void AddBone(SkeletalModel& model, const char* name, const glm::vec3& t, const glm::quat& r,
		const unsigned int* children, unsigned int numChildren)
	{
		sBone bone;
		bone.Name = name;
		bone.NodeTransform = glm::translate(t) * glm::toMat4(r);
		bone.OffsetMatrix = glm::mat4x4(1.0f);
		bone.FinalTransformation = glm::mat4x4(1.0f);
		bone.NumWeights = 0;
		bone.pWeights = NULL;
		bone.NumChildren = numChildren;
		bone.pChildren = NULL;
		if (numChildren > 0)
		{
			bone.pChildren = new unsigned int[numChildren];
			for (unsigned int i = 0; i < numChildren; ++i)
				bone.pChildren[i] = children[i];
		}
		model.GetSkeleton().Bones.push_back(bone);
	}

	void ComputeParents(const sSkeleton& skel, std::vector<int>& parents)
	{
		parents.assign(skel.Bones.size(), -1);
		for (unsigned int i = 0; i < skel.Bones.size(); ++i)
			for (unsigned int c = 0; c < skel.Bones[i].NumChildren; ++c)
				parents[skel.Bones[i].pChildren[c]] = (int)i;
	}

	void ComputeOrder(const sSkeleton& skel, std::vector<unsigned int>& order)
	{
		order.clear();
		std::vector<unsigned int> stack(1, 0);
		while (!stack.empty())
		{
			unsigned int u = stack.back();
			stack.pop_back();
			order.push_back(u);
			for (unsigned int c = 0; c < skel.Bones[u].NumChildren; ++c)
				stack.push_back(skel.Bones[u].pChildren[c]);
		}
	}

	// Standard inverse-bind offsets: Offset_i = inverse(B_i).
	void FinalizeBindOffsets(SkeletalModel& model)
	{
		sSkeleton& skel = model.GetSkeleton();
		std::vector<int> parents;
		ComputeParents(skel, parents);
		std::vector<unsigned int> order;
		ComputeOrder(skel, order);
		std::vector<glm::mat4x4> B(skel.Bones.size());
		for (unsigned int o = 0; o < order.size(); ++o)
		{
			unsigned int i = order[o];
			B[i] = (parents[i] < 0) ? skel.Bones[i].NodeTransform : B[parents[i]] * skel.Bones[i].NodeTransform;
		}
		for (unsigned int i = 0; i < skel.Bones.size(); ++i)
			skel.Bones[i].OffsetMatrix = glm::inverse(B[i]);
	}

	void SetWeights(SkeletalModel& model, unsigned int boneIndex, const sWeight* weights, unsigned int count)
	{
		sBone& bone = model.GetSkeleton().Bones[boneIndex];
		bone.NumWeights = count;
		bone.pWeights = new sWeight[count];
		for (unsigned int i = 0; i < count; ++i)
			bone.pWeights[i] = weights[i];
	}

	void AddMesh(SkeletalModel& model, const glm::vec3* verts, const glm::vec3* norms, unsigned int count)
	{
		sAnimatedMesh mesh;
		mesh.NumVertices = count;
		mesh.pVertices = new glm::vec3[count];
		mesh.pNormals = new glm::vec3[count];
		mesh.pTransformedVertices = new glm::vec3[count];
		mesh.pTransformedNormals = new glm::vec3[count];
		mesh.NumIndices = 0;
		mesh.pIndices = NULL;
		for (unsigned int i = 0; i < count; ++i)
		{
			mesh.pVertices[i] = verts[i];
			mesh.pNormals[i] = norms[i];
		}
		model.AddMesh(mesh);
	}

	void AddChannel(SkeletalModel& model, const char* bone,
		const glm::vec3& p0, const glm::quat& r0,
		const glm::vec3& p1, const glm::quat& r1)
	{
		sNodeAnimation na;
		na.Name = bone;
		sNodeAnimationKey<glm::vec3> pk0 = { p0, 0.0f };
		sNodeAnimationKey<glm::vec3> pk1 = { p1, 2.0f };
		sNodeAnimationKey<glm::quat> rk0 = { r0, 0.0f };
		sNodeAnimationKey<glm::quat> rk1 = { r1, 2.0f };
		na.PositionKeys.push_back(pk0);
		na.PositionKeys.push_back(pk1);
		na.RotationKeys.push_back(rk0);
		na.RotationKeys.push_back(rk1);
		model.GetAnimation().NodeAnimations.push_back(na);
	}

	// Asymmetric 4-bone rig: hips -> {arm.up -> arm.lo, spine}. Bind pose is not
	// left/right symmetric, the global inverse is a non-identity rigid transform
	// and the animated rotations do not commute. "spine" has no channel.
	void BuildMainRigSkeleton(SkeletalModel& model)
	{
		const unsigned int hipsKids[] = { 1, 3 };
		const unsigned int upKids[] = { 2 };
		AddBone(model, "hips", glm::vec3(0.10f, 1.00f, -0.05f), glm::quat(1, 0, 0, 0), hipsKids, 2);
		AddBone(model, "arm.up", glm::vec3(0.35f, 0.25f, 0.10f), RotZ(25.0f), upKids, 1);
		AddBone(model, "arm.lo", glm::vec3(0.50f, -0.10f, 0.05f), RotY(40.0f), NULL, 0);
		AddBone(model, "spine", glm::vec3(-0.05f, 0.30f, -0.02f), RotX(10.0f), NULL, 0);
		FinalizeBindOffsets(model);

		model.SetGlobalInverseTransform(glm::translate(glm::vec3(-0.20f, -0.90f, 0.15f)) *
			glm::toMat4(glm::angleAxis(12.0f, glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)))));

		const glm::vec3 verts[] = {
			glm::vec3(0.45f, 1.30f, 0.15f), glm::vec3(0.90f, 1.20f, 0.20f),
			glm::vec3(0.30f, 1.10f, -0.10f), glm::vec3(0.05f, 1.00f, 0.00f),
			glm::vec3(0.80f, 1.05f, 0.30f) };
		const glm::vec3 norms[] = {
			glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f)), glm::normalize(glm::vec3(-0.4f, 0.6f, 0.7f)),
			glm::normalize(glm::vec3(0.5f, -0.5f, 0.7f)), glm::normalize(glm::vec3(0.2f, 0.9f, -0.4f)),
			glm::normalize(glm::vec3(-0.6f, 0.3f, 0.74f)) };
		AddMesh(model, verts, norms, 5);

		const sWeight w0[] = { { 3, 1.0f }, { 4, 0.75f } };
		const sWeight w1[] = { { 0, 1.0f }, { 2, 0.6f } };
		const sWeight w2[] = { { 1, 1.0f }, { 4, 0.25f } };
		const sWeight w3[] = { { 2, 0.4f } };
		SetWeights(model, 0, w0, 2);
		SetWeights(model, 1, w1, 2);
		SetWeights(model, 2, w2, 2);
		SetWeights(model, 3, w3, 1);
	}

	void AddMainRigAnimation(SkeletalModel& model)
	{
		model.GetAnimation().Duration = 2.0f;
		model.GetAnimation().TicksPerSecond = 1.0f;
		AddChannel(model, "hips",
			glm::vec3(0.10f, 1.00f, -0.05f), glm::quat(1, 0, 0, 0),
			glm::vec3(0.40f, 1.15f, 0.20f), RotZ(35.0f));
		AddChannel(model, "arm.up",
			glm::vec3(0.35f, 0.25f, 0.10f), RotZ(25.0f),
			glm::vec3(0.30f, 0.35f, 0.00f), RotX(-45.0f));
		AddChannel(model, "arm.lo",
			glm::vec3(0.50f, -0.10f, 0.05f), RotY(40.0f),
			glm::vec3(0.55f, -0.05f, 0.15f), RotZ(-30.0f));
		// "spine" intentionally has no channel: it keeps its bind local pose.
	}

	void BuildMainRig(SkeletalModel& model)
	{
		BuildMainRigSkeleton(model);
		AddMainRigAnimation(model);
	}

	// Same 4 joints but stored out of parent-first order: "hand" (index 1) is the
	// child of "arm" (index 2). Topology: root -> {arm -> hand, twist}.
	void BuildShuffledRig(SkeletalModel& model)
	{
		const unsigned int rootKids[] = { 2, 3 };
		const unsigned int armKids[] = { 1 };
		AddBone(model, "root", glm::vec3(0.0f, 0.9f, 0.1f), RotY(-15.0f), rootKids, 2);
		AddBone(model, "hand", glm::vec3(0.45f, 0.05f, -0.10f), RotX(30.0f), NULL, 0);
		AddBone(model, "arm", glm::vec3(0.30f, 0.20f, 0.05f), RotZ(35.0f), armKids, 1);
		AddBone(model, "twist", glm::vec3(-0.20f, 0.15f, 0.0f), RotX(-20.0f), NULL, 0);
		FinalizeBindOffsets(model);

		model.SetGlobalInverseTransform(glm::translate(glm::vec3(0.10f, -0.80f, 0.20f)) *
			glm::toMat4(glm::angleAxis(-8.0f, glm::normalize(glm::vec3(1.0f, 0.0f, 0.3f)))));

		model.GetAnimation().Duration = 2.0f;
		model.GetAnimation().TicksPerSecond = 1.0f;
		AddChannel(model, "root", glm::vec3(0.0f, 0.9f, 0.1f), RotY(-15.0f),
			glm::vec3(0.2f, 1.0f, 0.0f), RotZ(25.0f));
		AddChannel(model, "arm", glm::vec3(0.30f, 0.20f, 0.05f), RotZ(35.0f),
			glm::vec3(0.35f, 0.15f, 0.10f), RotX(40.0f));
		AddChannel(model, "hand", glm::vec3(0.45f, 0.05f, -0.10f), RotX(30.0f),
			glm::vec3(0.50f, 0.0f, -0.05f), RotY(-35.0f));
		// "twist" keeps its bind local pose.

		const glm::vec3 verts[] = {
			glm::vec3(0.35f, 1.15f, 0.15f), glm::vec3(0.80f, 1.10f, 0.0f),
			glm::vec3(0.0f, 0.95f, 0.1f), glm::vec3(0.60f, 1.20f, 0.20f) };
		const glm::vec3 norms[] = {
			glm::normalize(glm::vec3(0.5f, 0.5f, 0.7f)), glm::normalize(glm::vec3(-0.3f, 0.9f, 0.3f)),
			glm::normalize(glm::vec3(0.7f, -0.3f, 0.6f)), glm::normalize(glm::vec3(-0.5f, -0.5f, 0.7f)) };
		AddMesh(model, verts, norms, 4);

		const sWeight w0[] = { { 2, 0.5f } };
		const sWeight w1[] = { { 1, 1.0f }, { 3, 0.3f } };
		const sWeight w2[] = { { 0, 1.0f }, { 3, 0.7f } };
		const sWeight w3[] = { { 2, 0.5f } };
		SetWeights(model, 0, w0, 1);
		SetWeights(model, 1, w1, 2);
		SetWeights(model, 2, w2, 2);
		SetWeights(model, 3, w3, 1);
	}

	// 5-bone rig for self-pairing: root -> {L -> L.end, R -> R.end}.
	void BuildSelfPairRig(SkeletalModel& model)
	{
		const unsigned int rootKids[] = { 1, 2 };
		const unsigned int lKids[] = { 3 };
		const unsigned int rKids[] = { 4 };
		AddBone(model, "root", glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1, 0, 0, 0), rootKids, 2);
		AddBone(model, "L", glm::vec3(0.30f, 0.10f, 0.0f), RotZ(20.0f), lKids, 1);
		AddBone(model, "R", glm::vec3(-0.30f, 0.10f, 0.05f), RotZ(-15.0f), rKids, 1);
		AddBone(model, "L.end", glm::vec3(0.40f, 0.0f, 0.10f), RotY(30.0f), NULL, 0);
		AddBone(model, "R.end", glm::vec3(-0.40f, 0.0f, -0.05f), RotX(25.0f), NULL, 0);
		FinalizeBindOffsets(model);

		model.SetGlobalInverseTransform(glm::translate(glm::vec3(0.15f, -1.0f, -0.10f)) *
			glm::toMat4(glm::angleAxis(20.0f, glm::normalize(glm::vec3(0.0f, 1.0f, 0.2f)))));

		model.GetAnimation().Duration = 2.0f;
		model.GetAnimation().TicksPerSecond = 1.0f;
		AddChannel(model, "root", glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1, 0, 0, 0),
			glm::vec3(0.10f, 1.05f, 0.10f), RotX(15.0f));
		AddChannel(model, "L", glm::vec3(0.30f, 0.10f, 0.0f), RotZ(20.0f),
			glm::vec3(0.32f, 0.15f, 0.05f), RotY(45.0f));
		AddChannel(model, "R", glm::vec3(-0.30f, 0.10f, 0.05f), RotZ(-15.0f),
			glm::vec3(-0.28f, 0.12f, 0.0f), RotX(-30.0f));
		// "L.end" and "R.end" keep their bind local poses.

		const glm::vec3 verts[] = {
			glm::vec3(0.35f, 1.15f, 0.05f), glm::vec3(-0.35f, 1.15f, 0.0f),
			glm::vec3(0.75f, 1.10f, 0.15f), glm::vec3(-0.70f, 1.05f, 0.0f) };
		const glm::vec3 norms[] = {
			glm::normalize(glm::vec3(0.6f, 0.6f, 0.5f)), glm::normalize(glm::vec3(-0.6f, 0.6f, 0.5f)),
			glm::normalize(glm::vec3(0.2f, 0.9f, 0.4f)), glm::normalize(glm::vec3(-0.2f, 0.9f, -0.4f)) };
		AddMesh(model, verts, norms, 4);

		const sWeight w0[] = { { 3, 0.4f } };
		const sWeight w1[] = { { 0, 1.0f }, { 2, 0.2f } };
		const sWeight w2[] = { { 1, 1.0f } };
		const sWeight w3[] = { { 2, 0.8f } };
		const sWeight w4[] = { { 3, 0.6f } };
		SetWeights(model, 0, w0, 1);
		SetWeights(model, 1, w1, 2);
		SetWeights(model, 2, w2, 1);
		SetWeights(model, 3, w3, 1);
		SetWeights(model, 4, w4, 1);
	}

	// Static pose animation: one key per bone at t = 0.
	void SetSingleKeyAnimation(SkeletalModel& model, const std::vector<glm::vec3>& ts, const std::vector<glm::quat>& rs)
	{
		sAnimation& anim = model.GetAnimation();
		anim.Duration = 2.0f;
		anim.TicksPerSecond = 1.0f;
		for (unsigned int i = 0; i < model.GetSkeleton().Bones.size(); ++i)
		{
			sNodeAnimation na;
			na.Name = model.GetSkeleton().Bones[i].Name;
			sNodeAnimationKey<glm::vec3> pk = { ts[i], 0.0f };
			sNodeAnimationKey<glm::quat> rk = { rs[i], 0.0f };
			na.PositionKeys.push_back(pk);
			na.RotationKeys.push_back(rk);
			anim.NodeAnimations.push_back(na);
		}
	}

	// ------------------------------------------------------------------
	// Independent reference: mirror target -> local TR -> per-bone blend -> FK
	// ------------------------------------------------------------------

	const sNodeAnimation* FindChannel(const sAnimation& anim, const std::string& name)
	{
		for (unsigned int i = 0; i < anim.NodeAnimations.size(); ++i)
			if (anim.NodeAnimations[i].Name == name)
				return &anim.NodeAnimations[i];
		return NULL;
	}

	glm::vec3 SamplePosition(const sNodeAnimation& na, float t)
	{
		const std::vector<sNodeAnimationKey<glm::vec3> >& keys = na.PositionKeys;
		if (keys.size() == 1)
			return keys[0].Value;
		unsigned int idx = 0;
		for (unsigned int i = 0; i + 1 < keys.size(); ++i)
			if (t < keys[i + 1].Time)
			{
				idx = i;
				break;
			}
		float f = (t - keys[idx].Time) / (keys[idx + 1].Time - keys[idx].Time);
		return glm::mix(keys[idx].Value, keys[idx + 1].Value, f);
	}

	glm::quat SampleRotation(const sNodeAnimation& na, float t)
	{
		const std::vector<sNodeAnimationKey<glm::quat> >& keys = na.RotationKeys;
		if (keys.size() == 1)
			return keys[0].Value;
		unsigned int idx = 0;
		for (unsigned int i = 0; i + 1 < keys.size(); ++i)
			if (t < keys[i + 1].Time)
			{
				idx = i;
				break;
			}
		float f = (t - keys[idx].Time) / (keys[idx + 1].Time - keys[idx].Time);
		return glm::slerp(keys[idx].Value, keys[idx + 1].Value, f);
	}

	struct RefPose
	{
		std::vector<glm::mat4x4> final; // expected FinalTransformation per bone
		std::vector<glm::vec3>  mirT;   // full mirror target locals (pre-blend)
		std::vector<glm::quat>  mirR;
	};

	RefPose ReferencePose(const SkeletalModel& model,
		const std::vector<unsigned int>& partners,
		const glm::vec3& normal,
		const std::vector<float>& weights,
		float animTime)
	{
		const sSkeleton& skel = model.GetSkeleton();
		const sAnimation& anim = model.GetAnimation();
		const unsigned int n = (unsigned int)skel.Bones.size();
		const glm::mat4x4& I = model.GetGlobalInverseTransform();

		std::vector<int> parent;
		ComputeParents(skel, parent);
		std::vector<unsigned int> order;
		ComputeOrder(skel, order);

		// Original locals: sampled channels, bind local pose where there is none.
		std::vector<glm::vec3> origT(n);
		std::vector<glm::quat> origR(n);
		std::vector<glm::mat4x4> origLocal(n);
		for (unsigned int i = 0; i < n; ++i)
		{
			glm::vec3 t;
			glm::quat r;
			const sNodeAnimation* na = FindChannel(anim, skel.Bones[i].Name);
			if (na)
			{
				t = SamplePosition(*na, animTime);
				r = SampleRotation(*na, animTime);
			}
			else
			{
				ExtractTR(skel.Bones[i].NodeTransform, t, r);
			}
			origT[i] = t;
			origR[i] = glm::normalize(r);
			origLocal[i] = glm::translate(t) * glm::toMat4(r);
		}

		// Original and bind global poses.
		std::vector<glm::mat4x4> G(n), B(n);
		for (unsigned int o = 0; o < n; ++o)
		{
			unsigned int i = order[o];
			G[i] = (parent[i] < 0) ? origLocal[i] : G[parent[i]] * origLocal[i];
			B[i] = (parent[i] < 0) ? skel.Bones[i].NodeTransform : B[parent[i]] * skel.Bones[i].NodeTransform;
		}

		// Full mirror target in model space, back to skeleton global space.
		const glm::mat4x4 H = Reflection(normal);
		const glm::mat4x4 invI = glm::inverse(I);
		std::vector<glm::mat4x4> Gm(n);
		for (unsigned int i = 0; i < n; ++i)
		{
			unsigned int p = partners[i];
			glm::mat4x4 K = I * G[p];
			glm::mat4x4 C = I * B[p];
			glm::mat4x4 Km = H * (K * glm::inverse(C)) * H * (I * B[i]);
			Gm[i] = invI * Km;
		}

		// Mirrored locals against each target's own mirrored parent (root: identity).
		RefPose out;
		out.mirT.resize(n);
		out.mirR.resize(n);
		{
			std::vector<glm::mat4x4> invGm(n);
			for (unsigned int i = 0; i < n; ++i)
				invGm[i] = glm::inverse(Gm[i]);
			for (unsigned int i = 0; i < n; ++i)
			{
				glm::mat4x4 M = (parent[i] < 0) ? Gm[i] : invGm[parent[i]] * Gm[i];
				ExtractTR(M, out.mirT[i], out.mirR[i]);
			}
		}

		// Per-bone local blend, then a full FK with I and OffsetMatrix.
		std::vector<glm::mat4x4> Gb(n);
		out.final.resize(n);
		for (unsigned int o = 0; o < n; ++o)
		{
			unsigned int i = order[o];
			float w = weights[i];
			glm::vec3 t = glm::mix(origT[i], out.mirT[i], w);
			glm::quat r = SlerpShort(origR[i], out.mirR[i], w);
			glm::mat4x4 L = glm::translate(t) * glm::toMat4(r);
			Gb[i] = (parent[i] < 0) ? L : Gb[parent[i]] * L;
			out.final[i] = I * Gb[i] * skel.Bones[i].OffsetMatrix;
		}
		return out;
	}

	// Reference CPU skinning of mesh 0 with the given final matrices.
	void ReferenceSkin(const SkeletalModel& model, const std::vector<glm::mat4x4>& finals,
		std::vector<glm::vec3>& outV, std::vector<glm::vec3>& outN)
	{
		const sAnimatedMesh& mesh = model.GetMesh(0);
		const sSkeleton& skel = model.GetSkeleton();
		outV.assign(mesh.NumVertices, glm::vec3(0.0f));
		outN.assign(mesh.NumVertices, glm::vec3(0.0f));
		for (unsigned int b = 0; b < skel.Bones.size(); ++b)
		{
			const sBone& bone = skel.Bones[b];
			glm::mat3x3 rot(finals[b]);
			for (unsigned int w = 0; w < bone.NumWeights; ++w)
			{
				sWeight weight = bone.pWeights[w];
				outV[weight.VertexID] += glm::vec3(finals[b] * glm::vec4(mesh.pVertices[weight.VertexID], 1.0f)) * weight.Weight;
				outN[weight.VertexID] += (rot * mesh.pNormals[weight.VertexID]) * weight.Weight;
			}
		}
		for (unsigned int v = 0; v < mesh.NumVertices; ++v)
			outN[v] = glm::normalize(outN[v]);
	}

	// ------------------------------------------------------------------
	// Snapshot / compare helpers
	// ------------------------------------------------------------------

	std::vector<glm::mat4x4> SnapshotFinals(const SkeletalModel& model)
	{
		std::vector<glm::mat4x4> out(model.GetSkeleton().Bones.size());
		for (unsigned int i = 0; i < out.size(); ++i)
			out[i] = model.GetSkeleton().Bones[i].FinalTransformation;
		return out;
	}

	bool FinalsMatch(const SkeletalModel& model, const std::vector<glm::mat4x4>& ref, float tol)
	{
		if (model.GetSkeleton().Bones.size() != ref.size())
			return false;
		for (unsigned int i = 0; i < ref.size(); ++i)
			if (!MatClose(model.GetSkeleton().Bones[i].FinalTransformation, ref[i], tol))
				return false;
		return true;
	}

	void SnapshotMesh(const SkeletalModel& model, std::vector<glm::vec3>& v, std::vector<glm::vec3>& n)
	{
		const sAnimatedMesh& mesh = model.GetMesh(0);
		v.assign(mesh.pTransformedVertices, mesh.pTransformedVertices + mesh.NumVertices);
		n.assign(mesh.pTransformedNormals, mesh.pTransformedNormals + mesh.NumVertices);
	}

	bool MeshMatches(const SkeletalModel& model, const std::vector<glm::vec3>& v, const std::vector<glm::vec3>& n, float tol)
	{
		const sAnimatedMesh& mesh = model.GetMesh(0);
		if (mesh.NumVertices != v.size())
			return false;
		for (unsigned int i = 0; i < v.size(); ++i)
		{
			if (!VecClose(mesh.pTransformedVertices[i], v[i], tol))
				return false;
			if (!VecClose(mesh.pTransformedNormals[i], n[i], tol))
				return false;
		}
		return true;
	}

	bool AnyFinalDiffers(const SkeletalModel& a, const SkeletalModel& b, float tol)
	{
		for (unsigned int i = 0; i < a.GetSkeleton().Bones.size(); ++i)
			if (!MatClose(a.GetSkeleton().Bones[i].FinalTransformation, b.GetSkeleton().Bones[i].FinalTransformation, tol))
				return true;
		return false;
	}

	std::vector<unsigned int> MainPartners()
	{
		std::vector<unsigned int> p;
		p.push_back(3); p.push_back(2); p.push_back(1); p.push_back(0);
		return p;
	}

	glm::vec3 MainNormal()
	{
		return glm::normalize(glm::vec3(1.0f, 0.35f, -0.2f));
	}

	// ------------------------------------------------------------------
	// spaces
	// ------------------------------------------------------------------

	int CaseFullWeightMirrorReference()
	{
		SkeletalModel model;
		BuildMainRig(model);
		std::vector<unsigned int> partners = MainPartners();
		glm::vec3 normal = MainNormal();
		TASK_CHECK(model.ConfigureMirror(partners, normal)); // weights default to 1
		model.Update(0.7f);

		std::vector<float> ones(4, 1.0f);
		RefPose ref = ReferencePose(model, partners, normal, ones, 0.7f);
		for (unsigned int i = 0; i < 4; ++i)
			TASK_CHECK(MatClose(model.GetSkeleton().Bones[i].FinalTransformation, ref.final[i], kTol));

		std::vector<glm::vec3> v, n;
		ReferenceSkin(model, ref.final, v, n);
		const sAnimatedMesh& mesh = model.GetMesh(0);
		for (unsigned int i = 0; i < mesh.NumVertices; ++i)
		{
			TASK_CHECK(VecClose(mesh.pTransformedVertices[i], v[i], kTol));
			TASK_CHECK(VecClose(mesh.pTransformedNormals[i], n[i], kTol));
		}

		// The mirrored pose must genuinely differ from the original motion.
		SkeletalModel twin;
		BuildMainRig(twin);
		twin.Update(0.7f);
		TASK_CHECK(AnyFinalDiffers(model, twin, 0.02f));
		return 0;
	}

	int CaseMirrorInvolution()
	{
		std::vector<unsigned int> partners = MainPartners();
		glm::vec3 normal = MainNormal();
		std::vector<float> ones(4, 1.0f);

		SkeletalModel a; // original motion
		BuildMainRig(a);
		a.Update(0.7f);
		std::vector<glm::mat4x4> origFinals = SnapshotFinals(a);
		std::vector<glm::vec3> ov, on;
		SnapshotMesh(a, ov, on);

		SkeletalModel b; // mirrored once
		BuildMainRig(b);
		TASK_CHECK(b.ConfigureMirror(partners, normal));
		b.Update(0.7f);

		// Feed the mirrored pose back as a static animation and mirror again:
		// geometrically mu(mu(K)) == K, so c must reproduce a's original pose.
		RefPose ref = ReferencePose(b, partners, normal, ones, 0.7f);
		SkeletalModel c;
		BuildMainRigSkeleton(c);
		SetSingleKeyAnimation(c, ref.mirT, ref.mirR);
		TASK_CHECK(c.ConfigureMirror(partners, normal));
		c.Update(0.7f);

		TASK_CHECK(FinalsMatch(c, origFinals, kTol));
		TASK_CHECK(MeshMatches(c, ov, on, kTol));
		return 0;
	}

	int CaseRestPoseInvariant()
	{
		std::vector<unsigned int> partners = MainPartners();
		glm::vec3 normal = MainNormal();

		SkeletalModel model; // no animation channels at all: the pose is the bind pose
		BuildMainRigSkeleton(model);
		model.GetAnimation().Duration = 1.0f;
		model.GetAnimation().TicksPerSecond = 1.0f;
		TASK_CHECK(model.ConfigureMirror(partners, normal));
		model.Update(0.4f);

		SkeletalModel twin;
		BuildMainRigSkeleton(twin);
		twin.GetAnimation().Duration = 1.0f;
		twin.GetAnimation().TicksPerSecond = 1.0f;
		twin.Update(0.4f);

		TASK_CHECK(FinalsMatch(model, SnapshotFinals(twin), kPathTol));
		std::vector<glm::vec3> tv, tn;
		SnapshotMesh(twin, tv, tn);
		TASK_CHECK(MeshMatches(model, tv, tn, kPathTol));

		// With Offset == inverse(B), the rest pose gives Final == I exactly.
		for (unsigned int i = 0; i < 4; ++i)
			TASK_CHECK(MatClose(model.GetSkeleton().Bones[i].FinalTransformation, model.GetGlobalInverseTransform(), kTol));
		return 0;
	}

	int CasePartialWeightsLocalBlend()
	{
		SkeletalModel model;
		BuildMainRig(model);
		std::vector<unsigned int> partners = MainPartners();
		glm::vec3 normal = MainNormal();
		TASK_CHECK(model.ConfigureMirror(partners, normal));
		std::vector<float> weights;
		weights.push_back(1.0f);  // hips
		weights.push_back(0.5f);  // arm.up: parent and child differ
		weights.push_back(0.25f); // arm.lo
		weights.push_back(0.0f);  // spine keeps its original local pose
		TASK_CHECK(model.SetMirrorWeights(weights));
		model.Update(0.7f);

		RefPose ref = ReferencePose(model, partners, normal, weights, 0.7f);
		for (unsigned int i = 0; i < 4; ++i)
			TASK_CHECK(MatClose(model.GetSkeleton().Bones[i].FinalTransformation, ref.final[i], kTol));

		std::vector<glm::vec3> v, n;
		ReferenceSkin(model, ref.final, v, n);
		TASK_CHECK(MeshMatches(model, v, n, kTol));

		// The blend is per-bone: this is neither the full mirror nor the original.
		std::vector<float> ones(4, 1.0f);
		RefPose full = ReferencePose(model, partners, normal, ones, 0.7f);
		TASK_CHECK(!MatClose(model.GetSkeleton().Bones[2].FinalTransformation, full.final[2], 0.02f));
		return 0;
	}

	int CaseReconfigureRecaptures()
	{
		SkeletalModel model;
		BuildMainRig(model);
		std::vector<unsigned int> partners = MainPartners();
		glm::vec3 n1 = MainNormal();
		TASK_CHECK(model.ConfigureMirror(partners, n1));
		std::vector<float> w03(4, 0.3f);
		TASK_CHECK(model.SetMirrorWeights(w03));
		model.Update(0.7f);
		RefPose ref1 = ReferencePose(model, partners, n1, w03, 0.7f);
		TASK_CHECK(FinalsMatch(model, ref1.final, kTol));

		// Reconfiguring captures a fresh reference and resets the weights to 1.
		glm::vec3 n2 = glm::normalize(glm::vec3(0.15f, 0.6f, 1.0f));
		TASK_CHECK(model.ConfigureMirror(partners, n2));
		model.Update(0.0f);
		std::vector<float> ones(4, 1.0f);
		RefPose ref2 = ReferencePose(model, partners, n2, ones, 0.7f);
		TASK_CHECK(FinalsMatch(model, ref2.final, kTol));
		std::vector<glm::vec3> v, n;
		ReferenceSkin(model, ref2.final, v, n);
		TASK_CHECK(MeshMatches(model, v, n, kTol));
		return 0;
	}

	// ------------------------------------------------------------------
	// mixed_hierarchy
	// ------------------------------------------------------------------

	int CaseNonParentFirstStorage()
	{
		SkeletalModel model;
		BuildShuffledRig(model);
		std::vector<unsigned int> partners = MainPartners(); // root<->twist, hand<->arm
		glm::vec3 normal = glm::normalize(glm::vec3(0.25f, 1.0f, 0.45f));
		TASK_CHECK(model.ConfigureMirror(partners, normal));
		std::vector<float> weights;
		weights.push_back(0.8f);
		weights.push_back(1.0f);
		weights.push_back(0.4f);
		weights.push_back(0.6f);
		TASK_CHECK(model.SetMirrorWeights(weights));
		model.Update(0.7f);

		RefPose ref = ReferencePose(model, partners, normal, weights, 0.7f);
		for (unsigned int i = 0; i < 4; ++i)
			TASK_CHECK(MatClose(model.GetSkeleton().Bones[i].FinalTransformation, ref.final[i], kTol));
		std::vector<glm::vec3> v, n;
		ReferenceSkin(model, ref.final, v, n);
		TASK_CHECK(MeshMatches(model, v, n, kTol));
		return 0;
	}

	int CaseSelfPairsAndMixedPairs()
	{
		SkeletalModel model;
		BuildSelfPairRig(model);
		std::vector<unsigned int> partners; // root, L.end and R.end mirror themselves
		partners.push_back(0);
		partners.push_back(2);
		partners.push_back(1);
		partners.push_back(3);
		partners.push_back(4);
		glm::vec3 normal = glm::normalize(glm::vec3(1.0f, 0.2f, 0.1f));
		TASK_CHECK(model.ConfigureMirror(partners, normal));
		std::vector<float> weights;
		weights.push_back(1.0f);
		weights.push_back(1.0f);
		weights.push_back(0.5f);
		weights.push_back(1.0f);
		weights.push_back(0.25f);
		TASK_CHECK(model.SetMirrorWeights(weights));
		model.Update(0.7f);

		RefPose ref = ReferencePose(model, partners, normal, weights, 0.7f);
		for (unsigned int i = 0; i < 5; ++i)
			TASK_CHECK(MatClose(model.GetSkeleton().Bones[i].FinalTransformation, ref.final[i], kTol));
		std::vector<glm::vec3> v, n;
		ReferenceSkin(model, ref.final, v, n);
		TASK_CHECK(MeshMatches(model, v, n, kTol));
		return 0;
	}

	int CaseInvalidTopologyRejected()
	{
		const glm::vec3 normal(0.0f, 0.0f, 1.0f);

		// A valid 2-bone control must configure successfully.
		{
			SkeletalModel m;
			const unsigned int kids[] = { 1 };
			AddBone(m, "root", glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), kids, 1);
			AddBone(m, "leaf", glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1, 0, 0, 0), NULL, 0);
			std::vector<unsigned int> p;
			p.push_back(0); p.push_back(1);
			TASK_CHECK(m.ConfigureMirror(p, normal));
		}
		// Empty skeleton.
		{
			SkeletalModel m;
			TASK_CHECK(!m.ConfigureMirror(std::vector<unsigned int>(), normal));
		}
		// Two roots.
		{
			SkeletalModel m;
			AddBone(m, "a", glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), NULL, 0);
			AddBone(m, "b", glm::vec3(1.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), NULL, 0);
			std::vector<unsigned int> p;
			p.push_back(0); p.push_back(1);
			TASK_CHECK(!m.ConfigureMirror(p, normal));
		}
		// A cycle detached from the root (1 <-> 2), so not all bones are reachable.
		{
			SkeletalModel m;
			const unsigned int c1[] = { 2 };
			const unsigned int c2[] = { 1 };
			AddBone(m, "root", glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), NULL, 0);
			AddBone(m, "x", glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1, 0, 0, 0), c1, 1);
			AddBone(m, "y", glm::vec3(0.0f, 2.0f, 0.0f), glm::quat(1, 0, 0, 0), c2, 1);
			std::vector<unsigned int> p;
			p.push_back(0); p.push_back(1); p.push_back(2);
			TASK_CHECK(!m.ConfigureMirror(p, normal));
		}
		// Duplicated child link.
		{
			SkeletalModel m;
			const unsigned int kids[] = { 1, 1 };
			AddBone(m, "root", glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), kids, 2);
			AddBone(m, "leaf", glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1, 0, 0, 0), NULL, 0);
			std::vector<unsigned int> p;
			p.push_back(0); p.push_back(1);
			TASK_CHECK(!m.ConfigureMirror(p, normal));
		}
		// Child index out of range.
		{
			SkeletalModel m;
			const unsigned int kids[] = { 5 };
			AddBone(m, "root", glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), kids, 1);
			AddBone(m, "leaf", glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1, 0, 0, 0), NULL, 0);
			std::vector<unsigned int> p;
			p.push_back(0); p.push_back(1);
			TASK_CHECK(!m.ConfigureMirror(p, normal));
		}
		// A bone parented to itself.
		{
			SkeletalModel m;
			const unsigned int kids[] = { 0 };
			AddBone(m, "root", glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), kids, 1);
			std::vector<unsigned int> p;
			p.push_back(0);
			TASK_CHECK(!m.ConfigureMirror(p, normal));
		}
		// Non-rigid bind reference (scaled NodeTransform).
		{
			SkeletalModel m;
			const unsigned int kids[] = { 1 };
			AddBone(m, "root", glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), kids, 1);
			AddBone(m, "leaf", glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1, 0, 0, 0), NULL, 0);
			m.GetSkeleton().Bones[1].NodeTransform = glm::scale(glm::vec3(2.0f, 1.0f, 1.0f));
			std::vector<unsigned int> p;
			p.push_back(0); p.push_back(1);
			TASK_CHECK(!m.ConfigureMirror(p, normal));
		}
		// Non-rigid global inverse.
		{
			SkeletalModel m;
			const unsigned int kids[] = { 1 };
			AddBone(m, "root", glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), kids, 1);
			AddBone(m, "leaf", glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1, 0, 0, 0), NULL, 0);
			m.SetGlobalInverseTransform(glm::scale(glm::vec3(2.0f, 2.0f, 2.0f)));
			std::vector<unsigned int> p;
			p.push_back(0); p.push_back(1);
			TASK_CHECK(!m.ConfigureMirror(p, normal));
		}
		return 0;
	}

	// ------------------------------------------------------------------
	// lifecycle
	// ------------------------------------------------------------------

	int CaseConfigureFailureAtomicity()
	{
		SkeletalModel model;
		BuildMainRig(model);
		std::vector<unsigned int> partners = MainPartners();
		glm::vec3 normal = MainNormal();
		TASK_CHECK(model.ConfigureMirror(partners, normal));
		std::vector<float> weights;
		weights.push_back(0.5f);
		weights.push_back(0.4f);
		weights.push_back(0.6f);
		weights.push_back(0.7f);
		TASK_CHECK(model.SetMirrorWeights(weights));
		model.Update(0.7f);
		std::vector<glm::mat4x4> pose = SnapshotFinals(model);
		std::vector<glm::vec3> pv, pn;
		SnapshotMesh(model, pv, pn);

		// Bad partner arrays.
		TASK_CHECK(!model.ConfigureMirror(std::vector<unsigned int>(3, 0), normal)); // wrong size
		unsigned int oob[] = { 3, 2, 1, 4 };       // index out of range
		TASK_CHECK(!model.ConfigureMirror(std::vector<unsigned int>(oob, oob + 4), normal));
		unsigned int dup[] = { 3, 2, 2, 0 };       // not a permutation
		TASK_CHECK(!model.ConfigureMirror(std::vector<unsigned int>(dup, dup + 4), normal));
		unsigned int cyc[] = { 1, 2, 3, 0 };       // permutation, not an involution
		TASK_CHECK(!model.ConfigureMirror(std::vector<unsigned int>(cyc, cyc + 4), normal));

		// Bad normals.
		TASK_CHECK(!model.ConfigureMirror(partners, glm::vec3(0.0f, 0.0f, 2.0f))); // non-unit
		TASK_CHECK(!model.ConfigureMirror(partners, glm::vec3(0.0f, 0.0f, 0.0f))); // zero
		float nan = std::numeric_limits<float>::quiet_NaN();
		float inf = std::numeric_limits<float>::infinity();
		TASK_CHECK(!model.ConfigureMirror(partners, glm::vec3(nan, 0.0f, 1.0f)));
		TASK_CHECK(!model.ConfigureMirror(partners, glm::vec3(inf, 0.0f, 0.0f)));

		// Bad weight arrays.
		TASK_CHECK(!model.SetMirrorWeights(std::vector<float>(3, 0.5f))); // wrong length
		std::vector<float> badW(4, 0.5f);
		badW[1] = nan;
		TASK_CHECK(!model.SetMirrorWeights(badW));
		badW[1] = -0.1f;
		TASK_CHECK(!model.SetMirrorWeights(badW));
		badW[1] = 1.1f;
		TASK_CHECK(!model.SetMirrorWeights(badW));
		badW[1] = inf;
		TASK_CHECK(!model.SetMirrorWeights(badW));

		// The last valid configuration and the visible pose are preserved.
		model.Update(0.0f);
		TASK_CHECK(FinalsMatch(model, pose, kTightTol));
		TASK_CHECK(MeshMatches(model, pv, pn, kTightTol));

		// ... and it is still the w = {0.5, 0.4, 0.6, 0.7} blend (weights kept).
		RefPose ref = ReferencePose(model, partners, normal, weights, 0.7f);
		TASK_CHECK(FinalsMatch(model, ref.final, kTol));
		return 0;
	}

	int CaseDisableAndReenable()
	{
		std::vector<unsigned int> partners = MainPartners();
		glm::vec3 normal = MainNormal();

		SkeletalModel a;
		BuildMainRig(a);
		TASK_CHECK(a.ConfigureMirror(partners, normal));
		a.Update(0.7f);
		std::vector<glm::mat4x4> mirrored = SnapshotFinals(a);

		SkeletalModel b; // unmirrored twin
		BuildMainRig(b);
		b.Update(0.7f);
		std::vector<glm::mat4x4> original = SnapshotFinals(b);
		std::vector<glm::vec3> bv, bn;
		SnapshotMesh(b, bv, bn);

		// Disabling restores the original update behaviour.
		a.DisableMirror();
		a.Update(0.0f);
		TASK_CHECK(FinalsMatch(a, original, kTightTol));
		TASK_CHECK(MeshMatches(a, bv, bn, kTightTol));

		// Re-enabling re-captures the same reference: identical results, no
		// accumulation of previously mirrored poses, across repeated cycles.
		for (int cycle = 0; cycle < 3; ++cycle)
		{
			TASK_CHECK(a.ConfigureMirror(partners, normal));
			a.Update(0.0f);
			TASK_CHECK(FinalsMatch(a, mirrored, kTightTol));
			a.DisableMirror();
			a.Update(0.0f);
			TASK_CHECK(FinalsMatch(a, original, kTightTol));
		}

		// DisableMirror on a never-configured model is a harmless no-op.
		SkeletalModel c;
		BuildMainRig(c);
		c.DisableMirror();
		c.Update(0.7f);
		TASK_CHECK(FinalsMatch(c, original, kTightTol));
		return 0;
	}

	int CaseZeroWeightsAndRepeatedUpdates()
	{
		std::vector<unsigned int> partners = MainPartners();
		glm::vec3 normal = MainNormal();

		SkeletalModel a;
		BuildMainRig(a);
		TASK_CHECK(a.ConfigureMirror(partners, normal));
		std::vector<float> zeros(4, 0.0f);
		TASK_CHECK(a.SetMirrorWeights(zeros));
		a.Update(0.7f);

		SkeletalModel b; // unmirrored twin
		BuildMainRig(b);
		b.Update(0.7f);

		// All-zero weights reproduce the original motion.
		TASK_CHECK(FinalsMatch(a, SnapshotFinals(b), kPathTol));
		std::vector<glm::vec3> bv, bn;
		SnapshotMesh(b, bv, bn);
		TASK_CHECK(MeshMatches(a, bv, bn, kPathTol));

		// Repeated Update(0) does not accumulate anything.
		std::vector<glm::mat4x4> p0 = SnapshotFinals(a);
		for (int i = 0; i < 3; ++i)
		{
			a.Update(0.0f);
			TASK_CHECK(FinalsMatch(a, p0, kTightTol));
		}

		// Switching to full weights applies the mirror exactly once per update.
		std::vector<float> ones(4, 1.0f);
		TASK_CHECK(a.SetMirrorWeights(ones));
		a.Update(0.0f);
		std::vector<glm::mat4x4> p1 = SnapshotFinals(a);
		RefPose ref = ReferencePose(a, partners, normal, ones, 0.7f);
		TASK_CHECK(FinalsMatch(a, ref.final, kTol));
		a.Update(0.0f);
		a.Update(0.0f);
		TASK_CHECK(FinalsMatch(a, p1, kTightTol));
		return 0;
	}

	int CaseInterleavedModels()
	{
		std::vector<unsigned int> partnersA = MainPartners();
		glm::vec3 normalA = MainNormal();
		std::vector<unsigned int> partnersB;
		partnersB.push_back(0); partnersB.push_back(1); partnersB.push_back(2); partnersB.push_back(3);
		glm::vec3 normalB = glm::normalize(glm::vec3(0.2f, 1.0f, 0.5f));
		std::vector<float> weightsB;
		weightsB.push_back(0.3f);
		weightsB.push_back(0.7f);
		weightsB.push_back(0.2f);
		weightsB.push_back(0.9f);

		SkeletalModel a, b;
		BuildMainRig(a);
		BuildMainRig(b);
		TASK_CHECK(a.ConfigureMirror(partnersA, normalA));
		TASK_CHECK(b.ConfigureMirror(partnersB, normalB));
		TASK_CHECK(b.SetMirrorWeights(weightsB));

		SkeletalModel a2, b2; // solo twins running the same update sequences
		BuildMainRig(a2);
		BuildMainRig(b2);
		TASK_CHECK(a2.ConfigureMirror(partnersA, normalA));
		TASK_CHECK(b2.ConfigureMirror(partnersB, normalB));
		TASK_CHECK(b2.SetMirrorWeights(weightsB));

		a.Update(0.3f);
		b.Update(0.5f);
		a.Update(0.2f);
		b.Update(0.25f);
		a.Update(0.1f);

		a2.Update(0.3f);
		a2.Update(0.2f);
		a2.Update(0.1f);
		b2.Update(0.5f);
		b2.Update(0.25f);

		TASK_CHECK(FinalsMatch(a, SnapshotFinals(a2), kTightTol));
		TASK_CHECK(FinalsMatch(b, SnapshotFinals(b2), kTightTol));
		std::vector<glm::vec3> v, n;
		SnapshotMesh(a2, v, n);
		TASK_CHECK(MeshMatches(a, v, n, kTightTol));
		SnapshotMesh(b2, v, n);
		TASK_CHECK(MeshMatches(b, v, n, kTightTol));

		// And a's pose is still the correct mirror of its own current motion.
		std::vector<float> ones(4, 1.0f);
		RefPose refA = ReferencePose(a, partnersA, normalA, ones, 0.6f);
		TASK_CHECK(FinalsMatch(a, refA.final, kTol));
		return 0;
	}

	int CaseClearResetsMirror()
	{
		std::vector<unsigned int> partners = MainPartners();
		glm::vec3 normal = MainNormal();

		SkeletalModel a;
		BuildMainRig(a);
		TASK_CHECK(a.ConfigureMirror(partners, normal));
		a.Update(0.7f);

		a.Clear();
		TASK_CHECK(a.GetNumMeshes() == 0);
		TASK_CHECK(a.GetSkeleton().Bones.empty());

		// The mirror configuration is gone: the rebuilt model plays the original motion.
		BuildMainRig(a);
		a.Update(0.7f);
		SkeletalModel b;
		BuildMainRig(b);
		b.Update(0.7f);
		TASK_CHECK(FinalsMatch(a, SnapshotFinals(b), kTightTol));
		TASK_CHECK(!a.SetMirrorWeights(std::vector<float>(4, 1.0f))); // nothing configured
		return 0;
	}
}

const task_case task_cases[] =
{
	{ "spaces", "full_weight_mirror_reference", CaseFullWeightMirrorReference },
	{ "spaces", "mirror_involution", CaseMirrorInvolution },
	{ "spaces", "rest_pose_invariant", CaseRestPoseInvariant },
	{ "spaces", "partial_weights_local_blend", CasePartialWeightsLocalBlend },
	{ "spaces", "reconfigure_recaptures", CaseReconfigureRecaptures },
	{ "mixed_hierarchy", "non_parent_first_storage", CaseNonParentFirstStorage },
	{ "mixed_hierarchy", "self_pairs_and_mixed_pairs", CaseSelfPairsAndMixedPairs },
	{ "mixed_hierarchy", "invalid_topology_rejected", CaseInvalidTopologyRejected },
	{ "lifecycle", "configure_failure_atomicity", CaseConfigureFailureAtomicity },
	{ "lifecycle", "disable_and_reenable", CaseDisableAndReenable },
	{ "lifecycle", "zero_weights_and_repeated_updates", CaseZeroWeightsAndRepeatedUpdates },
	{ "lifecycle", "interleaved_models", CaseInterleavedModels },
	{ "lifecycle", "clear_resets_mirror", CaseClearResetsMirror },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
