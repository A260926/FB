// Two-priority differential IK demo.
//
// Builds a small arm, asks a high-priority task to drive the tip upward along a
// direction the lower hinge cannot contribute to, and lets a low-priority task
// use the remaining freedom. Prints the residuals and asserts the actual skinned
// output, then prints DIFFERENTIAL_IK_DEMO_OK.

#include "SkeletalAnimation/DifferentialIK.h"
#include "SkeletalAnimation/SkeletalModel.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace SA;

namespace
{
	void InitBone(sBone& a_Bone, const char* a_Name, const glm::mat4x4& a_NodeTransform)
	{
		a_Bone.Name = a_Name;
		a_Bone.NodeTransform = a_NodeTransform;
		a_Bone.OffsetMatrix = glm::mat4x4(1);
		a_Bone.FinalTransformation = glm::mat4x4(1);
		a_Bone.NumWeights = 0;
		a_Bone.pWeights = NULL;
		a_Bone.NumChildren = 0;
		a_Bone.pChildren = NULL;
	}

	void SetChildren(sSkeleton& a_Skeleton, unsigned int a_Bone, const std::vector<unsigned int>& a_Children)
	{
		delete[] a_Skeleton.Bones[a_Bone].pChildren;
		a_Skeleton.Bones[a_Bone].NumChildren = static_cast<unsigned int>(a_Children.size());
		a_Skeleton.Bones[a_Bone].pChildren = a_Children.empty() ? NULL : new unsigned int[a_Children.size()];
		for (std::size_t i = 0; i < a_Children.size(); ++i)
			a_Skeleton.Bones[a_Bone].pChildren[i] = a_Children[i];
	}

	void BuildArm(SkeletalModel& a_Model)
	{
		sSkeleton& skeleton = a_Model.GetSkeleton();
		skeleton.Bones.resize(3);
		InitBone(skeleton.Bones[0], "root", glm::mat4x4(1));
		InitBone(skeleton.Bones[1], "upper", glm::translate(glm::vec3(1, 0, 0)));
		InitBone(skeleton.Bones[2], "lower", glm::translate(glm::vec3(1, 0, 0)));
		SetChildren(skeleton, 0, { 1 });
		SetChildren(skeleton, 1, { 2 });
		SetChildren(skeleton, 2, {});

		sAnimation& anim = a_Model.GetAnimation();
		anim.Duration = 100.0f;
		anim.TicksPerSecond = 0.0f;
		sNodeAnimation channel;
		channel.Name = "root";
		channel.PositionKeys.push_back({ glm::vec3(0), 0.0f });
		channel.RotationKeys.push_back({ glm::quat(1, 0, 0, 0), 0.0f });
		anim.NodeAnimations.push_back(channel);
	}

	bool Fail(const char* a_Message)
	{
		std::fprintf(stderr, "DIFFERENTIAL_IK_DEMO_FAIL: %s\n", a_Message);
		return false;
	}
}

int main()
{
	SkeletalModel model;
	BuildArm(model);

	// One vertex weighted to the tip bone so the report's end-effector and the
	// skinned output can be compared directly.
	const glm::vec3 tipLocal(1.0f, 0.0f, 0.0f);
	sBone& tipBone = model.GetSkeleton().Bones[2];
	tipBone.NumWeights = 1;
	tipBone.pWeights = new sWeight[1];
	tipBone.pWeights[0].VertexID = 0;
	tipBone.pWeights[0].Weight = 1.0f;

	sAnimatedMesh mesh = {};
	mesh.NumVertices = 1;
	mesh.pVertices = new glm::vec3[1]{ tipLocal };
	mesh.pNormals = new glm::vec3[1]{ glm::vec3(0, 1, 0) };
	mesh.pTransformedVertices = new glm::vec3[1];
	mesh.pTransformedNormals = new glm::vec3[1];
	mesh.NumIndices = 0;
	mesh.pIndices = NULL;
	model.AddMesh(mesh);

	std::vector<sIKHinge> hinges(2);
	hinges[0] = { 1, glm::vec3(0, 0, 1), -3.0f, 3.0f, 10.0f };  // swings the tip in y
	hinges[1] = { 2, glm::vec3(0, 1, 0), -3.0f, 3.0f, 10.0f };  // swings the tip in z
	if (!model.ConfigureIK(hinges))
		return Fail("ConfigureIK") ? 0 : 1;

	std::vector<sIKTask> tasks(2);
	// High task on the upper bone's tip: only hinge 0 can move it, so the high
	// layer fixes nu0 and leaves hinge 1 entirely free.
	tasks[0].BoneIndex = 1;
	tasks[0].LocalPoint = glm::vec3(1.0f, 0.0f, 0.0f);
	tasks[0].DesiredVelocity = glm::vec3(0.0f, 2.0f, 0.0f);
	tasks[0].Priority = 0;   // high
	tasks[0].Weight = 1.0f;
	// Low task on the lower bone's tip. It shares the chain with the high task,
	// so part of its velocity (y) is already fixed by the high layer; the low
	// layer fills in the rest (z) with the hinge the high layer left free.
	tasks[1].BoneIndex = 2;
	tasks[1].LocalPoint = tipLocal;
	tasks[1].DesiredVelocity = glm::vec3(0.0f, 4.0f, 1.5f);
	tasks[1].Priority = 1;   // low
	tasks[1].Weight = 1.0f;
	if (!model.SetIKTasks(tasks))
		return Fail("SetIKTasks") ? 0 : 1;

	const float dt = 0.1f;
	if (!model.UpdateIK(dt))
		return Fail("UpdateIK") ? 0 : 1;

	const sIKReport& report = model.GetIKReport();
	std::printf("nu=(%.6f, %.6f) theta=(%.6f, %.6f)\n",
	            report.AngularVelocity[0], report.AngularVelocity[1],
	            report.Angles[0], report.Angles[1]);
	std::printf("layer residual high=%.8f low=%.8f\n",
	            report.LayerResidual[0], report.LayerResidual[1]);

	// The high task is on the upper tip, one unit out: y velocity = nu0, so
	// nu0 = 2. The low task's y component is then fixed at 4; hinge 1 supplies
	// the remaining z velocity, nu1 = -1.5.
	if (std::fabs(report.AngularVelocity[0] - 2.0f) > 1e-4f)
		return Fail("high priority layer did not drive hinge 0") ? 0 : 1;
	if (std::fabs(report.AngularVelocity[1] + 1.5f) > 1e-4f)
		return Fail("low priority layer did not use the free hinge") ? 0 : 1;
	if (std::fabs(report.LayerResidual[0]) > 1e-4f)
		return Fail("high priority layer not satisfied") ? 0 : 1;
	if (std::fabs(report.LayerResidual[1]) > 1e-4f)
		return Fail("low priority layer not satisfied") ? 0 : 1;

	// The actual skinned vertex must equal the low task's reported position.
	const glm::vec3 skinned = model.GetMesh(0).pTransformedVertices[0];
	std::printf("skinned tip=(%.6f, %.6f, %.6f)\n", skinned.x, skinned.y, skinned.z);
	if (std::fabs(skinned.x - report.EndEffectorPosition[1].x) > 1e-4f ||
	    std::fabs(skinned.y - report.EndEffectorPosition[1].y) > 1e-4f ||
	    std::fabs(skinned.z - report.EndEffectorPosition[1].z) > 1e-4f)
		return Fail("skinned output does not match the reported end-effector") ? 0 : 1;

	// With theta already integrated, a zero-time call must not advance again.
	const glm::vec3 before = model.GetMesh(0).pTransformedVertices[0];
	if (!model.UpdateIK(0.0f))
		return Fail("zero-time UpdateIK") ? 0 : 1;
	if (model.GetIKReport().AngularVelocity[0] != 0.0f ||
	    model.GetIKReport().AngularVelocity[1] != 0.0f)
		return Fail("zero-time step integrated angular velocity") ? 0 : 1;
	if (glm::length(model.GetMesh(0).pTransformedVertices[0] - before) > 1e-6f)
		return Fail("zero-time step moved the mesh") ? 0 : 1;

	std::puts("DIFFERENTIAL_IK_DEMO_OK");
	return 0;
}
