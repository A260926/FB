#pragma once

// Strict-priority differential inverse kinematics for SkeletalModel.
//
// The controller is opt-in: call ConfigureIK() once, then SetIKTasks() and
// UpdateIK() (or plain Update()) each frame. The original single action,
// single mesh skinning and on-disk format are untouched.

#include "Types.h"

#include <vector>

namespace SA
{
	// One hinge joint driven by the IK controller.
	//
	//   BoneIndex : index into sSkeleton::Bones. Each bone may own at most one
	//               hinge. The axis is a unit vector in the bone's own local
	//               space; the correction rotation is applied on the right of
	//               the animated local rotation, i.e.
	//                   q_local_corrected = q_anim * angleAxis(theta, axis)
	//               so a positive theta turns about +axis in this bone's space.
	//   MinAngle  : inclusive lower bound for theta, in radians (must be <= 0).
	//   MaxAngle  : inclusive upper bound for theta, in radians (must be >= 0).
	//   MaxSpeed  : strictly positive angular speed limit, radians per second.
	struct sIKHinge
	{
		unsigned int BoneIndex;
		glm::vec3    Axis;       // unit local axis
		float        MinAngle;   // radians, <= 0
		float        MaxAngle;   // radians, >= 0
		float        MaxSpeed;   // radians / second, > 0
	};

	// One end-effector velocity task.
	//
	//   BoneIndex        : bone the local point is attached to.
	//   LocalPoint       : point in the bone's local space (an eccentric point
	//                      offset from the bone origin is allowed).
	//   DesiredVelocity  : desired point velocity in model space (units/second).
	//   Priority         : non-negative integer. Lower layers are satisfied
	//                      first; within a layer tasks are weighted.
	//   Weight           : positive scalar weight inside its layer.
	struct sIKTask
	{
		unsigned int BoneIndex;
		glm::vec3    LocalPoint;
		glm::vec3    DesiredVelocity;
		unsigned int Priority;
		float        Weight;
	};

	// Snapshot of one UpdateIK step. All values refer to the step that was just
	// integrated (velocities and residuals use the pre-integration Jacobian).
	struct sIKReport
	{
		bool                            Valid;
		float                           Dt;             // seconds used this step
		float                           AnimationTime;  // new action time
		std::vector<float>              AngularVelocity; // nu, one per hinge, rad/s
		std::vector<float>              Angles;          // theta AFTER integration
		std::vector<float>              LayerResidual;   // sum(w*||J*nu+v_anim-v||^2) per layer
		std::vector<glm::vec3>          EndEffectorPosition; // model-space point after integration
	};

	// The controller itself is exposed as methods on SkeletalModel:
	//
	//   bool ConfigureIK(const std::vector<sIKHinge>&);
	//   bool SetIKTasks(const std::vector<sIKTask>&);
	//   bool UpdateIK(float a_Dt);
	//   void DisableIK();
	//   const sIKReport& GetIKReport();
	//
	// ConfigureIK copies at most 16 hinges, validates the topology and the
	// parameters, and resets this instance's theta to zero. Each bone may own at
	// most one hinge. SetIKTasks replaces and copies all tasks (it validates but
	// does not require a configured hinge set) while preserving theta; several
	// task points may share ancestors, and the same bone may be used with
	// different eccentric points. UpdateIK only accepts a configured state and a
	// finite 0 <= dt <= 0.1 s; dt == 0 re-samples the current action at the
	// current theta and skins without advancing time or theta (reported angular
	// velocity is zero). While enabled, the ordinary Update(dt) enters the same
	// control path; once disabled the original behaviour returns.

	class SkeletalModel;

	// Convenience free functions that forward to the model methods.
	bool             ConfigureIK(SkeletalModel& a_Model, const std::vector<sIKHinge>& a_Hinges);
	bool             SetIKTasks(SkeletalModel& a_Model, const std::vector<sIKTask>& a_Tasks);
	bool             UpdateIK(SkeletalModel& a_Model, float a_Dt);
	void             DisableIK(SkeletalModel& a_Model);
	const sIKReport& GetIKReport(const SkeletalModel& a_Model);
} // namespace SA
