[![logo](media/icon/ozz-grey-256.png)](http://guillaumeblanc.github.io/ozz-animation/)

ozz-animation
=============

open source c++ 3d skeletal animation library and toolset
---------------------------------------------------------

ozz-animation provides runtime character animation playback functionalities (loading, sampling, blending...). It proposes a low-level renderer agnostic and game-engine agnostic implementation, focusing on performance and memory constraints with a data-oriented design.

ozz-animation comes with the toolchain to convert from major Digital Content Creation formats (gltf, Fbx, Collada, Obj, 3ds, dxf) to ozz optimized runtime structures. Offline libraries are also provided to implement the conversion from any other animation and skeleton format.

Follow [this link](https://github.com/guillaumeblanc/ozz-animation/issues/175) for a list of projects using ozz-animation.

Documentation
-------------

Documentation and samples are available from [ozz-animation website](http://guillaumeblanc.github.io/ozz-animation/).

Supported platforms
-------------------

ozz-animation is tested on WebAssembly, Linux, macOS and Windows, for x86, x86-64 and ARM architectures. The run-time code (ozz_base, ozz_animation, ozz_geometry) depends only on c++17, on the C and the C++ standard libraries, and has no OS specific code. Portability to any other platform shouldn't be an issue.

Samples, tools and tests depend on external libraries (glfw, tinygltf, Fbx SDK, jsoncpp, gtest, ...), which aren't need to ship with ozz-animation runtime.

Build status
------------

|         | Linux  | macOS | Windows | WebAssembly |
| ------- | ------ | ------ | ------- | ------- |
| master  | [![Linux](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/linux.yml/badge.svg?branch=master)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/linux.yml) | [![macOS](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/macos.yml/badge.svg?branch=master)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/macos.yml) | [![Windows](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/windows.yml/badge.svg?branch=master)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/windows.yml) | [![WASM](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/wasm.yml/badge.svg?branch=master)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/wasm.yml) |
| develop | [![Linux](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/linux.yml/badge.svg?branch=develop)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/linux.yml) | [![macOS](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/macos.yml/badge.svg?branch=develop)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/macos.yml) | [![Windows](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/windows.yml/badge.svg?branch=develop)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/windows.yml) | [![WASM](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/wasm.yml/badge.svg?branch=develop)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/wasm.yml) |

The dashboard for all branches is available [here](http://guillaumeblanc.github.io/ozz-animation/documentation/dashboard/).

Contributing
------------

All contributions are welcome: code reviews, bug reports, bug fixes, samples, features, platforms, testing, documentation, optimizations...

Please read [CONTRIBUTING](CONTRIBUTING.md) file for more details about how to submit bugs or contribute to the code.

Multi-effector inverse kinematics
---------------------------------

`ozz::animation::IKMultiEffectorJob` (header
`include/ozz/animation/runtime/ik_multi_effector_job.h`) solves several
inverse-kinematics end points at once over a shared set of rotatable joints.
Running a single-effector solver once per hand lets the last hand drag the
shared ancestors away from the previous hand; the multi-effector job puts
every end point in a single optimization system, so effectors sharing an
ancestor, or several effectors attached to the same joint through different
local offsets, are all accounted for by every joint update.

### Target space

Each effector references a joint, a fixed offset in that joint **local
space**, and a target position in skeleton **model space**. The solved point
is the offset carried through the whole joint hierarchy into model space,
i.e. the same point a `LocalToModelJob` would produce for that offset. All
lengths are model units and every effector weight must be strictly positive.
The objective minimized within the angle bounds is

```
objective = sum_effector weight * |model_position(effector) - target|^2
          + lambda * sum_joint angle^2
```

with a strictly positive Tikhonov factor `lambda`.

### Angle convention

Every rotatable joint has a single degree of freedom: a normalized hinge
axis expressed in the joint local space and an angle interval
`[lower, upper]` in radians that must contain 0. The output local rotation is
the input local rotation right-multiplied by the hinge rotation:

```
output_rotation = input_rotation * Quaternion::FromAxisAngle(axis, angle)
```

The solver always starts from angle 0. Local translations and scales are
never modified (input scales must be unit), joints not listed as rotatable
stay fixed yet still participate in hierarchy propagation, and the invalid
padding lanes of the SoA pose are preserved bit for bit.

### Solver and termination report

The joint angles are found with a damped Gauss-Newton method (exact
geometric Jacobian, Cholesky normal equations, Levenberg-Marquardt damping
and backtracking), with a projected-gradient line search for active angle
bounds. Every accepted step strictly decreases the full objective; no global
optimum is promised for the non-convex problem, and degenerate straight
chains or unreachable targets stay finite instead of producing NaNs.

The reusable `IKMultiEffectorJob::Context` is preallocated with
`Prepare(skeleton, max_effectors)`; `Run` performs no heap allocation and
supports at most 64 rotatable joints and 32 effectors. The `Report` exposes:

- `initial_objective` / `final_objective`: objective at angle 0 and at the
  returned pose,
- `iteration_objectives` (a caller span holding at least `max_iterations + 1`
  values) plus `iteration_count`: the objective after each accepted
  iteration, a non increasing sequence starting at the initial value,
- `residuals`: the model-space `position - target` vector of every effector,
- `projected_gradient_norm`: infinity norm of the bound-projected gradient,
- `termination`: `kConverged` only when that norm is below `tolerance`
  (residual distances alone never declare convergence), otherwise
  `kIterationLimit`, in which case the best finite pose is still returned.

Invalid parameters, an unprepared context or a skeleton mismatch make `Run`
return `false` without touching the output pose or report; a legal problem
that merely fails to converge is not a parameter error.

License
-------

ozz-animation is hosted on [github](http://github.com/guillaumeblanc/ozz-animation/) and distributed under the **MIT License** (MIT).
