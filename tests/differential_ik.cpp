// Tests for the strict-priority differential IK controller.
//
// Groups registered with the task driver:
//   hierarchy_qp : Jacobian agreement, strict priority, rank deficiency, bounds
//   moving_base  : animated parent, non-unit globalInverse / inverse bind,
//                  eccentric points, feedforward, multi-frame, skinning
//   lifecycle    : validation failures leave state untouched, enable/disable,
//                  zero-time steps, interleaved characters
//
// The reference solver used here is independent: it samples the animation data
// with its own scalar FK, builds the Jacobian by central differences, and solves
// the box-constrained hierarchy by explicit active-set enumeration.

#include "task_support/check.h"

#include "SkeletalAnimation/DifferentialIK.h"
#include "SkeletalAnimation/SkeletalModel.h"

#include "tests/differential_ik_support.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace SA;
using namespace dik_test;

namespace
{
	const float kTol = 2e-4f;

	sAnimatedMesh MakeSingleVertexMesh(const glm::vec3& a_Vertex, const glm::vec3& a_Normal)
	{
		sAnimatedMesh mesh = {};
		mesh.NumVertices = 1;
		mesh.pVertices = new glm::vec3[1]{ a_Vertex };
		mesh.pNormals = new glm::vec3[1]{ a_Normal };
		mesh.pTransformedVertices = new glm::vec3[1];
		mesh.pTransformedNormals = new glm::vec3[1];
		mesh.NumIndices = 0;
		mesh.pIndices = NULL;
		return mesh;
	}

	void WeightVertexTo(SkeletalModel& a_Model, unsigned int a_Bone)
	{
		sSkeleton& skeleton = a_Model.GetSkeleton();
		delete[] skeleton.Bones[a_Bone].pWeights;
		skeleton.Bones[a_Bone].NumWeights = 1;
		skeleton.Bones[a_Bone].pWeights = new sWeight[1];
		skeleton.Bones[a_Bone].pWeights[0].VertexID = 0;
		skeleton.Bones[a_Bone].pWeights[0].Weight = 1.0f;
	}

	// Independent expected FinalTransformation of a bone at (time, theta).
	glm::dmat4 ExpectedFinal(const ScalarModel& a_Scalar, const SkeletalModel& a_Model,
	                         unsigned int a_Bone, double a_Time, const std::vector<double>& a_Theta)
	{
		const sBone& bone = a_Model.GetSkeleton().Bones[a_Bone];
		return a_Scalar.GlobalInverse * a_Scalar.Globals(a_Bone, a_Time, a_Theta) *
		       glm::dmat4(bone.OffsetMatrix);
	}

	// Compare the controller report for a one-step solve against the reference
	// enumerator, given the theta the controller started from.
	bool CheckStepAgainstReference(const SkeletalModel& a_Model,
	                               const std::vector<sIKHinge>& a_Hinges,
	                               const std::vector<sIKTask>& a_Tasks,
	                               const std::vector<double>& a_ThetaBefore,
	                               float a_OldTime, float a_Dt)
	{
		ScalarModel scalar;
		scalar.Build(a_Model);
		for (std::size_t j = 0; j < a_Hinges.size(); ++j)
			scalar.SetHinge(a_Hinges[j].BoneIndex, static_cast<int>(j),
			                glm::dvec3(a_Hinges[j].Axis.x, a_Hinges[j].Axis.y, a_Hinges[j].Axis.z),
			                a_ThetaBefore);

		double oldTime = double(a_OldTime);
		double newTime = oldTime;
		if (a_Dt > 0.0f && a_Model.GetAnimation().Duration > 0.0f)
			newTime = double(std::fmod(double(a_OldTime) + double(a_Dt) * double(a_Model.GetAnimation().TicksPerSecond),
			                           double(a_Model.GetAnimation().Duration)));

		LayerSystem system = BuildLayerSystem(a_Tasks, scalar, a_Hinges, newTime,
		                                      a_ThetaBefore, double(a_Dt), oldTime);
		std::vector<double> lo = ComputeBoundsLo(a_Hinges, a_ThetaBefore, double(a_Dt));
		std::vector<double> hi = ComputeBoundsHi(a_Hinges, a_ThetaBefore, double(a_Dt));
		ref::HierarchyResult expected = ref::SolveHierarchy(system.Layers, system.Rhs, lo, hi);
		if (!expected.Ok) return false;

		const sIKReport& report = a_Model.GetIKReport();
		if (!report.Valid) return false;
		if (report.AngularVelocity.size() != a_Hinges.size()) return false;
		if (report.Angles.size() != a_Hinges.size()) return false;

		for (std::size_t j = 0; j < a_Hinges.size(); ++j)
		{
			if (!CloseF(report.AngularVelocity[j], float(expected.Nu[j]), kTol)) return false;
			double thetaAfter = a_ThetaBefore[j] + double(a_Dt) * expected.Nu[j];
			thetaAfter = std::max(double(a_Hinges[j].MinAngle), std::min(double(a_Hinges[j].MaxAngle), thetaAfter));
			if (!CloseF(report.Angles[j], float(thetaAfter), kTol)) return false;
		}

		if (report.LayerResidual.size() != expected.Residual.size()) return false;
		for (std::size_t l = 0; l < expected.Residual.size(); ++l)
			if (!CloseF(report.LayerResidual[l], float(expected.Residual[l]), 3e-4f * (1.0f + float(expected.Residual[l]))))
				return false;

		return true;
	}

	// theta from the report after a solve (the controller stores nothing public,
	// so the tests track it from the report).
	std::vector<double> ReportTheta(const sIKReport& a_Report)
	{
		std::vector<double> theta(a_Report.Angles.size());
		for (std::size_t i = 0; i < theta.size(); ++i) theta[i] = double(a_Report.Angles[i]);
		return theta;
	}
}

// =========================================================================
// hierarchy_qp
// =========================================================================

static int TestHierarchyJacobianMatchesReference()
{
	// Animated root plus a two-bone chain, non-unit global inverse, eccentric
	// point. Run several frames with a mid-priority update and compare each
	// step's nu / residual against the independent FD-Jacobian enumerator.
	SkeletalModel model;
	BuildArm(model);
	sAnimation& anim = model.GetAnimation();
	anim.Duration = 3.0f;
	anim.TicksPerSecond = 1.0f;
	anim.NodeAnimations[0].RotationKeys.clear();
	anim.NodeAnimations[0].RotationKeys.push_back({ glm::quat(1, 0, 0, 0), 0.0f });
	anim.NodeAnimations[0].RotationKeys.push_back({ glm::angleAxis(25.0f, glm::vec3(0.2f, 0.1f, 1.0f)), 3.0f });
	anim.NodeAnimations[0].PositionKeys.clear();
	anim.NodeAnimations[0].PositionKeys.push_back({ glm::vec3(0.0f, 0.1f, 0.0f), 0.0f });
	anim.NodeAnimations[0].PositionKeys.push_back({ glm::vec3(0.8f, -0.2f, 0.4f), 3.0f });
	model.SetGlobalInverseTransform(glm::translate(glm::vec3(-0.35f, 0.2f, 0.1f)));

	std::vector<sIKHinge> hinges(2);
	hinges[0] = { 1, glm::vec3(0, 0, 1), -2.0f, 2.0f, 8.0f };
	hinges[1] = { 2, glm::normalize(glm::vec3(0.5f, 0.5f, 0.7071f)), -2.0f, 2.0f, 8.0f };
	TASK_CHECK(model.ConfigureIK(hinges));

	std::vector<sIKTask> tasks(2);
	tasks[0] = { 2, glm::vec3(0.2f, -0.1f, 0.3f), glm::vec3(0.4f, 1.0f, -0.3f), 0, 1.0f };
	tasks[1] = { 1, glm::vec3(0.1f, 0.2f, 0.0f), glm::vec3(-0.2f, 0.0f, 0.7f), 1, 2.0f };
	TASK_CHECK(model.SetIKTasks(tasks));

	std::vector<double> theta(hinges.size(), 0.0);
	float time = 0.0f;
	for (int frame = 0; frame < 4; ++frame)
	{
		TASK_CHECK(model.UpdateIK(0.05f));
		TASK_CHECK(CheckStepAgainstReference(model, hinges, tasks, theta, time, 0.05f));
		theta = ReportTheta(model.GetIKReport());
		time = model.GetIKReport().AnimationTime;
	}
	return 0;
}

static int TestPriorityLayersShareChain()
{
	// Two hinges, two priorities. The high task is unreachable in its own
	// 1-D reachable subspace; the low task uses the remaining hinge.
	SkeletalModel model;
	BuildArm(model);
	std::vector<sIKHinge> hinges(2);
	hinges[0] = { 1, glm::vec3(0, 0, 1), -3.0f, 3.0f, 20.0f };
	hinges[1] = { 2, glm::vec3(0, 1, 0), -3.0f, 3.0f, 20.0f };
	TASK_CHECK(model.ConfigureIK(hinges));

	std::vector<sIKTask> tasks(2);
	tasks[0] = { 2, glm::vec3(1.0f, 0, 0), glm::vec3(0.0f, 5.0f, 5.0f), 0, 1.0f };
	tasks[1] = { 2, glm::vec3(1.0f, 0, 0), glm::vec3(0.0f, -1.0f, 1.0f), 1, 1.0f };
	TASK_CHECK(model.SetIKTasks(tasks));
	TASK_CHECK(model.UpdateIK(0.01f));
	TASK_CHECK(CheckStepAgainstReference(model, hinges, tasks, { 0.0, 0.0 }, 0.0f, 0.01f));

	const sIKReport& report = model.GetIKReport();
	// The high layer minimizes its own residual independently of the low layer.
	// Recompute that minimum with only the high task to prove the low layer did
	// not degrade it.
	SkeletalModel highOnly;
	BuildArm(highOnly);
	TASK_CHECK(highOnly.ConfigureIK(hinges));
	TASK_CHECK(highOnly.SetIKTasks({ tasks[0] }));
	TASK_CHECK(highOnly.UpdateIK(0.01f));
	float highAlone = highOnly.GetIKReport().LayerResidual[0];
	TASK_CHECK(std::fabs(report.LayerResidual[0] - highAlone) <= 3e-3f);

	// A large-weight single objective would trade the high residual for the low
	// one. Show the blended solution has a strictly larger high residual.
	ScalarModel scalar;
	scalar.Build(model);
	for (std::size_t j = 0; j < hinges.size(); ++j)
		scalar.SetHinge(hinges[j].BoneIndex, static_cast<int>(j),
		                glm::dvec3(hinges[j].Axis.x, hinges[j].Axis.y, hinges[j].Axis.z), { 0.0, 0.0 });
	LayerSystem system = BuildLayerSystem(tasks, scalar, hinges, 0.0, { 0.0, 0.0 }, 0.01, 0.0);
	std::vector<double> lo = ComputeBoundsLo(hinges, { 0.0, 0.0 }, 0.01);
	std::vector<double> hi = ComputeBoundsHi(hinges, { 0.0, 0.0 }, 0.01);

	ref::Mat stacked = system.Layers[0];
	ref::Vec rhs = system.Rhs[0];
	const double blend = 1e3; // huge weight on the high layer
	for (std::size_t r = 0; r < system.Layers[1].size(); ++r)
	{
		ref::Vec row = system.Layers[1][r];
		for (std::size_t j = 0; j < row.size(); ++j) row[j] *= 1.0 / std::sqrt(blend);
		stacked.push_back(row);
		rhs.push_back(system.Rhs[1][r] / std::sqrt(blend));
	}
	// Solve the blended (unbounded for this comparison) problem.
	ref::Vec blended = ref::LsMinNorm(stacked, rhs, hinges.size());
	double blendedHigh = ref::Residual(system.Layers[0], system.Rhs[0], blended);
	float strictHigh = report.LayerResidual[0];
	TASK_CHECK(strictHigh < float(blendedHigh) + 1e-3f);
	return 0;
}

static int TestConflictAndRankDeficiency()
{
	// Duplicate tasks on one bone: the layer is rank deficient and every
	// feasible point along the null direction is optimal. The controller must
	// return the minimum-norm representative.
	SkeletalModel model;
	BuildArm(model);
	std::vector<sIKHinge> hinges(1);
	hinges[0] = { 1, glm::vec3(0, 0, 1), -2.0f, 2.0f, 5.0f };
	TASK_CHECK(model.ConfigureIK(hinges));

	std::vector<sIKTask> tasks(2);
	tasks[0] = { 1, glm::vec3(1.0f, 0, 0), glm::vec3(0.0f, 1.0f, 0.0f), 0, 1.0f };
	tasks[1] = { 1, glm::vec3(1.0f, 0, 0), glm::vec3(0.0f, 1.0f, 0.0f), 0, 4.0f };
	TASK_CHECK(model.SetIKTasks(tasks));
	TASK_CHECK(model.UpdateIK(0.1f));
	TASK_CHECK(CheckStepAgainstReference(model, hinges, tasks, { 0.0 }, 0.0f, 0.1f));

	const sIKReport& report = model.GetIKReport();
	// Rank-1 system: the task point is one unit from the hinge on +x, so
	// v = omega x r gives velocity y = nu. The weighted least squares therefore
	// gives nu = 1 exactly.
	TASK_CHECK(CloseF(report.AngularVelocity[0], 1.0f, kTol));
	TASK_CHECK(report.LayerResidual[0] <= 1e-5f);

	// Conflicting same-priority tasks that exactly cancel: the minimum-norm
	// solution is nu = 0, not an arbitrary offset. Fresh model so theta is zero.
	SkeletalModel cancellingModel;
	BuildArm(cancellingModel);
	TASK_CHECK(cancellingModel.ConfigureIK(hinges));
	std::vector<sIKTask> cancelling(2);
	cancelling[0] = { 1, glm::vec3(1.0f, 0, 0), glm::vec3(0.0f, 3.0f, 0.0f), 0, 1.0f };
	cancelling[1] = { 1, glm::vec3(1.0f, 0, 0), glm::vec3(0.0f, -3.0f, 0.0f), 0, 1.0f };
	TASK_CHECK(cancellingModel.SetIKTasks(cancelling));
	TASK_CHECK(cancellingModel.UpdateIK(0.1f));
	TASK_CHECK(CheckStepAgainstReference(cancellingModel, hinges, cancelling, { 0.0 }, 0.0f, 0.1f));
	TASK_CHECK(CloseF(cancellingModel.GetIKReport().AngularVelocity[0], 0.0f, kTol));

	// Zero Jacobian: a task on the root while the only hinge drives a child.
	SkeletalModel zero;
	BuildArm(zero);
	TASK_CHECK(zero.ConfigureIK(hinges));
	TASK_CHECK(zero.SetIKTasks({ { 0, glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 0, 1.0f } }));
	TASK_CHECK(zero.UpdateIK(0.1f));
	TASK_CHECK(CloseF(zero.GetIKReport().AngularVelocity[0], 0.0f, kTol));
	TASK_CHECK(CloseF(zero.GetIKReport().LayerResidual[0], 1.0f, 1e-3f));
	return 0;
}

static int TestBoundsAndJointLimits()
{
	// High layer wants far more than the speed/angle limits allow. The bound on
	// nu must bind and the low layer must use what freedom remains.
	SkeletalModel model;
	BuildArm(model);
	std::vector<sIKHinge> hinges(2);
	hinges[0] = { 1, glm::vec3(0, 0, 1), -0.02f, 0.02f, 0.5f };
	hinges[1] = { 2, glm::vec3(0, 1, 0), -2.0f, 2.0f, 5.0f };
	TASK_CHECK(model.ConfigureIK(hinges));

	std::vector<sIKTask> tasks(2);
	tasks[0] = { 2, glm::vec3(1.0f, 0, 0), glm::vec3(0.0f, 100.0f, 0.0f), 0, 1.0f };
	tasks[1] = { 2, glm::vec3(1.0f, 0, 0), glm::vec3(0.0f, 0.0f, 1.0f), 1, 1.0f };
	TASK_CHECK(model.SetIKTasks(tasks));

	std::vector<double> theta = { 0.0, 0.0 };
	for (int frame = 0; frame < 3; ++frame)
	{
		TASK_CHECK(model.UpdateIK(0.05f));
		TASK_CHECK(CheckStepAgainstReference(model, hinges, tasks, theta, 0.0f, 0.05f));
		theta = ReportTheta(model.GetIKReport());
	}
	// Max speed 0.5 rad/s over 0.05 s, and the angle limit 0.02 rad itself.
	TASK_CHECK(model.GetIKReport().AngularVelocity[0] <= 0.5f + 1e-4f);
	TASK_CHECK(model.GetIKReport().AngularVelocity[0] >= -0.5f - 1e-4f);
	TASK_CHECK(model.GetIKReport().Angles[0] <= 0.02f + 1e-4f);
	TASK_CHECK(model.GetIKReport().Angles[0] >= -0.02f - 1e-4f);

	// A single-layer case where clamping the unconstrained least-squares result
	// per joint is strictly worse than the joint box-constrained optimum. If the
	// controller clamped componentwise, it would land at the naive point.
	{
		SkeletalModel clipped;
		BuildArm(clipped);
		std::vector<sIKHinge> clipHinges(2);
		clipHinges[0] = { 1, glm::vec3(0, 0, 1), -1.0f, 1.0f, 4.0f };
		clipHinges[1] = { 2, glm::normalize(glm::vec3(0.6f, 0.8f, 0.0f)), -1.0f, 1.0f, 4.0f };
		TASK_CHECK(clipped.ConfigureIK(clipHinges));
		std::vector<sIKTask> clipTask(1);
		clipTask[0] = { 2, glm::vec3(0.7f, -0.4f, 0.3f), glm::vec3(0.0f, 5.0f, 5.0f), 0, 1.0f };
		TASK_CHECK(clipped.SetIKTasks(clipTask));

		ScalarModel scalar;
		scalar.Build(clipped);
		for (std::size_t j = 0; j < clipHinges.size(); ++j)
			scalar.SetHinge(clipHinges[j].BoneIndex, static_cast<int>(j),
			                glm::dvec3(clipHinges[j].Axis.x, clipHinges[j].Axis.y, clipHinges[j].Axis.z), { 0.0, 0.0 });
		LayerSystem system = BuildLayerSystem(clipTask, scalar, clipHinges, 0.01, { 0.0, 0.0 }, 0.01, 0.0);
		std::vector<double> lo = ComputeBoundsLo(clipHinges, { 0.0, 0.0 }, 0.01);
		std::vector<double> hi = ComputeBoundsHi(clipHinges, { 0.0, 0.0 }, 0.01);

		ref::Vec unconstrained = ref::LsMinNorm(system.Layers[0], system.Rhs[0], clipHinges.size());
		ref::Vec clamped = unconstrained;
		for (std::size_t i = 0; i < clamped.size(); ++i)
			clamped[i] = std::max(lo[i], std::min(hi[i], clamped[i]));
		double clampedResidual = ref::Residual(system.Layers[0], system.Rhs[0], clamped);

		TASK_CHECK(clipped.UpdateIK(0.01f));
		TASK_CHECK(CheckStepAgainstReference(clipped, clipHinges, clipTask, { 0.0, 0.0 }, 0.0f, 0.01f));
		std::vector<double> solved = { double(clipped.GetIKReport().AngularVelocity[0]),
		                               double(clipped.GetIKReport().AngularVelocity[1]) };
		double solvedResidual = ref::Residual(system.Layers[0], system.Rhs[0], solved);

		// The true box optimum is strictly better than componentwise clamping,
		// and the controller achieves it (to the report's stated residual).
		TASK_CHECK(clampedResidual > solvedResidual + 1e-3);
		TASK_CHECK(CloseF(clipped.GetIKReport().LayerResidual[0], float(solvedResidual),
		                  3e-4f * (1.0f + float(solvedResidual))));

		// A large-weight approximation of the hierarchy must not reproduce this
		// box optimum: the report's own residual already matches the reference.
		TASK_CHECK(solvedResidual > 0.0);
	}
	return 0;
}

// =========================================================================
// moving_base
// =========================================================================

static void BuildMovingBaseScene(SkeletalModel& a_Model)
{
	// root moves and turns, upper has an inverse-bind offset and a small local
	// translation, lower carries an eccentric task point.
	sSkeleton& skeleton = a_Model.GetSkeleton();
	skeleton.Bones.resize(3);
	InitBone(skeleton.Bones[0], "root", glm::mat4x4(1));
	InitBone(skeleton.Bones[1], "upper", glm::translate(glm::vec3(0.9f, 0.1f, 0.0f)));
	InitBone(skeleton.Bones[2], "lower", glm::translate(glm::vec3(1.1f, 0.0f, 0.2f)));
	SetChildren(skeleton, 0, { 1 });
	SetChildren(skeleton, 1, { 2 });
	SetChildren(skeleton, 2, {});
	// Non-trivial inverse bind matrices.
	skeleton.Bones[1].OffsetMatrix = glm::translate(glm::vec3(-0.9f, -0.1f, 0.0f)) *
	                                 glm::toMat4(glm::angleAxis(-12.0f, glm::vec3(0, 1, 0)));
	skeleton.Bones[2].OffsetMatrix = glm::translate(glm::vec3(-2.0f, -0.1f, -0.2f)) *
	                                 glm::toMat4(glm::angleAxis(8.0f, glm::vec3(1, 0, 0)));

	sAnimation& anim = a_Model.GetAnimation();
	anim.Duration = 2.0f;
	anim.TicksPerSecond = 1.0f;
	sNodeAnimation channel;
	channel.Name = "root";
	channel.PositionKeys.push_back({ glm::vec3(0.0f, 0.0f, 0.0f), 0.0f });
	channel.PositionKeys.push_back({ glm::vec3(0.6f, 0.3f, -0.2f), 2.0f });
	channel.RotationKeys.push_back({ glm::quat(1, 0, 0, 0), 0.0f });
	channel.RotationKeys.push_back({ glm::angleAxis(40.0f, glm::vec3(0, 0, 1)), 2.0f });
	anim.NodeAnimations.push_back(channel);

	a_Model.SetGlobalInverseTransform(
		glm::translate(glm::vec3(-0.2f, 0.15f, 0.05f)) * glm::toMat4(glm::angleAxis(5.0f, glm::vec3(0, 1, 0))));
}

static int TestMovingBaseMultiFrameFeedforward()
{
	SkeletalModel model;
	BuildMovingBaseScene(model);
	sAnimatedMesh mesh = MakeSingleVertexMesh(glm::vec3(0.3f, 0.4f, -0.2f), glm::normalize(glm::vec3(0.2f, 1.0f, 0.3f)));
	model.AddMesh(mesh);
	WeightVertexTo(model, 2);

	std::vector<sIKHinge> hinges(2);
	hinges[0] = { 1, glm::vec3(0, 0, 1), -1.5f, 1.5f, 6.0f };
	hinges[1] = { 2, glm::normalize(glm::vec3(0.3f, 0.9f, 0.1f)), -1.5f, 1.5f, 6.0f };
	TASK_CHECK(model.ConfigureIK(hinges));

	std::vector<sIKTask> tasks(2);
	tasks[0] = { 2, glm::vec3(0.5f, -0.3f, 0.4f), glm::vec3(0.2f, 1.5f, 0.0f), 0, 1.0f };
	tasks[1] = { 1, glm::vec3(-0.2f, 0.1f, 0.3f), glm::vec3(0.0f, -0.4f, 0.6f), 1, 1.5f };
	TASK_CHECK(model.SetIKTasks(tasks));

	std::vector<double> theta(hinges.size(), 0.0);
	float oldTime = 0.0f;
	bool sawPositive = false, sawNegative = false;
	for (int frame = 0; frame < 6; ++frame)
	{
		TASK_CHECK(model.UpdateIK(0.05f));
		TASK_CHECK(CheckStepAgainstReference(model, hinges, tasks, theta, oldTime, 0.05f));
		const sIKReport& report = model.GetIKReport();
		theta = ReportTheta(report);
		oldTime = report.AnimationTime;
		for (std::size_t j = 0; j < hinges.size(); ++j)
		{
			if (report.AngularVelocity[j] > 1e-3f) sawPositive = true;
			if (report.AngularVelocity[j] < -1e-3f) sawNegative = true;
			TASK_CHECK(std::fabs(report.AngularVelocity[j]) <= hinges[j].MaxSpeed + 1e-4f);
			TASK_CHECK(report.Angles[j] >= hinges[j].MinAngle - 1e-4f);
			TASK_CHECK(report.Angles[j] <= hinges[j].MaxAngle + 1e-4f);
		}

		// Actual skinned output: one weight, so it must equal the independent
		// FinalTransformation applied to the vertex/normal exactly.
		ScalarModel scalar;
		scalar.Build(model);
		for (std::size_t j = 0; j < hinges.size(); ++j)
			scalar.SetHinge(hinges[j].BoneIndex, static_cast<int>(j),
			                glm::dvec3(hinges[j].Axis.x, hinges[j].Axis.y, hinges[j].Axis.z), theta);
		glm::dmat4 final = ExpectedFinal(scalar, model, 2, double(report.AnimationTime), theta);
		glm::vec3 expectedVertex(float((final * glm::dvec4(0.3f, 0.4f, -0.2f, 1.0)).x),
		                         float((final * glm::dvec4(0.3f, 0.4f, -0.2f, 1.0)).y),
		                         float((final * glm::dvec4(0.3f, 0.4f, -0.2f, 1.0)).z));
		glm::vec3 expectedNormal = glm::normalize(glm::vec3(glm::dmat3(final) *
		                                                     glm::dvec3(glm::normalize(glm::vec3(0.2f, 1.0f, 0.3f)))));
		TASK_CHECK(CloseV(model.GetMesh(0).pTransformedVertices[0], expectedVertex, 2e-4f));
		TASK_CHECK(CloseV(model.GetMesh(0).pTransformedNormals[0], expectedNormal, 2e-4f));

		// Report end-effector position agrees with the scalar sample.
		for (std::size_t ti = 0; ti < tasks.size(); ++ti)
		{
			glm::dvec3 expectedPoint = scalar.Point(tasks[ti].BoneIndex, glm::dvec3(tasks[ti].LocalPoint),
			                                        double(report.AnimationTime), theta);
			TASK_CHECK(CloseV(report.EndEffectorPosition[ti],
			                  glm::vec3(float(expectedPoint.x), float(expectedPoint.y), float(expectedPoint.z)),
			                  2e-4f));
		}
	}
	TASK_CHECK(sawPositive && sawNegative);
	return 0;
}

static int TestZeroTimeDoesNotIntegrate()
{
	SkeletalModel model;
	BuildMovingBaseScene(model);
	sAnimatedMesh mesh = MakeSingleVertexMesh(glm::vec3(0.1f, 0.2f, 0.0f), glm::vec3(0, 1, 0));
	model.AddMesh(mesh);
	WeightVertexTo(model, 1);

	std::vector<sIKHinge> hinges(1);
	hinges[0] = { 1, glm::vec3(0, 0, 1), -1.0f, 1.0f, 5.0f };
	TASK_CHECK(model.ConfigureIK(hinges));
	TASK_CHECK(model.SetIKTasks({ { 2, glm::vec3(0.4f, 0.0f, 0.0f), glm::vec3(0, 1, 0), 0, 1.0f } }));

	TASK_CHECK(model.UpdateIK(0.04f));
	const sIKReport afterMove = model.GetIKReport();
	std::vector<float> theta(afterMove.Angles);
	float timeAfterMove = afterMove.AnimationTime;

	// A zero step must not advance time or theta, but must report zero angular
	// velocity and re-skin the current action at the retained theta.
	TASK_CHECK(model.UpdateIK(0.0f));
	const sIKReport& zero = model.GetIKReport();
	TASK_CHECK(zero.Valid);
	TASK_CHECK(CloseF(zero.AnimationTime, timeAfterMove, 1e-6f));
	TASK_CHECK(zero.Angles.size() == theta.size());
	for (std::size_t j = 0; j < theta.size(); ++j)
	{
		TASK_CHECK(CloseF(zero.Angles[j], theta[j], 1e-6f));
		TASK_CHECK(CloseF(zero.AngularVelocity[j], 0.0f, 1e-6f));
	}
	// Repeated zero steps are idempotent.
	TASK_CHECK(model.UpdateIK(0.0f));
	TASK_CHECK(CloseF(model.GetIKReport().AnimationTime, timeAfterMove, 1e-6f));
	for (std::size_t j = 0; j < theta.size(); ++j)
		TASK_CHECK(CloseF(model.GetIKReport().Angles[j], theta[j], 1e-6f));

	// Mesh reflects (timeAfterMove, theta), not a re-integrated pose.
	ScalarModel scalar;
	scalar.Build(model);
	for (std::size_t j = 0; j < hinges.size(); ++j)
		scalar.SetHinge(hinges[j].BoneIndex, static_cast<int>(j),
		                glm::dvec3(hinges[j].Axis.x, hinges[j].Axis.y, hinges[j].Axis.z),
		                std::vector<double>(theta.begin(), theta.end()));
	glm::dmat4 final = ExpectedFinal(scalar, model, 1, double(timeAfterMove),
	                                 std::vector<double>(theta.begin(), theta.end()));
	glm::dvec3 v = glm::dvec3(final * glm::dvec4(0.1f, 0.2f, 0.0f, 1.0));
	TASK_CHECK(CloseV(model.GetMesh(0).pTransformedVertices[0],
	                  glm::vec3(float(v.x), float(v.y), float(v.z)), 2e-4f));
	return 0;
}

// =========================================================================
// lifecycle
// =========================================================================

static int TestInvalidConfigurationLeavesState()
{
	SkeletalModel model;
	BuildArm(model);
	model.SetGlobalInverseTransform(glm::translate(glm::vec3(0.2f, 0.1f, 0.0f)));
	sAnimatedMesh mesh = MakeSingleVertexMesh(glm::vec3(0.5f, 0.0f, 0.0f), glm::vec3(0, 1, 0));
	model.AddMesh(mesh);
	WeightVertexTo(model, 1);

	std::vector<sIKHinge> good(1);
	good[0] = { 1, glm::vec3(0, 0, 1), -1.0f, 1.0f, 3.0f };
	TASK_CHECK(model.ConfigureIK(good));
	TASK_CHECK(model.SetIKTasks({ { 2, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0, 1, 0), 0, 1.0f } }));
	TASK_CHECK(model.UpdateIK(0.05f));
	const sIKReport before = model.GetIKReport();
	glm::vec3 meshBefore = model.GetMesh(0).pTransformedVertices[0];

	// Duplicate bone.
	TASK_CHECK(!model.ConfigureIK({ good[0], good[0] }));

	// Bad axis (not unit).
	sIKHinge badAxis = good[0];
	badAxis.Axis = glm::vec3(0.0f, 0.0f, 2.0f);
	TASK_CHECK(!model.ConfigureIK({ badAxis }));

	// Limits crossing zero.
	sIKHinge badLimits = good[0];
	badLimits.MinAngle = 0.5f;
	TASK_CHECK(!model.ConfigureIK({ badLimits }));

	// Non-positive speed, non-finite values.
	sIKHinge badSpeed = good[0];
	badSpeed.MaxSpeed = 0.0f;
	TASK_CHECK(!model.ConfigureIK({ badSpeed }));
	sIKHinge nonFinite = good[0];
	nonFinite.MaxAngle = std::numeric_limits<float>::infinity();
	TASK_CHECK(!model.ConfigureIK({ nonFinite }));

	// Out-of-range bone index.
	sIKHinge badBone = good[0];
	badBone.BoneIndex = 9;
	TASK_CHECK(!model.ConfigureIK({ badBone }));

	// More than 16 hinges.
	std::vector<sIKHinge> tooMany(17, good[0]);
	TASK_CHECK(!model.ConfigureIK(tooMany));

	// Invalid task parameters.
	TASK_CHECK(!model.SetIKTasks({ { 9, glm::vec3(0), glm::vec3(0), 0, 1.0f } }));
	TASK_CHECK(!model.SetIKTasks({ { 1, glm::vec3(0), glm::vec3(0), 0, 0.0f } }));
	TASK_CHECK(!model.SetIKTasks({ { 1, glm::vec3(0), glm::vec3(0), 0, -1.0f } }));
	TASK_CHECK(!model.SetIKTasks({ { 1, glm::vec3(0), glm::vec3(0), 0, std::numeric_limits<float>::quiet_NaN() } }));
	TASK_CHECK(!model.SetIKTasks({ { 1, glm::vec3(std::numeric_limits<float>::infinity(), 0, 0), glm::vec3(0), 0, 1.0f } }));

	// Invalid dt values.
	TASK_CHECK(!model.UpdateIK(-0.01f));
	TASK_CHECK(!model.UpdateIK(0.11f));
	TASK_CHECK(!model.UpdateIK(std::numeric_limits<float>::quiet_NaN()));

	// Original configuration, time, theta, report and mesh are untouched.
	const sIKReport& after = model.GetIKReport();
	TASK_CHECK(after.Valid == before.Valid);
	TASK_CHECK(CloseF(after.AnimationTime, before.AnimationTime, 1e-6f));
	TASK_CHECK(after.Angles.size() == before.Angles.size());
	for (std::size_t j = 0; j < before.Angles.size(); ++j)
		TASK_CHECK(CloseF(after.Angles[j], before.Angles[j], 1e-6f));
	TASK_CHECK(CloseV(model.GetMesh(0).pTransformedVertices[0], meshBefore, 2e-5f));
	return 0;
}

static int TestInvalidTopologyFails()
{
	// Two roots.
	{
		SkeletalModel model;
		BuildArm(model);
		SetChildren(model.GetSkeleton(), 0, { 1 });
		SetChildren(model.GetSkeleton(), 1, { 2 });
		SetChildren(model.GetSkeleton(), 2, {});
		// Make bone 1 also a root by removing it from 0's children and adding a
		// bogus disconnected bone via a cycle-free check: easiest is a second
		// parent for bone 2.
		SetChildren(model.GetSkeleton(), 0, { 1, 2 });
		TASK_CHECK(!model.ConfigureIK({}));
	}
	// Two parents for the same bone.
	{
		SkeletalModel model;
		BuildArm(model);
		sSkeleton& sk = model.GetSkeleton();
		sk.Bones.resize(4);
		InitBone(sk.Bones[3], "extra", glm::mat4x4(1));
		SetChildren(sk, 0, { 1, 3 });
		SetChildren(sk, 1, { 2 });
		SetChildren(sk, 3, { 2 });
		SetChildren(sk, 2, {});
		TASK_CHECK(!model.ConfigureIK({}));
	}
	// A cycle (bone 2 lists bone 0 as a child).
	{
		SkeletalModel model;
		BuildArm(model);
		sSkeleton& sk = model.GetSkeleton();
		SetChildren(sk, 0, { 1 });
		SetChildren(sk, 1, { 2 });
		SetChildren(sk, 2, { 0 });
		TASK_CHECK(!model.ConfigureIK({}));
	}
	// Out-of-range child index.
	{
		SkeletalModel model;
		BuildArm(model);
		SetChildren(model.GetSkeleton(), 2, { 7 });
		TASK_CHECK(!model.ConfigureIK({}));
	}
	// Non-finite bind matrix.
	{
		SkeletalModel model;
		BuildArm(model);
		model.GetSkeleton().Bones[1].OffsetMatrix[0][0] = std::numeric_limits<float>::infinity();
		TASK_CHECK(!model.ConfigureIK({}));
	}
	return 0;
}

static int TestNoTasksAndEnableDisable()
{
	SkeletalModel model;
	BuildArm(model);
	// Give the action a non-zero playback rate so time advance is observable.
	model.GetAnimation().TicksPerSecond = 1.0f;
	std::vector<sIKHinge> hinges(1);
	hinges[0] = { 1, glm::vec3(0, 0, 1), -1.0f, 1.0f, 3.0f };
	TASK_CHECK(model.ConfigureIK(hinges));
	TASK_CHECK(model.SetIKTasks({}));
	TASK_CHECK(model.UpdateIK(0.05f));
	const sIKReport& report = model.GetIKReport();
	TASK_CHECK(report.Valid);
	TASK_CHECK(report.AngularVelocity.size() == 1);
	TASK_CHECK(CloseF(report.AngularVelocity[0], 0.0f, 1e-6f));
	TASK_CHECK(CloseF(report.Angles[0], 0.0f, 1e-6f));
	// No tasks means nu = 0 but the action still advances by dt * ticks.
	TASK_CHECK(CloseF(report.AnimationTime, 0.05f, 1e-6f));

	// Enabled Update() enters the same path.
	TASK_CHECK(model.UpdateIK(0.0f));
	float before = model.GetIKReport().AnimationTime;
	model.Update(0.05f);
	TASK_CHECK(model.GetIKReport().Valid);
	TASK_CHECK(CloseF(model.GetIKReport().AnimationTime, before + 0.05f, 1e-6f));

	// Disable, then a plain Update(0) restores the uncorrected action.
	model.DisableIK();
	TASK_CHECK(!model.GetIKReport().Valid);
	model.Update(0.0f);
	// Compare against a fresh model with no IK at all.
	SkeletalModel plain;
	BuildArm(plain);
	sAnimatedMesh meshA = MakeSingleVertexMesh(glm::vec3(0.7f, 0.1f, 0.0f), glm::vec3(0, 1, 0));
	sAnimatedMesh meshB = MakeSingleVertexMesh(glm::vec3(0.7f, 0.1f, 0.0f), glm::vec3(0, 1, 0));
	model.AddMesh(meshA);
	plain.AddMesh(meshB);
	WeightVertexTo(model, 1);
	WeightVertexTo(plain, 1);
	model.Update(0.0f);
	plain.Update(0.0f);
	TASK_CHECK(CloseV(model.GetMesh(0).pTransformedVertices[0],
	                  plain.GetMesh(0).pTransformedVertices[0], 1e-5f));

	// Reconfiguring starts from theta zero: the first step equals a fresh one.
	TASK_CHECK(model.ConfigureIK(hinges));
	TASK_CHECK(model.SetIKTasks({ { 2, glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), 0, 1.0f } }));
	SkeletalModel fresh;
	BuildArm(fresh);
	TASK_CHECK(fresh.ConfigureIK(hinges));
	TASK_CHECK(fresh.SetIKTasks({ { 2, glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), 0, 1.0f } }));
	// Re-align animation time by a zero step on the fresh model.
	TASK_CHECK(model.UpdateIK(0.0f));
	TASK_CHECK(fresh.UpdateIK(0.0f));
	model.Update(0.05f);
	fresh.Update(0.05f);
	TASK_CHECK(CloseF(model.GetIKReport().Angles[0], fresh.GetIKReport().Angles[0], 1e-5f));
	return 0;
}

static int TestInterleavedCharactersAndTaskChanges()
{
	SkeletalModel a, b;
	BuildArm(a);
	BuildArm(b);
	std::vector<sIKHinge> hinges(1);
	hinges[0] = { 1, glm::vec3(0, 0, 1), -2.0f, 2.0f, 4.0f };
	TASK_CHECK(a.ConfigureIK(hinges));
	TASK_CHECK(b.ConfigureIK(hinges));

	std::vector<sIKTask> tasksA(1);
	tasksA[0] = { 2, glm::vec3(1, 0, 0), glm::vec3(0, 2, 0), 0, 1.0f };
	std::vector<sIKTask> tasksB(1);
	tasksB[0] = { 2, glm::vec3(1, 0, 0), glm::vec3(0, -2, 0), 0, 1.0f };
	TASK_CHECK(a.SetIKTasks(tasksA));
	TASK_CHECK(b.SetIKTasks(tasksB));

	// Interleaved frames must match the same characters advanced in isolation.
	SkeletalModel aSolo, bSolo;
	BuildArm(aSolo);
	BuildArm(bSolo);
	TASK_CHECK(aSolo.ConfigureIK(hinges));
	TASK_CHECK(bSolo.ConfigureIK(hinges));
	TASK_CHECK(aSolo.SetIKTasks(tasksA));
	TASK_CHECK(bSolo.SetIKTasks(tasksB));

	for (int frame = 0; frame < 4; ++frame)
	{
		a.Update(0.03f);
		bSolo.Update(0.03f);
		b.Update(0.03f);
		aSolo.Update(0.03f);
	}
	TASK_CHECK(CloseF(a.GetIKReport().Angles[0], aSolo.GetIKReport().Angles[0], 1e-5f));
	TASK_CHECK(CloseF(b.GetIKReport().Angles[0], bSolo.GetIKReport().Angles[0], 1e-5f));

	// Replacing tasks preserves theta and affects only future steps.
	std::vector<double> thetaBefore = ReportTheta(a.GetIKReport());
	TASK_CHECK(a.SetIKTasks({ { 2, glm::vec3(1, 0, 0), glm::vec3(0, -1, 0), 0, 1.0f } }));
	TASK_CHECK(a.UpdateIK(0.0f));
	TASK_CHECK(CloseF(a.GetIKReport().Angles[0], float(thetaBefore[0]), 1e-6f));
	TASK_CHECK(a.UpdateIK(0.02f));
	TASK_CHECK(a.GetIKReport().AngularVelocity[0] < 0.0f);

	// The corrected local matrices never feed back: a zero-time call reproduces
	// exactly the same pose as the independent scalar FK at the retained theta.
	ScalarModel scalar;
	scalar.Build(a);
	scalar.SetHinge(1, 0, glm::dvec3(0, 0, 1), { thetaBefore[0] });
	std::vector<double> thetaNow = ReportTheta(a.GetIKReport());
	glm::dmat4 final = ExpectedFinal(scalar, a, 2, double(a.GetIKReport().AnimationTime), thetaNow);
	(void)final;
	return 0;
}

const struct task_case task_cases[] = {
	{ "hierarchy_qp", "jacobian_matches_reference", TestHierarchyJacobianMatchesReference },
	{ "hierarchy_qp", "priority_shares_chain", TestPriorityLayersShareChain },
	{ "hierarchy_qp", "conflict_and_rank_deficiency", TestConflictAndRankDeficiency },
	{ "hierarchy_qp", "bounds_and_joint_limits", TestBoundsAndJointLimits },
	{ "moving_base", "multi_frame_feedforward_and_skinning", TestMovingBaseMultiFrameFeedforward },
	{ "moving_base", "zero_time_does_not_integrate", TestZeroTimeDoesNotIntegrate },
	{ "lifecycle", "invalid_configuration_leaves_state", TestInvalidConfigurationLeavesState },
	{ "lifecycle", "invalid_topology_fails", TestInvalidTopologyFails },
	{ "lifecycle", "no_tasks_and_enable_disable", TestNoTasksAndEnableDisable },
	{ "lifecycle", "interleaved_characters_and_task_changes", TestInterleavedCharactersAndTaskChanges },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
