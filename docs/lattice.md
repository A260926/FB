# Lattice instancing

`src/raytracer/lattice.h` adds a finite integer-lattice instance set.  It lets
a scene repeat a small set of opaque prototypes over a possibly huge 3D grid
without creating an `Object` (or a scene-wide BVH leaf) per copy.

## Model

A lattice borrows a table of already `postinit`ed prototypes:

* `OBJECT_SPHERE` or `OBJECT_TRIANGLE` only, opaque and non-emittant;
* positions in the prototype's own local coordinates;
* the prototype's local bounding box may leave its owning cell, but must stay
  inside `[-period, 2 * period]` on every axis.

Each integer cell `cell = (i, j, k)` in the closed box `[lower, upper]`
contributes one copy of every prototype translated by
`(i * period.x, j * period.y, k * period.z)`.  The cell count can be in the
trillions; the lattice stores only the pointer table, the parameters and one
BVH over the prototypes.

The prototypes are **borrowed read-only** until `lattice_free`.  Distinct
lattices may share prototypes.  The renderer's global object list is never
modified.

## API

```c
struct Lattice *lattice_build(struct Object *const *prototypes, size_t count,
                              const v3 period, const int32_t lower[3],
                              const int32_t upper[3]);
void lattice_free(struct Lattice *lattice);

bool lattice_closest(const struct Lattice *lattice, const struct Ray *ray,
                     float tmin, float tmax, struct LatticeHit *hit,
                     struct LatticeStats *stats);
bool lattice_occluded(const struct Lattice *lattice, const struct Ray *ray,
                      float tmin, float tmax, struct LatticeStats *stats);
```

`struct Ray` is the existing type; `direction` must be a unit vector.  The
distance window is finite and right-open: `0 <= tmin < tmax`.

```c
struct LatticeHit {
    size_t prototype;   /* prototype input index */
    int32_t cell[3];    /* cell whose translated copy was hit */
    float distance;     /* ray parameter */
    v3 normal;          /* world-space unit normal */
};

struct LatticeStats {
    size_t cells;              /* ray cells actually visited */
    size_t prototype_queries;  /* prototype-BVH nodes examined */
    size_t primitive_tests;    /* prototype primitive tests */
};
```

`lattice_closest` returns the nearest hit in the window; a miss leaves `hit`
untouched.  `lattice_occluded` is an any-hit test (conservative but never
misses an occluder) for shadow rays.  Both reset `stats` at the start of every
query.

Distance ties are broken by cell `x`, then `y`, then `z`, then by prototype
input index.  A sphere's far root is returned when its near root is behind the
origin or before `tmin`; triangles keep the renderer's two-sided convention
and outward normal.

### Build validation

`lattice_build` returns `NULL` for:

* non-finite or non-positive periodicities;
* inverted ranges (`lower > upper` on any axis);
* non-sphere / non-triangle prototypes;
* transparent or emittant prototypes;
* prototypes whose local bounds leave `[-period, 2 * period]`.

An empty prototype set is legal (`count == 0` with valid parameters).  The
returned lattice is then simply empty (all queries miss).

### Query cost

Every query is first clipped to the lattice domain and to the conservative
union of translated prototype bounds.  It then walks only the integer cells
the ray actually traverses (a 3D DDA).  For each traversed cell it visits the
constant number of owning cells (at most a few per axis) whose translated
bounds reach into it, so a prototype that sticks out of its owning cell is
still found from the neighbouring cell it geometrically reaches.  Hits are
accepted only inside the ray interval of the cell being traversed, so a distant
copy is never reported early.

Queries allocate no memory: working set is bounded by the prototype BVH and a
fixed-size traversal state, not by the number of copies.

### Rendering

`render.h` gains:

```c
void render_set_lattice(struct Lattice *lattice, float far_distance);
void render_clear_lattice(void);
```

The renderer borrows a single built lattice per frame.  `far_distance` must be
a finite positive value supplied by the caller.  When set, primary and
reflected rays compare the lattice's closest hit within `far_distance` against
ordinary objects.  Shadow rays are unaffected and still terminate at the
actual light distance.  Setting or clearing must happen between frames.

Lattice hits take the prototype's material and normal; the texture is sampled
at `hit_world_point - cell * period` (the local prototype frame).  Reflected
rays may hit another copy.  Per-call state is local to `cast_ray`, so recursive
reflection/refraction is isolated; shared `Object`s are never translated and
no cell is stored in a global.  After `render_clear_lattice`, colours and the
random-number sequence are exactly as before the lattice was set.

## Example

`examples/lattice_ppm.c` builds two lattices (stacked-sphere pillars and a
triangular panel wall) from four prototypes, covers a few thousand copies
each, and writes a binary PPM without linking libtiff:

```
make -f Makefile.rt lattice-example
./build/lattice_ppm lattice.ppm
```

It also shows how to reuse `cast_ray` from an in-memory scene.

## Tests

`tests/lattice_target.c` registers `cells`, `identity` and `transport` groups
and is run by:

```
bash task_support/run.sh target
```

It cross-checks small lattices against an independent brute-force enumerator
(including prototypes crossing their owning cell, negative cells, ties and
finite shadow rays), and asserts the counter bounds on a 100,000-cell-per-axis
domain and a 4096-prototype lattice with a spatially separated column.  It
also exercises texture-local sampling, cross-copy reflection, set/replace/clear
and the untouched random stream through the real `cast_ray`.
