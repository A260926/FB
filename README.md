![libGDX Logo](libgdx_logo.svg#gh-light-mode-only)
![libGDX Logo](libgdx_logo_dark.svg#gh-dark-mode-only)

[![GitHub Actions Build Status](https://img.shields.io/github/actions/workflow/status/libgdx/libgdx/build-publish.yml?branch=master&label=GitHub%20Actions)](https://github.com/libgdx/libgdx/actions?query=workflow%3A%22Build+and+Publish%22)

[![Latest Version](https://img.shields.io/maven-central/v/com.badlogicgames.gdx/gdx?label=Version)](https://search.maven.org/artifact/com.badlogicgames.gdx/gdx)
[![Snapshots](https://img.shields.io/maven-metadata/v?metadataUrl=https%3A%2F%2Fcentral.sonatype.com%2Frepository%2Fmaven-snapshots%2Fcom%2Fbadlogicgames%2Fgdx%2Fgdx%2Fmaven-metadata.xml&label=Snapshots)](https://central.sonatype.com/repository/maven-snapshots/com/badlogicgames/gdx/gdx/maven-metadata.xml)

[![Discord Chat](https://img.shields.io/discord/348229412858101762?logo=discord)](https://libgdx.com/community/discord/)

## Cross-platform Game Development Framework
**[libGDX](https://libgdx.com) is a cross-platform Java game development framework based on OpenGL (ES), designed for Windows, Linux, macOS, Android, web browsers, and iOS.** It provides a robust and well-established environment for rapid prototyping and iterative development. Unlike other frameworks, libGDX does not impose a specific design or coding style, allowing you the freedom to create games according to your preferences.

## Open Source, Feature Packed, and Fostering a Large Third-Party Ecosystem
libGDX is released under the [Apache 2.0 License](https://www.apache.org/licenses/LICENSE-2.0.html), offering unrestricted usage in both commercial and non-commercial projects. While not mandatory, we appreciate any credit given to libGDX when you release a game or app using it. Check out our [showcase](https://libgdx.com/showcase/) for a selection of popular libGDX-powered games. With libGDX, you gain access to a comprehensive set of tools and features to develop multi-platform 2D and 3D games using Java.

Moreover, libGDX boasts a vibrant third-party ecosystem, with numerous [tools](https://libgdx.com/dev/tools/) and libraries that streamline development tasks. Explore the [awesome-libgdx](https://github.com/rafaskb/awesome-libgdx#readme) repository for a curated list of libGDX-centered libraries, serving as an excellent starting point for newcomers in the libGDX community.

![](https://libgdx.com/assets/images/index_showcase/game0.png)
###### An example game created with libGDX: [Pathway](https://store.steampowered.com/app/546430/Pathway/) by Robotality. Discover more captivating games in our [Showcase](https://libgdx.com/showcase/).

## Getting Started with libGDX / Documentation
Thanks to Gradle, you can easily set up libGDX without the need to download the framework itself. Your favorite build tool can handle everything for you. Additionally, we offer a convenient [setup tool](https://libgdx.com/dev/#how-to-get-started-with-libgdx) that automates project creation and downloads all the necessary components. Check out our **[website](https://libgdx.com/wiki/start/setup)** for instructions on getting started or refer to our comprehensive **[wiki](https://libgdx.com/wiki/)**.

- [Creating a libGDX Project](https://libgdx.com/wiki/start/setup)
- [Building a Simple Game](https://libgdx.com/wiki/start/a-simple-game)
- [Tutorials & Demos](https://libgdx.com/wiki/start/demos-and-tutorials)

We provide the libGDX [javadocs](https://javadoc.io/doc/com.badlogicgames.gdx) online for easy reference. Additionally, the javadocs are bundled with every libGDX distribution, ensuring smooth integration with your preferred IDE.

## Community & Contribution
Stay up to date with the **latest libGDX news** by following our [blog](https://libgdx.com/news/). For engaging discussions and support, join our official [libGDX Discord](https://libgdx.com/community/discord/).

### Reporting Issues
Use the **[Issue Tracker](https://github.com/libgdx/libgdx/issues)** here on GitHub to report any issues you encounter. Before submitting, please read our [Getting Help](https://libgdx.com/wiki/articles/getting-help) guide, which walks you through the process of reporting an issue effectively.

### Contributing to the Codebase
libGDX benefits greatly from contributions made by our dedicated developer community. We appreciate any assistance in making libGDX even better. Check out the [CONTRIBUTING.md](https://github.com/libgdx/libgdx/blob/master/.github/CONTRIBUTING.md) file for details on how to contribute. Note that contributing involves working directly with libGDX's source code, a process that regular users do not typically undertake. Refer to the [Working with the Source](https://libgdx.com/dev/from-source/) article for guidance.

You can also support our infrastructure (build server, web server, test devices) by contributing financially through our [Patreon](https://patreon.com/libgdx)!

## Pose-space corrective deformation

`com.badlogic.gdx.graphics.g3d.utils.PoseSpaceDeformer` interpolates artist authored corrective displacements (for example elbow
volume corrections) after animation blending and before linear blend skinning. It is pure Java, needs no OpenGL or native
library, and does not change animation interpolation or any shader.

Minimal Java usage:

```java
// Prepare once. Rotations are the unit local bone rotations in the same order as driverIds;
// each sample carries sparse displacements in bind-mesh space.
PoseSpaceDeformer.Sample[] samples = ...;
PoseSpaceDeformer.Prepared prepared = PoseSpaceDeformer.prepare(
    vertexCount, new String[] {"upperArm", "foreArm"},
    new Quaternion[] {bindUpper, bindFore}, new double[] {1.0, 1.0}, 0.8, samples);

// After AnimationController.update(...) (or BaseAnimationController) and instance.calculateTransforms():
float[] world = new float[vertexCount * 3];
prepared.evaluate(instance, nodePart, bindPositions, boneIndices4, boneWeights4, world);
```

`bindPositions` holds bind-mesh positions (`x, y, z` per vertex). `boneIndices4` and `boneWeights4` are
`vertexCount * 4` long, indices refer to `NodePart.bones`, weights are non-negative and sum to one per vertex. `outWorld` may
alias `bindPositions`; on failure it is left untouched.

Coordinate spaces and ordering: the sparse `deltas` are in bind-mesh space and are added to `bindPositions` first. The result is
then skinned with the existing `NodePart.bones` palette (which already contains the inverse bind transforms) using four-weight
linear blend skinning, and finally transformed by `instance.transform` to world space. The node's `globalTransform` is not applied
again for skinned parts, and the mesh is not modified or uploaded. Driver rotations are read from the animated
`Node.localTransform`, not from `Node.rotation`, and only each driver's own local rotation contributes to the pose distance.
Animated drivers may have non-driven parents.

The pose kernel is explicit: for driver `b`, `r_b = inverse(bindRotation_b) * localRotation_b` (samples use the same
convention); squared pose distance is `sum(weight_b * (1 - dot(r_b, s_b)^2))` with the dot product of unit quaternions clamped
to `[-1, 1]`; the kernel is `exp(-beta * distance)`. With `K` the sample-to-sample kernel matrix and `k(q)` the kernel between
the current pose and each sample, the correction is `delta(q) = k(q)^T * K^(-1) * D` where `D` holds each sample's displacement
for the vertex. This solves the linear system rather than normalizing kernel weights, so negative interpolation coefficients
are preserved. `K` is LU-factorized with partial pivoting during `prepare` and is not refactored per evaluation. Kernel and
linear algebra use `double`; output positions are `float`.

Paths with equivalent poses are rejected: if the absolute dot product of every driver rotation is within `1e-6` (about 0.16
degrees), the two samples are duplicates, so a quaternion and its negation count as the same pose. `prepare` also rejects an
empty driver list, duplicate driver ids, non-unit or non-finite rotations, non-positive or non-finite `beta`/weights,
out-of-range or duplicate vertex indices, and non-finite deltas. At least one neutral sample (bind rotations with all-zero
deltas) is required.

Ill-conditioned sample sets: the LU decomposition uses partial pivoting and throws `IllegalArgumentException` as soon as a pivot's
absolute value is below `1e-10`. There is no implicit regularization and samples are never silently dropped. A well-conditioned
small sample set (for example four poses with distinct joint angles) is accepted normally.

`evaluate` rejects a missing bone/node, a `NodePart` from another instance, a missing or non-finite `part.bones` palette, wrong
array lengths, invalid bone indices or weights, a non-affine/non-invertible instance transform, and driver local matrices with
shear, reflection, singularity or non-uniform scale. Driver local matrices must be rotation plus translation with a positive
uniform scale. All validation happens before anything is written, so a rejected call never leaves a partially updated output.
`Prepared` data is copied during `prepare`, is immutable afterwards, and may be shared across model instances, including
interleaved evaluations.

