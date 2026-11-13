[![CLA assistant](https://cla-assistant.io/readme/badge/nfrechette/acl)](https://cla-assistant.io/nfrechette/acl)
[![All Contributors](https://img.shields.io/github/all-contributors/nfrechette/acl)](#contributors-)
[![Build status](https://github.com/nfrechette/acl/actions/workflows/build_push.yml/badge.svg)](https://github.com/nfrechette/acl/actions)
[![Sonar Status](https://sonarcloud.io/api/project_badges/measure?project=nfrechette_acl&metric=alert_status)](https://sonarcloud.io/dashboard?id=nfrechette_acl)
[![GitHub release](https://img.shields.io/github/release/nfrechette/acl.svg)](https://github.com/nfrechette/acl/releases)
[![GitHub license](https://img.shields.io/badge/license-MIT-blue.svg)](https://raw.githubusercontent.com/nfrechette/acl/master/LICENSE)
[![Conan](https://img.shields.io/badge/on-conan-blue.svg)](https://conan.io/center/recipes/nfrechette-acl)
[![Discord](https://img.shields.io/discord/691048241864769647?label=discord)](https://discord.gg/UERt4bS)

# Animation Compression Library

Animation compression is a fundamental aspect of modern video game engines. Not only is it important to keep the memory footprint down but it is also critical to keep the animation clip sampling performance fast.

The more memory an animation clip consumes, the slower it will be to sample it and extract a character pose at runtime. For these reasons, any game that attempts to push the boundaries of what the hardware can achieve will at some point need to implement some form of animation compression.

While some degree of compression can easily be achieved with simple tricks, achieving high compression ratios, fast decompression, while simultaneously not compromising the accuracy of the resulting compressed animation requires a great deal of care.

## Goals

This library has four primary goals:

*  Implement state of the art and production ready animation compression algorithms
*  Be easy to integrate into modern video game engines
*  Serve as a benchmark to compare various techniques against one another
*  Document what works and doesn't work

Algorithms are optimized with a focus on (in this particular order):

*  Minimizing the compression artifacts in order to reach high cinematographic quality
*  Fast decompression on all our supported hardware
*  A small memory footprint to lower memory pressure at runtime as well as reducing disk and network usage

Decompression speed will not be sacrificed for a smaller memory footprint nor will accuracy be compromised under any circumstances.

## Philosophy

Much thought was put into designing the library for it to be as flexible and powerful as possible. To this end, the following decisions were made:

*  The library consists of **100% C++11** header files and is thus easy to integrate in any game engine
*  [An intermediary clip format](./docs/the_acl_file_format.md) is supported in order to facilitate debugging and bug reporting
*  All allocations use a [game provided allocator](./includes/acl/core/iallocator.h)
*  All asserts use a [game provided macro](./includes/acl/core/error.h)

## Supported platforms

Continuous integration tests a variety of platforms and configurations but it generally runs as-is anywhere where C++11 (or later) is supported. CI currently tests:

*  Windows VS2022: x86, x64, ARM64, ARM64EC
*  Windows VS2026: x86, x64, ARM64, ARM64EC
*  Linux GCC 14+: x86, x64, ARMv7, ARM64
*  Linux Clang 22+: x86, x64, ARMv7, ARM64
*  OS X XCode 16+: ARM64
*  Emscripten 6.0.11: WASM (with and without SIMD)

Each releases is also manually tested on iOS and Android.

On RISC-V (`riscv64`), ACL uses the scalar code path of RTM because RTM has no SIMD code for RISC-V. CI does not test `riscv64`. `make.py -cpu riscv64` supports native builds on a Linux `riscv64` host. To cross-compile, use CMake with a RISC-V toolchain file.

The [Unreal Engine](https://www.unrealengine.com/en-US/blog) is supported through a plugin originally found [here](https://github.com/nfrechette/acl-ue4-plugin) although it is now distributed with a more recent version as part of each engine release since UE 5.13.

## Getting started

This library is **100%** headers as such you just need to include them in your own project to start using it. However, if you wish to run the unit tests, regression tests, to contribute to ACL or use it for research, head on over to the [getting started](./docs/getting_started.md) section in order to setup your environment and make sure to check out the [contributing guidelines](CONTRIBUTING.md).

If you would like to integrate ACL into your own game engine, follow the integration instructions [here](./docs#how-to-integrate-the-library).

You can install `nfrechette-acl` with [Conan](https://conan.io/center/recipes/nfrechette-acl).

## Performance metrics

*  [Carnegie-Mellon University database performance](./docs/cmu_performance.md)
*  [Paragon database performance](./docs/paragon_performance.md)
*  [Matinee fight scene performance](./docs/fight_scene_performance.md)
*  [Decompression performance](./docs/decompression_performance.md)

## Protected keyframes for whole keyframe stripping

Whole keyframe stripping can remove a sample and reconstruct it later by linear
interpolation of its neighbors. Some moments must stay in the bit stream, for
example a foot contact or a hand grab. The `compression_keyframe_stripping_settings`
structure takes a protected keyframe list for this purpose.

```cpp
const uint32_t protected_keyframes[] = { 4, 12, 20 };

acl::compression_settings settings = acl::get_default_compression_settings();
settings.error_metric = &metric;
settings.keyframe_stripping.proportion = 0.7F;
settings.keyframe_stripping.protected_keyframes = protected_keyframes;
settings.keyframe_stripping.num_protected_keyframes = 3;

acl::compressed_tracks* compressed = nullptr;
acl::output_stats stats;
acl::error_result result = acl::compress_track_list(allocator, tracks, settings, compressed, stats);
```

A protected keyframe stays a retained sample endpoint for the greedy removal
process. It can still act as an interpolation endpoint for other candidates, but
the compressor never removes it. The compressed format and the decompressor do
not change. The library stores no copy of the original animation, and it does not
re-insert a key pose after decompression. See `examples/protected_contacts.cpp`
for a complete program that writes the contact samples to a CSV file.

### Protection is not quantization precision

A protected keyframe only prevents whole keyframe removal. It does not raise the
quantization precision of the retained value. The sample still uses the normal
rotation, translation, and scale bit rates, so it keeps the ordinary quantization
error. Use the track `precision` and the error metric to control value accuracy;
use the protected list to control which samples survive whole keyframe stripping.

### Index space

The indices refer to the samples of the input track array, before compression and
before loop optimization. The list must be strictly increasing, it must contain
no duplicate entry, and every value must be smaller than the number of samples
per input track. When loop optimization removes the last sample because it
repeats the first sample, a protected index that points at the removed last
sample maps to the retained frame 0. No other index shifts. When loop
optimization does not remove a sample, no mapping happens. Two indices may map to
the same retained frame, this is legal.

### Borrowed memory lifetime

The caller owns the `protected_keyframes` buffer. The compressor reads it only
during the synchronous `compress_track_list` call, and it does not retain the
pointer after the call returns. The buffer must stay valid for the duration of
that call. A non-zero count with a null pointer fails with an error result. When
the count is zero, the pointer is ignored and the behavior matches the legacy
behavior, including the settings hash and the compressed bytes.

### Database support limitation

A non-empty protected list cannot be combined with `enable_database_support`.
This revision does not extend the database tiering to account for protected
samples, so that combination returns an error result and produces no output. An
empty protected list does not add this restriction. Scalar tracks also reject a
non-empty protected list because whole keyframe stripping applies to transform
tracks only.

## External dependencies

You don't need anything else to get started: everything is self contained.
See [here](./external) for details.

## License, copyright, and code of conduct

This project uses the [MIT license](LICENSE).

Copyright (c) 2017 Nicholas Frechette & Animation Compression Library contributors

Please note that this project is released with a [Contributor Code of Conduct](CODE_OF_CONDUCT.md). By participating in this project you agree to abide by its terms.


## Contributors ✨

Thanks goes to these wonderful people ([emoji key](https://allcontributors.org/docs/en/emoji-key)):

<!-- ALL-CONTRIBUTORS-LIST:START - Do not remove or modify this section -->
<!-- prettier-ignore-start -->
<!-- markdownlint-disable -->
<table>
  <tbody>
    <tr>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/CodyDWJones"><img src="https://avatars.githubusercontent.com/u/28773740?v=4?s=100" width="100px;" alt="CodyDWJones"/><br /><sub><b>CodyDWJones</b></sub></a><br /><a href="https://github.com/nfrechette/acl/commits?author=CodyDWJones" title="Code">💻</a> <a href="#data-CodyDWJones" title="Data">🔣</a> <a href="#maintenance-CodyDWJones" title="Maintenance">🚧</a> <a href="#tool-CodyDWJones" title="Tools">🔧</a> <a href="#infra-CodyDWJones" title="Infrastructure (Hosting, Build-Tools, etc)">🚇</a> <a href="#ideas-CodyDWJones" title="Ideas, Planning, & Feedback">🤔</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/Meradrin"><img src="https://avatars.githubusercontent.com/u/7066278?v=4?s=100" width="100px;" alt="Meradrin"/><br /><sub><b>Meradrin</b></sub></a><br /><a href="https://github.com/nfrechette/acl/commits?author=Meradrin" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/tirpidz"><img src="https://avatars.githubusercontent.com/u/9991876?v=4?s=100" width="100px;" alt="Martin Turcotte"/><br /><sub><b>Martin Turcotte</b></sub></a><br /><a href="https://github.com/nfrechette/acl/commits?author=tirpidz" title="Code">💻</a> <a href="#tool-tirpidz" title="Tools">🔧</a> <a href="#ideas-tirpidz" title="Ideas, Planning, & Feedback">🤔</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/vjeffh"><img src="https://avatars.githubusercontent.com/u/22382688?v=4?s=100" width="100px;" alt="vjeffh"/><br /><sub><b>vjeffh</b></sub></a><br /><a href="https://github.com/nfrechette/acl/commits?author=vjeffh" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/Romain-Piquois"><img src="https://avatars.githubusercontent.com/u/3689912?v=4?s=100" width="100px;" alt="Romain-Piquois"/><br /><sub><b>Romain-Piquois</b></sub></a><br /><a href="https://github.com/nfrechette/acl/issues?q=author%3ARomain-Piquois" title="Bug reports">🐛</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/janisozaur"><img src="https://avatars.githubusercontent.com/u/550290?v=4?s=100" width="100px;" alt="Michał Janiszewski"/><br /><sub><b>Michał Janiszewski</b></sub></a><br /><a href="https://github.com/nfrechette/acl/commits?author=janisozaur" title="Code">💻</a> <a href="#tool-janisozaur" title="Tools">🔧</a> <a href="#maintenance-janisozaur" title="Maintenance">🚧</a> <a href="#infra-janisozaur" title="Infrastructure (Hosting, Build-Tools, etc)">🚇</a></td>
      <td align="center" valign="top" width="14.28%"><a href="http://keybase.io/visualphoenix"><img src="https://avatars.githubusercontent.com/u/394175?v=4?s=100" width="100px;" alt="Raymond Barbiero"/><br /><sub><b>Raymond Barbiero</b></sub></a><br /><a href="#ideas-visualphoenix" title="Ideas, Planning, & Feedback">🤔</a></td>
    </tr>
    <tr>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/ddeadguyy"><img src="https://avatars.githubusercontent.com/u/42090666?v=4?s=100" width="100px;" alt="ddeadguyy"/><br /><sub><b>ddeadguyy</b></sub></a><br /><a href="https://github.com/nfrechette/acl/commits?author=ddeadguyy" title="Code">💻</a> <a href="#ideas-ddeadguyy" title="Ideas, Planning, & Feedback">🤔</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://gitlab.com/intelligide"><img src="https://avatars.githubusercontent.com/u/12529837?v=4?s=100" width="100px;" alt="Yoann Potinet"/><br /><sub><b>Yoann Potinet</b></sub></a><br /><a href="#infra-intelligide" title="Infrastructure (Hosting, Build-Tools, etc)">🚇</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/jbrd-pg"><img src="https://avatars.githubusercontent.com/u/100773491?v=4?s=100" width="100px;" alt="James Bird"/><br /><sub><b>James Bird</b></sub></a><br /><a href="#ideas-jbrd-pg" title="Ideas, Planning, & Feedback">🤔</a> <a href="https://github.com/nfrechette/acl/commits?author=jbrd-pg" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/GermanAizek"><img src="https://avatars.githubusercontent.com/u/21138600?v=4?s=100" width="100px;" alt="Herman Semenov"/><br /><sub><b>Herman Semenov</b></sub></a><br /><a href="https://github.com/nfrechette/acl/commits?author=GermanAizek" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/Dreaming381"><img src="https://avatars.githubusercontent.com/u/25555355?v=4?s=100" width="100px;" alt="Tyler"/><br /><sub><b>Tyler</b></sub></a><br /><a href="https://github.com/nfrechette/acl/commits?author=Dreaming381" title="Code">💻</a> <a href="#ideas-Dreaming381" title="Ideas, Planning, & Feedback">🤔</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/naetherm"><img src="https://avatars.githubusercontent.com/u/16878712?v=4?s=100" width="100px;" alt="naetherm"/><br /><sub><b>naetherm</b></sub></a><br /><a href="https://github.com/nfrechette/acl/commits?author=naetherm" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/TetraSomia"><img src="https://avatars.githubusercontent.com/u/19408647?v=4?s=100" width="100px;" alt="Arthur Josso"/><br /><sub><b>Arthur Josso</b></sub></a><br /><a href="https://github.com/nfrechette/acl/commits?author=TetraSomia" title="Code">💻</a></td>
    </tr>
    <tr>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/Yusuf-PG"><img src="https://avatars.githubusercontent.com/u/168181217?v=4?s=100" width="100px;" alt="Yusuf Ismail"/><br /><sub><b>Yusuf Ismail</b></sub></a><br /><a href="https://github.com/nfrechette/acl/commits?author=Yusuf-PG" title="Code">💻</a></td>
    </tr>
  </tbody>
</table>

<!-- markdownlint-restore -->
<!-- prettier-ignore-end -->

<!-- ALL-CONTRIBUTORS-LIST:END -->

This project follows the [all-contributors](https://github.com/all-contributors/all-contributors) specification. Contributions of any kind welcome!
