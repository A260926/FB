# Toy Path Tracer [![Build Status](https://github.com/aras-p/ToyPathTracer/workflows/build_and_test/badge.svg)](https://github.com/aras-p/ToyPathTracer/actions)

Toy path tracer I did in 2018 for my own learning purposes. Somewhat based on Peter Shirley's
[Ray Tracing in One Weekend](https://raytracing.github.io/) minibook (highly recommended!), and on Kevin Beason's
[smallpt](http://www.kevinbeason.com/smallpt/).

![Screenshot](/Shots/screenshot.jpg?raw=true "Screenshot")

I decided to write blog posts about things I discover as I do this:

* [Part 0: Intro](http://aras-p.info/blog/2018/03/28/Daily-Pathtracer-Part-0-Intro/)
* [Part 1: Initial C++ and walkthrough](http://aras-p.info/blog/2018/03/28/Daily-Pathtracer-Part-1-Initial-C--/)
* [Part 2: Fix stupid performance issue](http://aras-p.info/blog/2018/03/28/Daily-Pathtracer-Part-2-Fix-Stupid/)
* [Part 3: C#, Unity and Burst](http://aras-p.info/blog/2018/03/28/Daily-Pathtracer-Part-3-CSharp-Unity-Burst/)
* [Part 4: Correctness fixes and Mitsuba](http://aras-p.info/blog/2018/03/31/Daily-Pathtracer-Part-4-Fixes--Mitsuba/)
* [Part 5: simple GPU version via Metal](http://aras-p.info/blog/2018/04/03/Daily-Pathtracer-Part-5-Metal-GPU/)
* [Part 6: simple GPU version via D3D11](http://aras-p.info/blog/2018/04/04/Daily-Pathtracer-Part-6-D3D11-GPU/)
* [Part 7: initial C++ SIMD & SoA](http://aras-p.info/blog/2018/04/10/Daily-Pathtracer-Part-7-Initial-SIMD/)
* [Part 8: SSE SIMD for HitSpheres](http://aras-p.info/blog/2018/04/11/Daily-Pathtracer-8-SSE-HitSpheres/)
* [Part 9: ryg optimizes my code](http://aras-p.info/blog/2018/04/13/Daily-Pathtracer-9-A-wild-ryg-appears/)
* [Part 10: Update all implementations to match](http://aras-p.info/blog/2018/04/16/Daily-Pathtracer-10-Update-CsharpGPU/)
* [Part 11: Buffer-oriented approach on CPU](http://aras-p.info/blog/2018/04/19/Daily-Pathtracer-11-Buffer-Oriented/)
* [Part 12: Buffer-oriented approach on GPU D3D11](http://aras-p.info/blog/2018/04/25/Daily-Pathtracer-12-GPU-Buffer-Oriented-D3D11/)
* [Part 13: GPU thread group data optimization](http://aras-p.info/blog/2018/05/28/Pathtracer-13-GPU-threadgroup-memory-is-useful/)
* [Part 14: Make it run on iOS](http://aras-p.info/blog/2018/05/30/Pathtracer-14-iOS/)
* [Part 15: A bunch of path tracing links](http://aras-p.info/blog/2018/08/01/Pathtracer-15-Pause--Links/)
* [Part 16: Unity C# Burst optimization](http://aras-p.info/blog/2018/10/29/Pathtracer-16-Burst-SIMD-Optimization/)
* [Part 17: WebAssembly](http://aras-p.info/blog/2018/11/16/Pathtracer-17-WebAssembly/)

Note: it can only do spheres, no bounding volume hierachy of any sorts, a lot of stuff hardcoded.

Performance numbers in Mray/s on a scene with ~50 spheres and two light sources, running on the CPU:

| Language | Approach                | Ryzen 5950 | AMD TR1950 | MBP 2021   | MBP 2018  | MBA 2020 | iPhone 11 | iPhone X | iPhone SE |
|:--- |:---                          |        ---:|        ---:|        ---:|       ---:|      ---:|       ---:|      ---:|       ---:|
| C++ | SIMD Intrinsics              |   281.0    |   187.0    |   105.4    |   74.0    |     32.3 |      26.4 |     12.9 |       8.5 |
|     | Scalar                       |   141.2    |   100.0    |    84.8    |   35.7    |          |      15.9 |
|     | WebAssembly (no threads, no SIMD) | 8.4   |     5.0    |     8.1    |           |          |       5.6 |
| C#  | Unity Burst "manual" SIMD    |   227.2    |   133.0    |   103.7    |   60.0    |          |      29.7 |
|     | Unity Burst                  |            |    82.0    |            |   36.0    |          |           |
|     | Unity (Editor)               |     6.5    |            |     3.4    |           |          |           |
|     | Unity (player Mono)          |     6.7    |            |     3.5    |           |          |           |
|     | Unity (player IL2CPP)        |    39.1    |            |    63.8    |           |          |      17.2 |
|     | .NET 6.0                     |    91.5    |    53.0    |    40.9    |           |
|     | .NET Core 2.0                |    86.1    |    53.0    |            |   23.6    |
|     | Mono --llvm                  |            |            |    35.1    |   22.0    |
|     | Mono                         |    23.6    |            |     3.6    |    6.1    |

More detailed specs of the machines above are:
* `Ryzen 5950`: AMD Ryzen 5950X (3.4GHz, 16c/32t), Visual Studio 2022.
* `AMD TR1950`: AMD ThreadRipper 1950X (3.4GHz, SMT disabled - 16c/16t), Visual Studio 2017.
* `MBP 2021`: Apple MacBook Pro M1 Max (8+2 cores), Xcode 13.2.
* `MBP 2018`: Apple MacBook Pro mid-2018 (Core i9 2.9GHz, 6c/12t).
* `MBA 2020`: Apple MacBook Air 2020 (Core i7 1.2GHz, 4c/8t).
* `iPhone 11`: A13 chip.
* `iPhone X`: A11 chip.
* `iPhone SE`: A9 chip.

Software versions:
* Unity 2021.3.16. Burst 1.6.6 (safety checks off). C# testing in editor, Release mode.
* Mono 6.12.

And on the GPU, via a compute shader in D3D11 or Metal depending on the platform:

| GPU | Perf |
|:--- |  ---:|
| D3D11 | |
| GeForce RTX 3080Ti         | 3920 |
| GeForce GTX 1080Ti         | 1854 |
| Metal | |
| MBP 2024 (M4 Max)          | 1680 |
| MBP 2021 (M1 Max)          | 1065 |
| MBP 2018 (Radeon Pro 560X) | 246 |
| MBA 2020 (Iris Plus)       | 201 |
| iPhone 11 Pro (A13)        | 80 |
| iPhone X (A11)             | 46 |
| iPhone SE (A9)             | 20 |


A lot of stuff in the implementation is *totally* suboptimal or using the tech in a "wrong" way.
I know it's just a simple toy, ok :)

### Building

* C++ projects:
  * Windows (Visual Studio 2017) in `Cpp/Windows/ToyPathTracer.sln`. DX11 Win32 app that displays result as a fullscreen CPU-updated or GPU-rendered texture.
    Pressing G toggles between GPU and CPU tracing, A toggles animation, P toggles progressive accumulation.
  * Mac/iOS (Xcode 10) in `Cpp/Apple/ToyPathTracer.xcodeproj`. Metal app that displays result as a fullscreen CPU-updated or GPU-rendered texture.
    Pressing G toggles between GPU and CPU tracing, A toggles animation, P toggles progressive accumulation.
    Should work on both Mac (`Test Mac` target) and iOS (`Test iOS` target).
  * WebAssembly in `Cpp/Emscripten/build.sh`. CPU, single threaded, no SIMD.
* C# project in `Cs/TestCs.sln`. A command line app that renders some frames and dumps out final TGA screenshot at the end.
* Unity project in `Unity`. I used Unity 2021.3.16.

### Optional CPU irradiance cache

`Cpp/Source/IrradianceCache.{h,cpp}` adds an optional, explicitly biased
irradiance cache to the CPU preview. Nearby diffuse (Lambert) hits reuse the
hemispherical lighting already computed at a nearby record instead of running
their own hemisphere transport. Metal, dielectric and emissive materials, plus
the explicit sphere-light direct estimate, always use the production
`Trace`/`Scatter` path. The cache is **off by default**; when off, the random
stream, rendered image and `GetSceneDesc` object/material layout are unchanged.

A record stores the original sphere id, hit side (front/back), position `x`,
the unit normal `n` facing the incoming light, the remaining scatter count, the
mean probe radiance `H` (i.e. `E/pi`, with no local albedo baked in) and a
radius `R`. Records with different remaining path depths are never mixed.

Enabling and configuring it:

```cpp
#include "Cpp/Source/IrradianceCache.h"

ICConfig cfg;
cfg.enabled      = true;
cfg.gatherSamples = 32;     // positive integer: hemisphere probes per gather
cfg.tau          = 0.5f;    // error threshold, 0 < tau <= 1
cfg.minRadius    = 0.05f;   // 0 < minRadius <= maxRadius, both finite
cfg.maxRadius    = 0.5f;
cfg.seed         = 0x1234u; // gather RNG seed
if (!ConfigureIrradianceCache(cfg)) { /* invalid config: state is unchanged */ }

// Draw with the cache for this frame only:
DrawTest(time, frame, w, h, pixels, rays, kFlagProgressive | kFlagIrradianceCache);
```

An invalid configuration is rejected and leaves the current records untouched;
any accepted configuration change clears all records. `ClearIrradianceCache()`
clears manually. Camera movement keeps the cache; a sphere position/radius,
material or emissive change changes the scene signature and invalidates every
record before the next draw.

Querying, gathering and round semantics:

* A record is usable only for the same sphere id, side and remaining depth, and
  when the error
  `e = |x - xr|/Rr + sqrt(max(0, 1 - dot(n, nr)))` is below `tau`, and the
  finite connecting segment between the two points (each lifted along its own
  normal by `kMinT`) is not blocked by another sphere. The owning sphere is
  ignored during that visibility check; every other sphere blocks.
* The interpolation weight is `1/e - 1/tau`; if any record has `e == 0`, only
  the zero-error records are averaged.
* With no usable record the cache runs a real gather of `gatherSamples`
  cosine-weighted probes. Probe transport reuses the real intersection and
  recursive `Trace` transport with the cache internally disabled. A probe's
  direct first hit on an emissive sphere contributes no self-emission (the sky
  is kept); a light seen after a specular event or a real scatter keeps its
  contribution. The direct sphere-light estimate at the shading point is still
  evaluated exactly once, and reuse multiplies the current material albedo by
  `H` exactly once.
* The gather radius is
  `R = clamp(M / sum(1/d_j), minRadius, maxRadius)`, with `M = gatherSamples`,
  each first-segment distance `d_j` clamped to `[kMinT, maxRadius]` and an
  escape recorded as `maxRadius`. Later bounce distances never replace the
  first segment.
* Each draw freezes the readable set at start; candidates produced during the
  round are invisible until the next draw, when they are published in stable
  `(frame, pixel, sample, path depth)` order. The gather random stream is a
  pure function of that identity and the seed, never of worker thread number or
  insertion order. Forward, reverse and the default enkiTS row scheduling all
  produce identical records and linear RGB (`SetRowScheduleMode`).

Read-only and statistics access:

```cpp
size_t n = 0;
const ICRecord* recs = GetIrradianceCacheRecords(n); // readable records
ICStats stats;
GetIrradianceCacheStats(stats); // queries, hits, gathers, probe/connection rays, ...
```

A self-contained test world (`ICWorld_*`) lets tests set sphere parameters and
a sky and fire real single rays through the same production integrator; it does
not duplicate a second integrator. The example in
`examples/irradiance_cache.cpp` runs two real `DrawTest` rounds, writes
`irradiance.ppm` and prints the hit/gather counters. The test suite in
`tests/irradiance_cache.cpp` is built and run by `task_support/run.sh target`.
