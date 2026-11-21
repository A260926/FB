# Skeletal Animation and Software Skinning
The aim of this project is to provide simple skeletal animation and software skinning (on the CPU) in C++.

This project can be used by developers who want to quickly add simple animations to their project. For commercial software this project may be... lacking... but can be used as a base to improve upon.

## Features
Feature       | Yes          | No     | Notes
------------- | :----------: | :----: | ----
Model Loading | V            |        | By using Assimp
Skeletal Animation | V       |        |
Vertex Animation |           | X      |
Rendering     |              | X      |
CPU Skinning  | V            |        |
GPU Skinning  |              | X      |
Skinned Vertices | V         |        |
Skinned Normals |            | X      | To do
Materials     |              | X      | To do
Textures      |              | X      | To do
Multi-threaded  |            | X      | Will maybe add
Highly Optimized    |        | X      | 

## Dependencies
The SkeletalAnimation project is only dependent on [GLM](http://glm.g-truc.net/).

The AssimpConverter project is dependent on [Assimp](http://assimp.sourceforge.net/index.html) and SkeletalAnimation.

In order to compile the projects, both project files need to be modified to use the include files, and link with their respective dependencies.

## Usage
Make sure your project links with `SkeletalAnimation.lib`, or compile `SkeletalModel.cpp` into your project.

### Loading
If you do not have your own model loader you can use Assimp to load a model and use AssimpConverter to convert it to a `SkeletalModel` that can be rendered and animated:

```
void LoadModel()
{
	Assimp::Importer Importer;
	const aiScene* pScene = Importer.ReadFile("some_animated_model.fbx",
		aiProcess_LimitBoneWeights |
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_SortByPType);

	AssimpConverter::Convert(pScene, g_AnimatedModel);
}
```

### Animating
After the `SkeletalModel` has been loaded it needs to be updated every frame to make it animate:
```
void Update(float a_Dt) // a_Dt is the elapsed time since last frame in seconds
{
  g_AnimatedModel.Update(a_Dt);
}
```

### Differential IK (strict priority)
`SkeletalAnimation/DifferentialIK.h` adds an optional, opt-in control layer that
runs inside the real `Update()` and the CPU skinning path. The single action,
single-mesh skinning and on-disk format are unchanged.

```
std::vector<SA::sIKHinge> hinges = {
  { 1, glm::vec3(0, 0, 1), -1.2f, 1.2f, 6.0f }, // bone, unit local axis, min rad, max rad, max rad/s
};
g_AnimatedModel.ConfigureIK(hinges);            // <= 16 hinges, one per bone; resets theta to 0

std::vector<SA::sIKTask> tasks = {
  { 2, glm::vec3(1, 0, 0), glm::vec3(0, 2, 0), 0, 1.0f }, // high priority
  { 2, glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), 1, 1.0f }, // low priority
};
g_AnimatedModel.SetIKTasks(tasks);

g_AnimatedModel.Update(dt);                      // or UpdateIK(dt); dt in [0, 0.1]
const SA::sIKReport& r = g_AnimatedModel.GetIKReport();
```

How a step works:

* **Angle state.** Each character keeps a persistent per-hinge angle `theta`
  (radians). The correction is applied on the right of that frame's animated
  local rotation, `q_local = q_anim * angleAxis(theta, axis)`; local translation
  is untouched. This is why repeating, changing tasks or interleaving characters
  never feeds a previously corrected local matrix back into the animation source.
* **Feedforward.** With `dt > 0` the old and new action times are found by the
  original rule. FK runs at both times while holding the old `theta`, the
  end-effector difference (after `globalInverse`) is divided by `dt`, and that
  is `v_anim`. The velocity task is therefore `J*nu + v_anim = desired`.
* **Analytic Jacobian.** `J` is evaluated at the new action time with the old
  `theta`, in the full ancestor chain (bone axes and eccentric points included).
* **Units.** Angles (`MinAngle`, `MaxAngle`, `theta`) and angular velocities
  (`nu`, `MaxSpeed`, the report) are radians and radians/second. Desired
  velocities are model-space units/second. Note that the pre-existing animation
  keys and `glm::angleAxis` calls in the loader are in degrees, exactly as
  before; the IK layer converts the sampled keyframes to its own radian-space
  double quaternions.
* **Per-step bounds.** Every `nu` obeys
  `max(-MaxSpeed, (MinAngle-theta)/dt) <= nu <= min(MaxSpeed, (MaxAngle-theta)/dt)`.
  The per-step problem is therefore a bounded *linear* velocity problem: it does
  not promise that a nonlinear end-effector lands exactly on a target in one
  frame.
* **Strict priority.** Layers are solved in ascending `Priority`. Each layer
  minimizes `sum(weight * ||J*nu + v_anim - desired||^2)` subject to all higher
  layers remaining exactly optimal, and only then is the minimum-Euclidean-norm
  `nu` of the common optimal set chosen. Layers are never folded into one large
  weighted objective, and `nu` is never obtained by clamping an unconstrained
  solve. Duplicate / linearly dependent tasks, a zero Jacobian, unreachable
  velocities and joints already at their limits are all legal; relative singular
  values at or below `1e-9` of the largest are treated as zero rank.
* **Integration and skinning.** After the solve, `theta += dt * nu` happens
  once, then one full FK pass at the new action time rebuilds every
  `FinalTransformation` (`globalInverse` and `OffsetMatrix` applied once each)
  and the original `TransformVertices` runs.
* **Report.** `sIKReport` holds the step's angular velocities, the integrated
  angles, the linear residual per layer (computed from the pre-integration `J`,
  `v_anim` and `nu`, not from post-integration displacement) and the final
  end-effector positions. `dt == 0` re-samples the current action at the current
  `theta`, skins, reports zero angular velocity and advances nothing.

`ConfigureIK` fails (leaving configuration, time, `theta`, report and visible
mesh untouched) on invalid topology (bone 0 must be the unique root of a
spanning acyclic tree), duplicate hinge bones, invalid / non-finite axes, limits,
speeds or task parameters, and `UpdateIK` rejects a non-finite or out-of-range
`dt`. With no tasks `nu = 0` and the action advances normally. `DisableIK()` (and
`Clear()`) drops the control state; the next plain `Update(0)` restores the
current action, and configuring again starts from zero.

### Rendering
Here is some (very crude) example code to demonstrate how to render the animated model using OpenGL:
```
void Render()
{
	for (unsigned int i = 0; i < g_AnimatedModel.GetNumMeshes(); ++i)
	{
		const SA::sAnimatedMesh& AnimMesh = g_AnimatedModel.GetMesh(i);

		glBegin(GL_TRIANGLES);
		for (unsigned int i = 0; i < AnimMesh.NumIndices; ++i)
		{
			unsigned int Index = AnimMesh.pIndices[i];
			glm::vec3 n = AnimMesh.pNormals[Index];
			glm::vec3 v = AnimMesh.pTransformedVertices[Index];
			
			glColor4f(n.x, n.y, n.z, 1);
			glVertex3f(v.x, v.y, v.z);
		}
		glEnd();
	}
}
```

# References

http://ogldev.atspace.co.uk/www/tutorial38/tutorial38.html
