/*
 * Copyright (c) 2021-2022 Wojciech Graj
 *
 * Licensed under the MIT license: https://opensource.org/licenses/MIT
 * Permission is granted to use, copy, modify, and redistribute the work.
 * Full license information available in the project LICENSE file.
 *
 * DESCRIPTION:
 *   Finite integer-lattice instancing of opaque spheres and triangles.
 *
 *   A lattice holds an array of already postinit'ed prototype Objects.  Each
 *   integer cell (i, j, k) of the closed box [lower, upper] contributes one
 *   copy of every prototype translated by (i * period.x, j * period.y,
 *   k * period.z).  The prototypes' local bounding boxes may stick out of
 *   their owning cell, but must be contained in [-period, 2 * period] on each
 *   axis.
 *
 *   Only the pointer table and the parameters are copied; the prototypes are
 *   borrowed read-only until lattice_free.  Memory and construction work are
 *   independent of the number of copies, which may be in the trillions.
 **/

#ifndef __LATTICE_H__
#define __LATTICE_H__

#include "object.h"

struct Lattice;

/* Per-query counters. Reset at the start of every query. */
struct LatticeStats {
	size_t cells; /* ray cells actually visited */
	size_t prototype_queries; /* prototype-tree visits (nodes examined) */
	size_t primitive_tests; /* prototype primitive intersection tests */
};

struct LatticeHit {
	size_t prototype; /* prototype input index */
	int32_t cell[3]; /* cell whose translated prototype was hit */
	float distance; /* ray parameter */
	v3 normal; /* world-space unit normal, pointing outwards */
};

/*
 * Builds a lattice from a table of prototypes.  Returns NULL on failure:
 * count == 0 is legal and returns an empty lattice; invalid (non-positive,
 * non-finite) periods, lower > upper on any axis, unsupported object types,
 * prototypes whose material is transparent or emittant, and prototypes whose
 * local bounding box leaves [-period, 2 * period] all fail.
 *
 * On success the returned lattice borrows `prototypes` (only the pointer
 * table is copied, not the objects).  The caller must keep the objects alive
 * and unmodified until lattice_free.  Distinct lattices may share prototypes.
 */
struct Lattice *lattice_build(struct Object *const *prototypes, size_t count, const v3 period, const int32_t lower[3], const int32_t upper[3]);

void lattice_free(struct Lattice *lattice);

/* Introspection used by the renderer.  `lattice_prototype` returns the
 * borrowed prototype object for an index, or NULL when out of range. */
struct Object *lattice_prototype(const struct Lattice *lattice, size_t index);
void lattice_get_period(const struct Lattice *lattice, v3 period);

/*
 * Closest hit of `ray` (unit direction) within [tmin, tmax).  On a hit fills
 * `hit` and returns true; on a miss leaves `hit` untouched and returns false.
 * Ties in distance are broken by cell x, then y, then z, then prototype
 * input index.
 */
bool lattice_closest(const struct Lattice *lattice, const struct Ray *ray, float tmin, float tmax, struct LatticeHit *hit, struct LatticeStats *stats);

/*
 * Any-hit test within [tmin, tmax).  Conservative: it may report a hit for an
 * object that the exact closest-intersection query would not select, but it
 * never misses an occluding prototype.  Returns true when something is hit.
 */
bool lattice_occluded(const struct Lattice *lattice, const struct Ray *ray, float tmin, float tmax, struct LatticeStats *stats);

/*
 * Primitive intersection used by the lattice, exposed for testing.
 * Finds the nearest intersection with distance in [tmin, tmax) and greater
 * than the object epsilon.  A sphere's far root is returned when the near
 * root is behind the origin or before tmin.  Returns false for unsupported
 * types.
 */
bool object_get_intersection_in_window(const struct Object *object, const struct Ray *ray, float tmin, float tmax, float *distance, v3 normal);

#endif /* __LATTICE_H__ */
