c-ray-fast
==========
An improved, but still extremely simple, raytracer.

This version of c-ray is cherry-picking the best parts of c-ray-f and c-ray-mt
which have diverged over time, and adds some sorely needed improvements, while
still maintaining the overall simplicity of c-ray and the simple 0-dependency
build.

Main improvements:
  - simple ray intersection acceleration
  - better task parallelization than c-ray-mt
  - improved backwards-compatible scene format
  - option for specifying the background color
  - option for producing an alpha mask
  - colored lights
  - gamma correction

Download
--------
Website: http://nuclear.mutantstargoat.com/sw/c-ray
Github repo: https://github.com/jtsiomb/c-ray

Latest release: 
  - https://github.com/jtsiomb/c-ray/releases/download/v2.0/c-ray-2.0.tar.gz
  - http://nuclear.mutantstargoat.com/sw/c-ray/c-ray-2.0.tar.gz

License
-------
Copyright (C) 2006-2021 John Tsiombikas <nuclear@member.fsf.org>

c-ray-fast is free software. Feel free to use, modify, and/or redistribute it
under the terms of the GNU General Public License v3, or at your option any
later version published by the Free Software Foundation. See COPYING for
details.

Projective prototypes and instances
------------------------------------
Besides plain spheres (`s`) and planes (`p x y z nx ny nz ...`), a scene can
name a *prototype* made of local spheres and place it many times with a generic
4x4 projective matrix, without ever baking the geometry into a mesh.

    # add a sphere to prototype "name" (created on first use)
    p "name" x y z radius "material"
    # place an instance with a row-major local-to-world homogeneous matrix
    i "instance" "name" m00 m01 m02 m03 m10 ... m33

Rules enforced by `load_scene` (any violation makes it return -1):
  - materials must be declared before they are referenced;
  - an `i` may only reference a prototype already created by a `p` record;
  - instance names are unique; the 16 matrix entries must be finite and the
    matrix invertible; prototype sphere radii must be finite and positive.

The last matrix row need not be affine `0 0 0 1`. When a sphere group crosses
the homogeneous denominator-zero plane its image is unbounded; such instances
are placed in a separate candidate list rather than given finite world bounds.
Only finite points obtained by homogeneous division of an original sphere
surface are rendered — the pole plane itself never produces a hit. Queries use
the same convention as everything else: `O + tD` with `ERR_MARGIN <= t <= 1`,
`D` not normalized, and occlusion queries honor the same interval. Multiplying
an entire matrix by any nonzero finite scalar leaves the geometry unchanged.

Data lifetime and rebuilding
-----------------------------
`load_scene` accumulates prototypes, instances, spheres and materials. The
acceleration structures are built separately by `prepare_scene()` (the CLI
calls it once after loading):
  - one octree over the local spheres per **prototype**, shared by every
    instance of that prototype — nothing is copied per instance and no
    per-sphere world quadric is baked;
  - a top-level spatial tree over bounded instances, plus the unbounded
    candidate table;
  - the legacy sphere octree.
`prepare_scene()` is re-entrant: it first releases every previously built
index and rebuilds them all, so editing sphere parameters or an instance
matrix and calling it again keeps the geometry, the candidate ranges and the
shared trees consistent. After it returns the indexed data is read-only; all
per-query temporaries (inverse-mapped ray coefficients, pole intervals,
traversal stacks, statistics) live on the calling thread's stack, so the
pthread render workers share no mutable intersection state.

Reading a hit
-------------
A `spoint` from a legacy sphere/plane keeps `obj` set. An instance hit leaves
`obj` NULL and instead sets `inst` (the instance) and `psph` (the local
sphere); its material is available through `sp->mat`. The two identities are
read back with:

    proj_hit_instance(sp)    // instance name
    proj_hit_prototype(sp)   // prototype name
    proj_hit_sphere(sp)      // index of the sphere within that prototype

Reflection, refraction and point-light shadow rays all go through this same
production path (`ray_scene` / `proj_intersect`). Per-query leaf/node counts
are returned through an optional `struct proj_query_stats`.

