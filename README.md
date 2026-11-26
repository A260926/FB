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

Dual quaternion skinning
------------------------

`ozz::geometry::SkinningJob` supports the historical linear matrix palette
skinning path and an opt-in dual quaternion path:

```cpp
ozz::geometry::SkinningJob job;
job.skinning_mode = ozz::geometry::SkinningJob::kDualQuaternion;
```

The default is `kLinear`; existing callers that do not set `skinning_mode` keep
the original validation and numerical behavior.

`joint_matrices` are final skinning matrices in model/character space. Each
matrix is normally `model_space_joint * inverse_bind_pose`, so input vertices
are mesh/bind-space positions and the resulting positions are in model space.
They are not raw local-space joint transforms. Compact palettes are supported:
`joint_indices` index `joint_matrices` directly and do not need to equal
skeleton joint indices.

The dual quaternion path is selected independently for every vertex. It is
used only when every influence with a strictly positive weight references a
finite rigid matrix: a `(0, 0, 0, 1)` last row, three unit and pairwise
orthogonal linear columns, and determinant `+1`, each checked with absolute
tolerance `1e-4`. Positive-weight uniform or non-uniform scale, shear or
reflection makes that vertex use the legacy linear formula for positions,
normals and tangents. Other vertices in the same mesh or palette can still use
dual quaternions. Zero-weight influences do not affect the rigid/linear
decision, although their indices must still be in range. When
`joint_inverse_transpose_matrices` is provided, linear fallback continues to use
them for normals and tangents; rigid dual quaternion normals and tangents use
the recovered rotation only, without translation or extra re-normalization.

In dual quaternion mode, all indices and explicit/restored weights are checked
before any output is written. Explicit and restored last weights must be finite
and in `[0, 1]`, explicit weights must sum to at most one, and every index must
reference the palette. Invalid data makes `Run()` return false without modifying
outputs. The dual quaternion `Run()` path does not allocate heap memory.

The no-window animation example is `examples/dq_skinning.cc`. It builds a
five-joint skeleton and animation, runs `SkeletonBuilder`, `AnimationBuilder`,
`SamplingJob` and `LocalToModelJob`, computes non-identity inverse-bind
matrices, remaps a compact palette, and prints position CSV for both modes.
With the support build:

```sh
cmake -S support -B .build -DCHECK_TARGET=ON
cmake --build .build --target dq_skinning -j 2
.build/dq_skinning
```

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

License
-------

ozz-animation is hosted on [github](http://github.com/guillaumeblanc/ozz-animation/) and distributed under the **MIT License** (MIT).
