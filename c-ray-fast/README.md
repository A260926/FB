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
  - directly ray-traced fused ball field groups (metaballs), including
    negative-weight notches, analytic roots (no meshing or fixed marching),
    and a repeatable in-memory scene preparation entry point

Fused ball field groups
------------------------
A field group is a surface defined directly by a scalar field, without ever
building a mesh or marching in fixed steps. Each group has a positive
isosurface value `tau` and one material. It contains any number of compactly
supported weighted kernels (components) with center `c`, positive radius `R`
and finite non-zero weight `w` (negative weights are allowed):

    F(p) = sum_i w_i * max(0, 1 - |p - c_i|^2 / R_i^2)^2 ,   surface F(p) = tau

The supporting spheres only delimit where each kernel contributes. Where
kernels overlap their fields are summed *first* and the isosurface is solved
afterwards; individual kernel hits are never combined. A negative kernel
subtracts from the field and can carve concave notches and multiple nested
layers. Empty groups (no components) are legal and produce no surface.

Along a ray `O + t D`, every active kernel is a quadratic in `t` squared, so
on each interval with a fixed active set the field is a polynomial of degree
at most four. Roots are isolated with a Sturm sequence (tangent and other
even-multiplicity roots are found without relying on sign changes), then
refined by machine-granularity bisection. The earliest root with
`ERR_MARGIN <= t <= 1` is returned, `D` keeps its original length, and rays
starting inside an entity return the exit root. The outward unit normal is
`-grad F / |grad F|`, pointing toward decreasing field.

Scene file additions (see `examples/metaballs.scn`):

    B "group name" tau "declared material name"
    b "group name" x y z R w

Duplicate group names, non-positive `tau`/`R`, zero or non-finite weights,
and references to undeclared materials or groups make `load_scene` fail and
the program exit non-zero. Field groups take part in nearest-hit queries,
point-light shadow rays, reflection and refraction exactly like spheres and
infinite planes.

C interface and repeatable preparation
--------------------------------------
The object declarations and global scene state live in `scene.h`; the field
production code is split into `metaball.c`/`metaball.h` and the spatial index
into `octree.c`. The useful entry points are:

    int  prepare_scene(void);
         Build (or rebuild) the octree over the currently loaded objects.
         It first frees the previous octree, so component centers, radii or
         weights may be edited between calls; conservative group bounds are
         recomputed. Returns 0 on success, -1 on failure.

    int  ray_metaball(struct metaball *mb, struct ray *ray, struct spoint *sp);
         Production ray/group intersection. Pass sp == NULL for an occlusion
         (any-hit) query; the same root finder serves nearest and shadow
         queries. Per-call scratch is isolated, so multi-threaded rendering
         never mutates shared objects.

    double mb_field(struct metaball *mb, struct vec3 *p);
    void   mb_gradient(struct metaball *mb, struct vec3 *p, struct vec3 *g);
         Evaluate the field and its world-space gradient at a point.

A program can fill the global arrays (`objects`, `metaballs`, `materials`,
`lights`, ...) directly in memory, call `prepare_scene()`, then drive
`get_primary_ray()`/`trace()` itself. `examples/memory_scene.c` is a complete
no-scene-file example that renders a group with a negative notch into a PPM
using only C99, libm and pthreads.

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
