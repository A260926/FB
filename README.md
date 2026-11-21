![OpenVDB](doc/img/banner.png)

| OpenVDB |   AX   |  Nano  | Houdini | License | CII |
| :----:  | :----: | :----: |  :----: | :-----: | :-: |
| [![core](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/build.yml/badge.svg)](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/build.yml) | [![ax](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/ax.yml/badge.svg)](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/ax.yml) | [![nano](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/nanovdb.yml/badge.svg)](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/nanovdb.yml) | [![hou](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/houdini.yml/badge.svg)](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/houdini.yml) | [![License](https://img.shields.io/github/license/AcademySoftwareFoundation/openvdb)](LICENSE) | [![CII Best Practices](https://bestpractices.coreinfrastructure.org/projects/2774/badge)](https://bestpractices.coreinfrastructure.org/projects/2774) |

-------------------------------------------------------------------------------

[Website](https://www.openvdb.org) |
[Discussion Forum](https://github.com/AcademySoftwareFoundation/openvdb/discussions) |
[Documentation](https://www.openvdb.org/documentation/doxygen) |
[Releases](https://github.com/AcademySoftwareFoundation/openvdb/releases) |
[License](https://www.apache.org/licenses/LICENSE-2.0) |
[Slack](https://slack.aswf.io/)

OpenVDB is an open source C++ library comprising a novel hierarchical data
structure and a large suite of tools for the efficient storage and manipulation
of sparse volumetric data discretized on three-dimensional grids. It was
developed by DreamWorks Animation for use in volumetric applications typically
encountered in feature film production.

### Development Repository

This GitHub repository hosts the trunk of the OpenVDB development. This implies
that it is the newest public version with the latest features and bug fixes.
However, it also means that it has not undergone a lot of testing and is
generally less stable than the [production releases](https://github.com/AcademySoftwareFoundation/openvdb/releases).

### License

OpenVDB is released under the [Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0),
which is a free, open source software license developed and maintained by the
Apache Software Foundation.

The trademarks of any contributor to this project may not be used in
association with the project without the contributor's express permission.

NOTE: OpenVDB has completed re-licensing from [Mozilla Public License Version 2.0](https://www.mozilla.org/MPL/2.0/) to [Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0). Please see the file RE-LICENSE_NOTE.txt for more details.

### Contributing

OpenVDB welcomes contributions to the OpenVDB project. Please refer to the
[contribution guidelines](CONTRIBUTING.md) for details on how to make a
contribution.

-------------------------------------------------------------------------------

### Developer Quick Start

The following provides basic installation examples for the core OpenVDB library.
Other components, such as the python module, OpenVDB AX, NanoVDB and various
executables, may require additional dependencies. See the
[build documentation](https://www.openvdb.org/documentation/doxygen/build.html)
for help with installations.

##### Linux/MacOS

```bash
# Linux
# @note If your distribution does not have required versions, consider using
#   apt pinning. See the dependency documentation for more details.
apt-get install -y libtbb-dev

# MacOS
# @note We are using homebrew in this example to install requried dependencies
#  https://brew.sh/
brew install tbb
```

```bash
git clone git@github.com:AcademySoftwareFoundation/openvdb.git
cd openvdb
mkdir build
cd build
cmake ..
make -j4 && make install
```

##### Windows

Note that the following commands have only been tested for 64bit systems/libraries.
It is recommended to set the `VCPKG_DEFAULT_TRIPLET` environment variable to
`x64-windows` to use 64-bit libraries by default. You will also require
[Visual Studio](https://visualstudio.microsoft.com/downloads/) (for the MSVC C++
runtime and compiler toolchains), [CMake](https://cmake.org/download/) and optionally
[vcpkg](https://github.com/microsoft/vcpkg) for the installation of OpenVDB's
dependencies.

```bash
vcpkg install zlib:x64-windows
vcpkg install tbb:x64-windows
```

```bash
git clone git@github.com:AcademySoftwareFoundation/openvdb.git
cd openvdb
mkdir build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE=<PATH_TO_VCPKG>\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -A x64 ..
cmake --build . --parallel 4 --config Release --target install
```

#### Building OpenVDB AX and NanoVDB

OpenVDB AX depends on the core OpenVDB library. NanoVDB can be built with and
without OpenVDB support. Note that NanoVDB has its own build instructions, see
the [NanoVDB build documentation](https://www.openvdb.org/documentation/doxygen/NanoVDB_HowToBuild.html)
for details.

The following variables can be passed to the `cmake` configure command. There
are more optional VDB components, see the [build documentation](https://www.openvdb.org/documentation/doxygen/build.html)
for a complete list.

| Option | Details |
| :----  | :-----  |
| `-D OPENVDB_BUILD_AX=ON`      | to enable OpenVDB AX |
| `-D OPENVDB_BUILD_NANOVDB=ON` | to enable NanoVDB |
| `-D NANOVDB_USE_OPENVDB=ON`   | to use OpenVDB in NanoVDB |

-------------------------------------------------------------------------------

### Fog adjoint tool (NanoVDB, CPU)

`nanovdb/nanovdb/tools/FogAdjoint.h` / `FogAdjoint.cc` implement a CPU fog
preview whose pixel RGB and transmittance errors can be back-propagated onto
the stored density parameters of a `nanovdb::FloatGrid`. Everything is
C++17 + the standard library + the bundled NanoVDB sources. The public
surface lives in `nanovdb::tools::fog`:

* `fog::Catalog` — the **parameter catalog** of a grid's active storage.
* `fog::forward(catalog, density, k, emission, rays, h, record*)` — the
  forward renderer with a **replaceable density vector**.
* `fog::Record` — an **owning record** of one forward pass.
* `fog::vjp(record, g, q)` — the analytic vector-Jacobian product.
* `examples/fit_fog.cpp` — a minimal density-fitting example;
  `tests/fog_adjoint.cpp` — the task tests.

#### Parameter identity

The catalog assigns **one scalar parameter to every active leaf voxel**
(level 0, value region of edge length 1) and **one to every active constant
tile** (levels 1/2/3, value regions of edge length 8/128/4096). Tiles are
*not* expanded into voxels. Parameters are numbered in ascending
`(level, origin.x, origin.y, origin.z)` order; `paramLevel(i)` and
`paramOrigin(i)` report the identity of parameter `i`, and
`initialDensity()` gathers the grid's active values in that same order.

* Active values of zero keep their parameter.
* Inactive values are always read as zero and have **no** parameter, even
  when the stored value is non-zero.
* The density vector handed to `forward` replaces the stored values but
  cannot change the active topology — the catalog alone defines which
  regions exist.
* Only grid maps of the form `world = scale * index + translate` with a
  positive uniform scale are supported (negative index coordinates are
  fine); other maps make the `Catalog` constructor throw
  `std::invalid_argument`.

#### Forward integration

One `forward` call takes the density vector, a global extinction `k`, an RGB
emission `e`, a batch of finite rays (each with a per-ray background) and a
world step `h`. Density, `k` and `e` must be finite and non-negative, `h`
finite and positive, ray directions non-zero and the `[t0, t1)` window
finite and increasing; violations throw `std::invalid_argument` before any
work is done.

For each ray `r(t) = o + t*d` the world length `D = (t1-t0)*|d|` is
partitioned from `t0` into segments of length `h`, the last segment taking
the remaining length `L <= h`. At every segment midpoint the density `rho`
is sampled **trilinearly**: integer coordinates are the sample nodes, the
octant weights come from the floor and fractional parts, and missing or
inactive corners contribute 0. With `a = exp(-rho*k*L)` the recurrence
`C += T * rho * e * (1-a)/(rho*k)`, `T *= a` is accumulated from `C = 0`,
`T = 1`; a zero denominator falls back to the continuous limit `L`. The
final color is `C + T * background`; the final `T` is returned alongside.
Integration is **not** clipped to the active bounding box (the one-cell
trilinear support halo also contributes) and never terminates early on a
transmittance threshold.

Empty space is skipped through a support hierarchy (a BVH over the
one-cell-dilated influence boxes of all parameters) whose size is
proportional to the active storage — no scene-spanning dense array is
built. Skipping only crosses samples whose eight corners are all inactive,
and sample indices are always computed from the original `t0`, so the
sampling phase is preserved exactly. `ForwardResult::sampleEvaluations`
counts the sample candidates actually evaluated and
`ForwardResult::hierarchyVisits` counts the real hierarchy node visits
(BVH nodes and corner-map probes).

#### Record ownership and the VJP

`forward` can fill a `fog::Record` with everything the backward pass needs:
per-segment corner parameter indices and trilinear weights, `rho`, segment
length, incoming transmittance, plus per-ray backgrounds, `k`, `e` and the
parameter count. The record **deep-copies** all of it, so it stays valid
after the source `GridHandle`, the catalog and the input vectors are
destroyed or modified. Records are move-only and fully independent:
`vjp` starts from zero on every call, and repeated or interleaved calls on
distinct records do not interfere. The cost of a call is linear in the
number of recorded segments and parameters.

`vjp(record, g, q)` returns the derivatives of `sum(g.RGB + q*T)` with
respect to the whole density vector, `k` and `e`. The propagation is fully
analytic — no parameter perturbation, no re-rendering, no automatic
differentiation library, and no division by the forward color or by `rho`
to bypass zero contributions. At `rho = 0` or `k = 0` the right
derivatives of the analytic continuous limits are used, including the
effect of extinction rising from zero on the emission of later segments.
A parameter referenced by several corners, samples or rays (e.g. one tile)
accumulates all of those contributions onto the same scalar; parameters
never touched by any sample receive exactly zero. All accumulation is done
in double precision.

#### Limitations of the fixed sampling scheme

* The integrator uses a **fixed world-space step** `h` with a midpoint
  rule: no adaptive refinement, no transmittance cutoff, and the sample
  phase is locked to `t0`. The VJP differentiates *this* discretized
  integral — including its segment partition — not the continuous volume
  integral, so gradients inherit the discretization error of the primal.
* Density is treated as constant within each segment (its trilinear
  midpoint value); the trilinear field itself is not integrated exactly.
* Only the density parameters, `k` and `e` are differentiated. Geometry,
  the active masks/topology, the camera, the step `h` and the sampling
  branches (which samples are evaluated) are all treated as constants.
* The grid map is restricted to a positive uniform scale plus translation.
* The implementation is CPU-only; the CUDA branch of
  `examples/ex_raytrace_fog_volume` keeps its original device integrator,
  while its CPU host path calls this same `fog::forward` (h=0.5, k=0.1,
  e=0, original `RayGenOp` and checkerboard background, PFM output).
