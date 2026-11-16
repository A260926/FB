/*
 * Tests for the finite integer-lattice instance set.
 *
 * Groups:
 *   cells     - analytic reference enumeration on small lattices, cell
 *               traversal behaviour and counters on huge lattices.
 *   identity  - distance ties resolved by cell (x, y, z) then prototype index.
 *   transport - integration with cast_ray / is_light_blocked, texture local
 *               sampling, cross-copy reflection, set/replace/clear.
 **/

#include "../task_support/check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "calc.h"
#include "lattice.h"
#include "material.h"
#include "object.h"
#include "render.h"

#include "../task_support/production.h"

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

#define MAX_PROTOTYPES 8

#define SQRT2 1.41421356f
#define SQRT3 1.73205081f
#define SQRT6 2.44948974f

struct ProtoDesc {
	int is_triangle;
	v3 center;
	float radius;
	v3 vertices[3];
	struct Object *object;
};

static struct Material *material_new(const v3 ka, const v3 ks, const v3 kr, const v3 kt, const v3 ke, struct Texture *texture)
{
	struct Material *material = malloc(sizeof(struct Material));
	material_init(material, 1, ks, ka, kr, kt, ke, 2.f, 1.f, texture);
	return material;
}

static struct Object *sphere_proto(struct Material *material, const v3 center, const float radius)
{
	struct Object *object = sphere_new(center, radius);
	object_init(object, material, -1.f, 0, OBJECT_SPHERE);
	object->object_data->postinit(object);
	return object;
}

static struct Object *triangle_proto(struct Material *material, v3 vertices[3])
{
	struct Object *object = triangle_new(vertices);
	object_init(object, material, -1.f, 0, OBJECT_TRIANGLE);
	object->object_data->postinit(object);
	return object;
}

static void desc_sphere(struct ProtoDesc *desc, struct Object *object, const v3 center, const float radius)
{
	desc->is_triangle = 0;
	assign3(desc->center, center);
	desc->radius = radius;
	desc->object = object;
}

static void desc_triangle(struct ProtoDesc *desc, struct Object *object, v3 vertices[3])
{
	desc->is_triangle = 1;
	memcpy(desc->vertices, vertices, sizeof(v3[3]));
	desc->object = object;
}

/* ------------------------------------------------------------------------- */
/* Independent analytic reference                                           */
/* ------------------------------------------------------------------------- */

struct RefHit {
	int hit;
	double distance;
	int32_t cell[3];
	size_t prototype;
	v3 normal;
};

static int ref_sphere(const struct ProtoDesc *desc, const v3 center, const struct Ray *ray, const double tmin, const double tmax, const double epsilon, double *distance, v3 normal)
{
	double ox = (double)ray->point[X] - (double)center[X];
	double oy = (double)ray->point[Y] - (double)center[Y];
	double oz = (double)ray->point[Z] - (double)center[Z];
	double dx = ray->direction[X], dy = ray->direction[Y], dz = ray->direction[Z];
	double b = -(dx * ox + dy * oy + dz * oz);
	double c = ox * ox + oy * oy + oz * oz - (double)desc->radius * (double)desc->radius;
	double det = b * b - c;
	if (det < 0.0)
		return 0;
	double root = b - sqrt(det);
	if (!(root > epsilon && root >= tmin))
		root = b + sqrt(det);
	if (!(root > epsilon && root >= tmin && root < tmax))
		return 0;
	*distance = root;
	double px = (double)ray->point[X] + dx * root;
	double py = (double)ray->point[Y] + dy * root;
	double pz = (double)ray->point[Z] + dz * root;
	normal[X] = (float)((px - center[X]) / desc->radius);
	normal[Y] = (float)((py - center[Y]) / desc->radius);
	normal[Z] = (float)((pz - center[Z]) / desc->radius);
	return 1;
}

static int ref_triangle(const struct ProtoDesc *desc, const v3 shift, const struct Ray *ray, const double tmin, const double tmax, const double epsilon, double *distance, v3 normal)
{
	double v0[3], e1[3], e2[3], dir[3], pvec[3], tvec[3], qvec[3];
	int axis;
	for (axis = 0; axis < 3; axis++) {
		v0[axis] = (double)desc->vertices[0][axis] + shift[axis];
		e1[axis] = (double)desc->vertices[1][axis] - (double)desc->vertices[0][axis];
		e2[axis] = (double)desc->vertices[2][axis] - (double)desc->vertices[0][axis];
		dir[axis] = ray->direction[axis];
	}
	pvec[0] = dir[1] * e2[2] - dir[2] * e2[1];
	pvec[1] = dir[2] * e2[0] - dir[0] * e2[2];
	pvec[2] = dir[0] * e2[1] - dir[1] * e2[0];
	double det = e1[0] * pvec[0] + e1[1] * pvec[1] + e1[2] * pvec[2];
	if (fabs(det) < 1e-9)
		return 0;
	double inv = 1.0 / det;
	for (axis = 0; axis < 3; axis++)
		tvec[axis] = (double)ray->point[axis] - v0[axis];
	double u = (tvec[0] * pvec[0] + tvec[1] * pvec[1] + tvec[2] * pvec[2]) * inv;
	if (u < 0.0 || u > 1.0)
		return 0;
	qvec[0] = tvec[1] * e1[2] - tvec[2] * e1[1];
	qvec[1] = tvec[2] * e1[0] - tvec[0] * e1[2];
	qvec[2] = tvec[0] * e1[1] - tvec[1] * e1[0];
	double v = (dir[0] * qvec[0] + dir[1] * qvec[1] + dir[2] * qvec[2]) * inv;
	if (v < 0.0 || u + v > 1.0)
		return 0;
	double t = (e2[0] * qvec[0] + e2[1] * qvec[1] + e2[2] * qvec[2]) * inv;
	if (!(t > epsilon && t >= tmin && t < tmax))
		return 0;
	*distance = t;
	normal[X] = (float)(e1[1] * e2[2] - e1[2] * e2[1]);
	normal[Y] = (float)(e1[2] * e2[0] - e1[0] * e2[2]);
	normal[Z] = (float)(e1[0] * e2[1] - e1[1] * e2[0]);
	float magnitude = sqrtf(normal[X] * normal[X] + normal[Y] * normal[Y] + normal[Z] * normal[Z]);
	mul3s(normal, 1.f / magnitude, normal);
	return 1;
}

static struct RefHit ref_closest(const struct ProtoDesc *descs, const size_t count, const v3 period, const int32_t lower[3], const int32_t upper[3], const struct Ray *ray, const float tmin, const float tmax)
{
	struct RefHit best;
	best.hit = 0;
	int64_t i, j, k;
	for (i = lower[X]; i <= upper[X]; i++) {
		for (j = lower[Y]; j <= upper[Y]; j++) {
			for (k = lower[Z]; k <= upper[Z]; k++) {
				int32_t cell[3] = { (int32_t)i, (int32_t)j, (int32_t)k };
				v3 shift = { (float)((double)i * period[X]), (float)((double)j * period[Y]), (float)((double)k * period[Z]) };
				size_t p;
				for (p = 0; p < count; p++) {
					double distance;
					v3 normal;
					int hit;
					double epsilon = descs[p].object->epsilon;
					if (descs[p].is_triangle) {
						hit = ref_triangle(&descs[p], shift, ray, tmin, tmax, epsilon, &distance, normal);
					} else {
						v3 center;
						add3v(descs[p].center, shift, center);
						hit = ref_sphere(&descs[p], center, ray, tmin, tmax, epsilon, &distance, normal);
					}
					if (!hit)
						continue;
					int better = 0;
					if (!best.hit)
						better = 1;
					else if (distance < best.distance)
						better = 1;
					else if (distance == best.distance) {
						if (cell[X] != best.cell[X])
							better = cell[X] < best.cell[X];
						else if (cell[Y] != best.cell[Y])
							better = cell[Y] < best.cell[Y];
						else if (cell[Z] != best.cell[Z])
							better = cell[Z] < best.cell[Z];
						else
							better = p < best.prototype;
					}
					if (better) {
						best.hit = 1;
						best.distance = distance;
						best.cell[X] = cell[X];
						best.cell[Y] = cell[Y];
						best.cell[Z] = cell[Z];
						best.prototype = p;
						assign3(best.normal, normal);
					}
				}
			}
		}
	}
	return best;
}

static int hit_matches(const struct LatticeHit *got, const struct RefHit *expected)
{
	if (got->prototype != expected->prototype)
		return 0;
	if (got->cell[X] != expected->cell[X] || got->cell[Y] != expected->cell[Y] || got->cell[Z] != expected->cell[Z])
		return 0;
	if (fabs((double)got->distance - expected->distance) > 3e-5)
		return 0;
	if (fabs((double)got->normal[X] - expected->normal[X]) > 3e-5)
		return 0;
	if (fabs((double)got->normal[Y] - expected->normal[Y]) > 3e-5)
		return 0;
	if (fabs((double)got->normal[Z] - expected->normal[Z]) > 3e-5)
		return 0;
	return 1;
}

/* ------------------------------------------------------------------------- */
/* cells: analytic reference comparison                                      */
/* ------------------------------------------------------------------------- */

static int cells_sphere_reference(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);
	/* Prototype centred in its owning cell.  A copy of cell k lives at
	 * local + k on each axis. */
	v3 center = { 0.5f, 0.5f, 0.5f };
	struct Object *prototype = sphere_proto(material, center, 0.25f);
	struct Object *prototypes[1] = { prototype };
	const v3 period = { 1.f, 1.f, 1.f };
	const int32_t lower[3] = { -2, -2, -2 };
	const int32_t upper[3] = { 2, 2, 2 };
	struct ProtoDesc descs[1];
	desc_sphere(&descs[0], prototype, center, 0.25f);

	struct Lattice *lattice = lattice_build(prototypes, 1, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	struct {
		struct Ray ray;
		float tmin, tmax;
	} queries[] = {
		{ { { 0.f, 0.f, 1.f }, { 0.5f, 0.5f, -5.f } }, 0.f, 1e6f },
		{ { { 0.f, 0.f, -1.f }, { 0.5f, 0.5f, 5.f } }, 0.f, 1e6f },
		{ { { 0.f, 0.f, 1.f }, { -1.5f, 0.5f, -5.f } }, 0.f, 1e6f },
		{ { { 0.f, 1.f, 0.f }, { 0.5f, -4.5f, 0.5f } }, 0.f, 1e6f },
		{ { { 0.f, 0.f, 1.f }, { 1.5f, -1.5f, -5.f } }, 0.f, 1e6f },
		{ { { 1.f / 3.7416574f, 2.f / 3.7416574f, 3.f / 3.7416574f }, { 0.f, 0.f, 0.f } }, 0.f, 1e6f },
		{ { { 0.f, 1.f, 0.f }, { -0.5f, -0.5f, -0.5f } }, 0.f, 1e6f },
		/* Starts in the empty half of a cell, still hits the neighbour. */
		{ { { 0.f, 0.f, 1.f }, { 0.5f, 0.5f, 0.1f } }, 0.f, 1e6f },
		/* The nearest copy begins at 0.75; tmin past it selects the next. */
		{ { { 0.f, 0.f, 1.f }, { 0.5f, 0.5f, -5.f } }, 4.4f, 1e6f },
	};

	size_t q;
	for (q = 0; q < arrlen(queries); q++) {
		struct LatticeStats stats;
		struct LatticeHit hit;
		memset(&hit, 0, sizeof(hit));
		struct RefHit expected = ref_closest(descs, 1, period, lower, upper, &queries[q].ray, queries[q].tmin, queries[q].tmax);
		int found = lattice_closest(lattice, &queries[q].ray, queries[q].tmin, queries[q].tmax, &hit, &stats);
		TASK_CHECK(found == expected.hit);
		if (expected.hit)
			TASK_CHECK(hit_matches(&hit, &expected));
		else
			TASK_CHECK(hit.prototype == 0 && hit.cell[X] == 0 && hit.cell[Y] == 0 && hit.cell[Z] == 0 && hit.distance == 0.f);

		/* Stats are reset on every query. */
		size_t cells = stats.cells;
		stats.cells = 999999;
		stats.prototype_queries = 999999;
		stats.primitive_tests = 999999;
		struct LatticeHit again;
		lattice_closest(lattice, &queries[q].ray, queries[q].tmin, queries[q].tmax, &again, &stats);
		TASK_CHECK(stats.cells == cells);
		TASK_CHECK(stats.cells < 999999 && stats.prototype_queries < 999999);
	}

	/* A miss must not touch the caller's hit structure. */
	struct LatticeStats miss_stats;
	struct LatticeHit miss_hit;
	memset(&miss_hit, 0x5a, sizeof(miss_hit));
	struct LatticeHit miss_copy;
	memcpy(&miss_copy, &miss_hit, sizeof(miss_hit));
	TASK_CHECK(!lattice_closest(lattice, &queries[0].ray, 30.0f, 40.0f, &miss_hit, &miss_stats));
	TASK_CHECK(memcmp(&miss_hit, &miss_copy, sizeof(miss_hit)) == 0);

	/* Points outside the lattice domain simply miss. */
	struct Ray outside_ray = { { 0.f, 0.f, 1.f }, { 0.5f, 100.5f, -5.f } };
	memset(&miss_hit, 0x5a, sizeof(miss_hit));
	TASK_CHECK(!lattice_closest(lattice, &outside_ray, 0.f, 1e6f, &miss_hit, &miss_stats));
	TASK_CHECK(memcmp(&miss_hit, &miss_copy, sizeof(miss_hit)) == 0);

	lattice_free(lattice);
	free(prototype);
	free(material);
	free(texture);
	return 0;
}

static int cells_triangle_reference(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);
	v3 vertices[3] = { { 0.f, 0.f, 0.2f }, { 1.f, 0.f, 0.2f }, { 0.f, 1.f, 0.2f } };
	struct Object *prototype = triangle_proto(material, vertices);
	struct Object *prototypes[1] = { prototype };
	const v3 period = { 1.f, 1.f, 1.f };
	const int32_t lower[3] = { -1, -1, -1 };
	const int32_t upper[3] = { 1, 1, 1 };
	struct ProtoDesc descs[1];
	desc_triangle(&descs[0], prototype, vertices);

	struct Lattice *lattice = lattice_build(prototypes, 1, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	struct {
		struct Ray ray;
		float tmin, tmax;
	} queries[] = {
		{ { { 0.f, 0.f, 1.f }, { 0.5f, 0.5f, -2.f } }, 0.f, 1e6f },
		{ { { 0.f, 0.f, 1.f }, { 1.5f, 0.3f, -2.f } }, 0.f, 1e6f },
		{ { { 0.f, 0.f, 1.f }, { -0.5f, 0.3f, -2.f } }, 0.f, 1e6f },
		{ { { 0.f, 0.f, -1.f }, { 0.5f, 0.5f, 2.f } }, 0.f, 1e6f },
		{ { { 1.f / SQRT6, 1.f / SQRT6, 2.f / SQRT6 }, { 0.f, 0.f, 0.f } }, 0.f, 1e6f },
		{ { { 0.f, 0.f, 1.f }, { 2.5f, 0.5f, -2.f } }, 0.f, 1e6f },
		{ { { 0.f, 0.f, 1.f }, { 0.5f, 0.5f, -2.f } }, 2.0f, 1e6f },
	};

	size_t q;
	for (q = 0; q < arrlen(queries); q++) {
		struct LatticeStats stats;
		struct LatticeHit hit;
		memset(&hit, 0, sizeof(hit));
		struct RefHit expected = ref_closest(descs, 1, period, lower, upper, &queries[q].ray, queries[q].tmin, queries[q].tmax);
		int found = lattice_closest(lattice, &queries[q].ray, queries[q].tmin, queries[q].tmax, &hit, &stats);
		TASK_CHECK(found == expected.hit);
		if (expected.hit) {
			if (!hit_matches(&hit, &expected))
				fprintf(stderr, "DEBUG q=%zu ray d=%.9g,%.9g,%.9g p=%.9g,%.9g,%.9g got p=%zu cell=%d,%d,%d d=%.9g n=%.9g,%.9g,%.9g | exp p=%zu cell=%d,%d,%d d=%.9g n=%.9g,%.9g,%.9g\n", q, (double)queries[q].ray.direction[0], (double)queries[q].ray.direction[1], (double)queries[q].ray.direction[2], (double)queries[q].ray.point[0], (double)queries[q].ray.point[1], (double)queries[q].ray.point[2], hit.prototype, hit.cell[0], hit.cell[1], hit.cell[2], (double)hit.distance, (double)hit.normal[0], (double)hit.normal[1], (double)hit.normal[2], expected.prototype, expected.cell[0], expected.cell[1], expected.cell[2], expected.distance, (double)expected.normal[0], (double)expected.normal[1], (double)expected.normal[2]);
			TASK_CHECK(hit_matches(&hit, &expected));
		}
	}

	lattice_free(lattice);
	free(prototype);
	free(material);
	free(texture);
	return 0;
}

/* A prototype that crosses its owning cell must be found from the cell the
 * hit actually lies in. */
static int cells_neighbour_cell(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);
	v3 center = { 0.9f, 0.5f, 0.5f };
	struct Object *prototype = sphere_proto(material, center, 0.5f);
	struct Object *prototypes[1] = { prototype };
	const v3 period = { 1.f, 1.f, 1.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 1, 0, 0 };
	struct ProtoDesc descs[1];
	desc_sphere(&descs[0], prototype, center, 0.5f);

	struct Lattice *lattice = lattice_build(prototypes, 1, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	/* The ray travels through cell x = 1, but the only sphere that reaches it
	 * is the copy owned by cell x = 0. */
	struct Ray ray = { { 0.f, 0.f, 1.f }, { 1.2f, 0.5f, -2.f } };
	struct LatticeStats stats;
	struct LatticeHit hit;
	struct RefHit expected = ref_closest(descs, 1, period, lower, upper, &ray, 0.f, 1e6f);
	TASK_CHECK(expected.hit);
	TASK_CHECK(lattice_closest(lattice, &ray, 0.f, 1e6f, &hit, &stats));
	TASK_CHECK(hit_matches(&hit, &expected));
	TASK_CHECK(hit.cell[X] == 0 && hit.cell[Y] == 0 && hit.cell[Z] == 0);
	/* The hit point lies in the cell the ray is traversing, x = 1. */
	TASK_CHECK(1.2f >= 1.f);

	/* Ray through cell x = 1 only, but far from both copies: no early hit. */
	struct Ray grazing = { { 0.f, 0.f, 1.f }, { 1.2f, 3.f, -2.f } };
	TASK_CHECK(!lattice_closest(lattice, &grazing, 0.f, 1e6f, &hit, &stats));

	lattice_free(lattice);
	free(prototype);
	free(material);
	free(texture);
	return 0;
}

/* Negative cells, zero direction components and rays starting inside a
 * prototype. */
static int cells_negative_zero_and_inside(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);
	v3 center = { 0.5f, 0.5f, 0.5f };
	struct Object *prototype = sphere_proto(material, center, 0.4f);
	struct Object *prototypes[1] = { prototype };
	const v3 period = { 2.f, 2.f, 2.f };
	const int32_t lower[3] = { -3, -1, -1 };
	const int32_t upper[3] = { -1, 1, 1 };
	struct ProtoDesc descs[1];
	desc_sphere(&descs[0], prototype, center, 0.4f);

	struct Lattice *lattice = lattice_build(prototypes, 1, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	struct {
		struct Ray ray;
		float tmin, tmax;
	} queries[] = {
		/* Hits the copy owned by cell (-3, 0, 0), negative cells. */
		{ { { 0.f, 0.f, 1.f }, { -5.5f, 0.5f, -4.f } }, 0.f, 1e6f },
		/* Zero x/y direction components. */
		{ { { 0.f, 0.f, -1.f }, { -3.5f, 0.5f, 5.f } }, 0.f, 1e6f },
		/* Starts inside the copy owned by cell (-2, -1, -1). */
		{ { { 0.f, 0.f, 1.f }, { -3.5f, -1.5f, -1.5f } }, 0.f, 1e6f },
		/* Exactly on a cell corner. */
		{ { { 1.f / SQRT3, 1.f / SQRT3, 1.f / SQRT3 }, { -4.f, -2.f, -2.f } }, 0.f, 1e6f },
	};

	size_t q;
	for (q = 0; q < arrlen(queries); q++) {
		struct LatticeStats stats;
		struct LatticeHit hit;
		struct RefHit expected = ref_closest(descs, 1, period, lower, upper, &queries[q].ray, queries[q].tmin, queries[q].tmax);
		int found = lattice_closest(lattice, &queries[q].ray, queries[q].tmin, queries[q].tmax, &hit, &stats);
		TASK_CHECK(found == expected.hit);
		if (expected.hit)
			TASK_CHECK(hit_matches(&hit, &expected));
	}

	lattice_free(lattice);
	free(prototype);
	free(material);
	free(texture);
	return 0;
}

/* A sphere's near root may be in front of the window; the far root is then
 * the valid hit.  Rays starting inside a sphere take the far root too. */
static int cells_far_root_window(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);
	v3 center = { 0.f, 0.f, 0.f };
	struct Object *prototype = sphere_proto(material, center, 1.f);
	struct Object *prototypes[1] = { prototype };
	const v3 period = { 4.f, 4.f, 4.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 0, 0, 0 };

	struct Lattice *lattice = lattice_build(prototypes, 1, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	struct LatticeStats stats;
	struct LatticeHit hit;

	/* Origin inside: only the far root (t = 1) is in front. */
	struct Ray inside = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, 0.f } };
	TASK_CHECK(lattice_closest(lattice, &inside, 0.f, 5.f, &hit, &stats));
	TASK_CHECK(fabsf(hit.distance - 1.f) < 1e-6f);
	TASK_CHECK(fabsf(hit.normal[X]) < 3e-5f && fabsf(hit.normal[Y]) < 3e-5f && fabsf(hit.normal[Z] - 1.f) < 3e-5f);

	/* Near root is 2, far root is 4.  With tmin = 3 only the far root
	 * qualifies; with tmin = 1 the near root is used. */
	struct Ray outside = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, -3.f } };
	TASK_CHECK(lattice_closest(lattice, &outside, 3.f, 5.f, &hit, &stats));
	TASK_CHECK(fabsf(hit.distance - 4.f) < 1e-5f);
	TASK_CHECK(lattice_closest(lattice, &outside, 1.f, 5.f, &hit, &stats));
	TASK_CHECK(fabsf(hit.distance - 2.f) < 1e-5f);

	/* Window entirely before the near root, and beyond the far root. */
	TASK_CHECK(!lattice_closest(lattice, &outside, 0.f, 1.9f, &hit, &stats));
	TASK_CHECK(!lattice_closest(lattice, &outside, 4.1f, 10.f, &hit, &stats));

	lattice_free(lattice);
	free(prototype);
	free(material);
	free(texture);
	return 0;
}

/* Finite shadow rays: occlusion respects the half-open query window. */
static int cells_occlusion_window(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);
	v3 center = { 0.5f, 0.5f, 2.f };
	struct Object *prototype = sphere_proto(material, center, 0.5f);
	struct Object *prototypes[1] = { prototype };
	const v3 period = { 4.f, 4.f, 4.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 0, 0, 0 };

	struct Lattice *lattice = lattice_build(prototypes, 1, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	struct Ray ray = { { 0.f, 0.f, 1.f }, { 0.5f, 0.5f, 0.f } };
	struct LatticeStats stats;
	/* Sphere surface begins at t = 1.5. */
	TASK_CHECK(!lattice_occluded(lattice, &ray, 0.f, 1.49f, &stats));
	TASK_CHECK(lattice_occluded(lattice, &ray, 0.f, 1.51f, &stats));
	TASK_CHECK(lattice_occluded(lattice, &ray, 1.0f, 2.0f, &stats));
	/* Window starts after the far surface: no hit. */
	TASK_CHECK(!lattice_occluded(lattice, &ray, 2.6f, 5.f, &stats));
	/* Stats reset on every query. */
	struct LatticeStats stats_before;
	lattice_occluded(lattice, &ray, 0.f, 1.51f, &stats_before);
	stats.cells = 777777;
	stats.prototype_queries = 777777;
	stats.primitive_tests = 777777;
	lattice_occluded(lattice, &ray, 0.f, 1.51f, &stats);
	TASK_CHECK(stats.cells == stats_before.cells && stats.cells < 777777);
	TASK_CHECK(stats.prototype_queries == stats_before.prototype_queries && stats.prototype_queries < 777777);

	lattice_free(lattice);
	free(prototype);
	free(material);
	free(texture);
	return 0;
}

/* Huge copy domains must not be scanned: the ray stops once it has hit, and
 * it starts on a cell boundary so the hit lands after one full cell step. */
static int cells_huge_domain_counters(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);
	v3 center = { 0.5f, 0.5f, 0.5f };
	struct Object *prototype = sphere_proto(material, center, 0.2f);
	struct Object *prototypes[1] = { prototype };
	const v3 period = { 1.f, 1.f, 1.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 99999, 99999, 99999 };

	struct Lattice *lattice = lattice_build(prototypes, 1, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	/* The ray starts inside cell z = 4, below that copy's sphere, crosses into
	 * cell z = 3 and hits that sphere there.  It must stop in the second
	 * cell, not walk the remaining 100,000 cells. */
	struct Ray ray = { { 0.f, 0.f, -1.f }, { 0.5f, 0.5f, 4.2f } };
	struct LatticeStats stats;
	struct LatticeHit hit;
	TASK_CHECK(lattice_closest(lattice, &ray, 0.f, 1e9f, &hit, &stats));
	TASK_CHECK(hit.cell[X] == 0 && hit.cell[Y] == 0 && hit.cell[Z] == 3);
	TASK_CHECK(fabsf(hit.distance - (4.2f - 3.7f)) < 1e-4f);
	printf("CELLS_HUGE_DOMAIN cells=%zu prototype_queries=%zu primitive_tests=%zu\n", stats.cells, stats.prototype_queries, stats.primitive_tests);
	TASK_CHECK(stats.cells <= 4);
	TASK_CHECK(stats.prototype_queries <= 128);

	/* Occlusion also stops as soon as it finds a blocker. */
	TASK_CHECK(lattice_occluded(lattice, &ray, 0.f, 1e9f, &stats));
	printf("CELLS_HUGE_OCCLUSION cells=%zu prototype_queries=%zu primitive_tests=%zu\n", stats.cells, stats.prototype_queries, stats.primitive_tests);
	TASK_CHECK(stats.cells <= 4);

	lattice_free(lattice);
	free(prototype);
	free(material);
	free(texture);
	return 0;
}

/* 4096 spatially separated prototype spheres: a ray crossing a single column
 * must test only a small number of primitives thanks to the prototype BVH. */
static int cells_many_prototypes_counters(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);
	enum { GRID = 16, COUNT = GRID * GRID * GRID };
	struct Object **prototypes = malloc(sizeof(struct Object *) * COUNT);
	size_t i;
	/* Spheres sitting at the *far* z=8 plane, so the ray only meets the
	 * column at (x, y) = (8, 8). */
	for (i = 0; i < COUNT; i++) {
		v3 center = { 0.5f + (float)(i % GRID), 0.5f + (float)((i / GRID) % GRID), 0.5f + (float)(i / (GRID * GRID)) };
		prototypes[i] = sphere_proto(material, center, 0.25f);
	}
	const v3 period = { 1024.f, 1024.f, 1024.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 0, 0, 0 };
	struct Lattice *lattice = lattice_build(prototypes, COUNT, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	/* Ray travels along +x at y = z = 8.5, through one row (a single
	 * y/z column) of spheres.  The first sphere is the copy at (x, y, z) =
	 * (0, 8, 8), centred at (0.5, 8.5, 8.5); its near surface is at x = 0.25,
	 * i.e. 3.25 units from the origin at x = -3. */
	struct Ray ray = { { 1.f, 0.f, 0.f }, { -3.f, 8.5f, 8.5f } };
	struct LatticeStats stats;
	struct LatticeHit hit;
	TASK_CHECK(lattice_closest(lattice, &ray, 0.f, 1e6f, &hit, &stats));
	printf("CELLS_MANY_PROTOTYPES prototype_queries=%zu primitive_tests=%zu\n", stats.prototype_queries, stats.primitive_tests);
	TASK_CHECK(stats.primitive_tests < 128);
	TASK_CHECK(stats.cells <= 4);
	TASK_CHECK(fabsf(hit.distance - (0.5f - 0.25f + 3.f)) < 1e-4f);
	TASK_CHECK(hit.prototype == (size_t)(0 + 8 * GRID + 8 * GRID * GRID));
	TASK_CHECK(hit.cell[X] == 0 && hit.cell[Y] == 0 && hit.cell[Z] == 0);

	lattice_free(lattice);
	for (i = 0; i < COUNT; i++)
		free(prototypes[i]);
	free(prototypes);
	free(material);
	free(texture);
	return 0;
}

/* Build validation and independent lifetimes. */
static int cells_build_validation(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);
	struct Texture *texture_alpha = texture_uniform_new(zero);
	struct Material *transparent = material_new(zero, zero, zero, (v3){ 0.5f, 0.5f, 0.5f }, zero, texture_alpha);
	struct Texture *texture_emitter = texture_uniform_new(zero);
	struct Material *emittant = material_new(zero, zero, zero, zero, (v3){ 1.f, 1.f, 1.f }, texture_emitter);

	v3 center = { 0.f, 0.f, 0.f };
	struct Object *sphere = sphere_proto(material, center, 0.1f);
	struct Object *sphere_transparent = sphere_proto(transparent, center, 0.1f);
	struct Object *sphere_emittant = sphere_proto(emittant, center, 0.1f);
	v3 vertices[3] = { { 0.f, 0.f, 0.f }, { 0.1f, 0.f, 0.f }, { 0.f, 0.1f, 0.f } };
	struct Object *triangle = triangle_proto(material, vertices);
	struct Object *sphere_far = sphere_proto(material, center, 5.f);
#ifdef UNBOUND_OBJECTS
	struct Object *plane = plane_new(center, (v3){ 0.f, 0.f, 1.f });
	object_init(plane, material, -1.f, 0, OBJECT_PLANE);
	plane->object_data->postinit(plane);
#endif

	const v3 period = { 1.f, 1.f, 1.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 1, 1, 1 };
	const int32_t inverted_lower[3] = { 1, 0, 0 };
	const int32_t inverted_upper[3] = { 0, 0, 1 };
	const v3 bad_periods[3] = { { 0.f, 1.f, 1.f }, { -1.f, 1.f, 1.f }, { NAN, 1.f, 1.f } };
	struct Object *spheres[1] = { sphere };
	struct Object *tris[1] = { triangle };
	struct Object *transparent_set[1] = { sphere_transparent };
	struct Object *emittant_set[1] = { sphere_emittant };
	struct Object *far_set[1] = { sphere_far };

	size_t b;
	for (b = 0; b < arrlen(bad_periods); b++)
		TASK_CHECK(lattice_build(spheres, 1, bad_periods[b], lower, upper) == NULL);
	TASK_CHECK(lattice_build(spheres, 1, period, inverted_lower, inverted_upper) == NULL);
	TASK_CHECK(lattice_build(transparent_set, 1, period, lower, upper) == NULL);
	TASK_CHECK(lattice_build(emittant_set, 1, period, lower, upper) == NULL);
	TASK_CHECK(lattice_build(far_set, 1, period, lower, upper) == NULL);
	TASK_CHECK(lattice_build(NULL, 1, period, lower, upper) == NULL);
#ifdef UNBOUND_OBJECTS
	struct Object *planes[1] = { plane };
	TASK_CHECK(lattice_build(planes, 1, period, lower, upper) == NULL);
#endif

	/* Supported triangles build fine. */
	struct Lattice *triangle_lattice = lattice_build(tris, 1, period, lower, upper);
	TASK_CHECK(triangle_lattice != NULL);
	/* The triangle owned by cell z = 1 sits at z = 1 and is hit first by a
	 * ray down -z from z = 2. */
	struct Ray triangle_probe = { { 0.f, 0.f, -1.f }, { 0.02f, 0.02f, 2.f } };
	struct LatticeStats triangle_stats;
	struct LatticeHit triangle_hit;
	TASK_CHECK(lattice_closest(triangle_lattice, &triangle_probe, 0.f, 100.f, &triangle_hit, &triangle_stats));
	TASK_CHECK(fabsf(triangle_hit.distance - 1.f) < 1e-5f);
	/* With z restricted to 0 only the origin copy is hit. */
	const int32_t flat_lower[3] = { 0, 0, 0 };
	const int32_t flat_upper[3] = { 1, 1, 0 };
	struct Lattice *flat_lattice = lattice_build(tris, 1, period, flat_lower, flat_upper);
	TASK_CHECK(flat_lattice != NULL);
	TASK_CHECK(lattice_closest(flat_lattice, &triangle_probe, 0.f, 100.f, &triangle_hit, &triangle_stats));
	TASK_CHECK(fabsf(triangle_hit.distance - 2.f) < 1e-5f);
	lattice_free(flat_lattice);
	lattice_free(triangle_lattice);

	/* A prototype that sticks out beyond [-period, 2 * period] is rejected,
	 * and the check is per axis. */
	struct Object *wide = sphere_proto(material, center, 1.5f); /* [-1.5, 1.5] */
	struct Object *wide_set[1] = { wide };
	TASK_CHECK(lattice_build(wide_set, 1, period, lower, upper) == NULL);
	const v3 wide_period = { 2.f, 1.f, 1.f };
	const int32_t wide_lower[3] = { 0, 0, 0 };
	const int32_t wide_upper[3] = { 0, 0, 0 };
	TASK_CHECK(lattice_build(wide_set, 1, wide_period, wide_lower, wide_upper) == NULL);
	/* Exactly on the boundary is accepted. */
	struct Object *edge = sphere_proto(material, center, 0.5f); /* [-0.5, 0.5] */
	struct Object *edge_set[1] = { edge };
	struct Lattice *edge_lattice = lattice_build(edge_set, 1, (v3){ 0.5f, 1.f, 1.f }, lower, upper);
	TASK_CHECK(edge_lattice != NULL);
	lattice_free(edge_lattice);

	/* An empty prototype set is legal with otherwise valid parameters. */
	struct Lattice *empty = lattice_build(NULL, 0, period, lower, upper);
	TASK_CHECK(empty != NULL);
	struct Ray any = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f } };
	struct LatticeStats stats;
	struct LatticeHit hit;
	TASK_CHECK(!lattice_closest(empty, &any, 0.f, 10.f, &hit, &stats));
	TASK_CHECK(!lattice_occluded(empty, &any, 0.f, 10.f, &stats));
	lattice_free(empty);
	TASK_CHECK(lattice_build(NULL, 0, bad_periods[0], lower, upper) == NULL);

	/* Independent lattices may share prototypes; freeing one leaves the
	 * other fully usable and the prototypes untouched. */
	struct Object *shared[1] = { sphere };
	struct Lattice *first = lattice_build(shared, 1, period, lower, upper);
	struct Lattice *second = lattice_build(shared, 1, period, lower, upper);
	TASK_CHECK(first != NULL && second != NULL);
	lattice_free(first);
	struct Ray probe = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, -3.f } };
	TASK_CHECK(lattice_closest(second, &probe, 0.f, 10.f, &hit, &stats));
	TASK_CHECK(fabsf(hit.distance - 2.9f) < 1e-5f);
	v3 corners_before[2], corners_after[2];
	sphere->object_data->get_corners(sphere, corners_before);
	lattice_free(second);
	sphere->object_data->get_corners(sphere, corners_after);
	TASK_CHECK(memcmp(corners_before, corners_after, sizeof(corners_before)) == 0);

	free(sphere);
	free(sphere_transparent);
	free(sphere_emittant);
	free(triangle);
	free(sphere_far);
#ifdef UNBOUND_OBJECTS
	free(plane);
#endif
	free(material);
	free(transparent);
	free(emittant);
	free(texture);
	free(texture_alpha);
	free(texture_emitter);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* identity: distance ties                                                   */
/* ------------------------------------------------------------------------- */

static int identity_setup(const v3 center, const float radius, const v3 period, const int32_t lower[3], const int32_t upper[3], struct Object **prototype, struct Material **material, struct Texture **texture, struct Lattice **lattice)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	*texture = texture_uniform_new(zero);
	*material = material_new(zero, zero, zero, zero, zero, *texture);
	*prototype = sphere_proto(*material, center, radius);
	struct Object *prototypes[1] = { *prototype };
	*lattice = lattice_build(prototypes, 1, period, lower, upper);
	return *lattice != NULL;
}

static int identity_cell_x(void)
{
	struct Object *prototype;
	struct Material *material;
	struct Texture *texture;
	struct Lattice *lattice;
	const v3 center = { 0.f, 0.f, 0.f };
	const v3 period = { 2.f, 2.f, 2.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 1, 0, 0 };
	identity_setup(center, 1.2f, period, lower, upper, &prototype, &material, &texture, &lattice);
	TASK_CHECK(lattice != NULL);

	struct Ray ray = { { 0.f, 0.f, 1.f }, { 1.f, 0.f, -5.f } };
	struct LatticeHit hit;
	struct LatticeStats stats;
	TASK_CHECK(lattice_closest(lattice, &ray, 0.f, 100.f, &hit, &stats));
	TASK_CHECK(hit.cell[X] == 0 && hit.cell[Y] == 0 && hit.cell[Z] == 0);
	/* Sphere at (0,0,0) radius 1.2; origin at x = 1, z = -5. */
	TASK_CHECK(fabsf(hit.distance - (5.f - sqrtf(1.44f - 1.f))) < 3e-5f);
	TASK_CHECK(hit.normal[X] > 0.f);

	struct ProtoDesc descs[1];
	desc_sphere(&descs[0], prototype, center, 1.2f);
	struct RefHit expected = ref_closest(descs, 1, period, lower, upper, &ray, 0.f, 100.f);
	TASK_CHECK(hit_matches(&hit, &expected));

	lattice_free(lattice);
	free(prototype);
	free(material);
	free(texture);
	return 0;
}

static int identity_cell_y(void)
{
	struct Object *prototype;
	struct Material *material;
	struct Texture *texture;
	struct Lattice *lattice;
	const v3 center = { 0.f, 0.f, 0.f };
	const v3 period = { 2.f, 2.f, 2.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 0, 1, 0 };
	identity_setup(center, 1.2f, period, lower, upper, &prototype, &material, &texture, &lattice);
	TASK_CHECK(lattice != NULL);

	struct Ray ray = { { 0.f, 0.f, 1.f }, { 0.f, 1.f, -5.f } };
	struct LatticeHit hit;
	struct LatticeStats stats;
	TASK_CHECK(lattice_closest(lattice, &ray, 0.f, 100.f, &hit, &stats));
	TASK_CHECK(hit.cell[X] == 0 && hit.cell[Y] == 0 && hit.cell[Z] == 0);
	TASK_CHECK(hit.normal[Y] > 0.f);

	struct ProtoDesc descs[1];
	desc_sphere(&descs[0], prototype, center, 1.2f);
	struct RefHit expected = ref_closest(descs, 1, period, lower, upper, &ray, 0.f, 100.f);
	TASK_CHECK(hit_matches(&hit, &expected));

	lattice_free(lattice);
	free(prototype);
	free(material);
	free(texture);
	return 0;
}

static int identity_cell_z(void)
{
	struct Object *prototype;
	struct Material *material;
	struct Texture *texture;
	struct Lattice *lattice;
	const v3 center = { 0.f, 0.f, 0.f };
	const v3 period = { 2.f, 2.f, 2.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 0, 0, 1 };
	identity_setup(center, 1.2f, period, lower, upper, &prototype, &material, &texture, &lattice);
	TASK_CHECK(lattice != NULL);

	struct Ray ray = { { 1.f, 0.f, 0.f }, { -5.f, 0.f, 1.f } };
	struct LatticeHit hit;
	struct LatticeStats stats;
	TASK_CHECK(lattice_closest(lattice, &ray, 0.f, 100.f, &hit, &stats));
	TASK_CHECK(hit.cell[X] == 0 && hit.cell[Y] == 0 && hit.cell[Z] == 0);
	TASK_CHECK(hit.normal[Z] > 0.f);

	struct ProtoDesc descs[1];
	desc_sphere(&descs[0], prototype, center, 1.2f);
	struct RefHit expected = ref_closest(descs, 1, period, lower, upper, &ray, 0.f, 100.f);
	TASK_CHECK(hit_matches(&hit, &expected));

	lattice_free(lattice);
	free(prototype);
	free(material);
	free(texture);
	return 0;
}

/* y is compared before z when both differ. */
static int identity_cell_y_before_z(void)
{
	struct Object *prototype;
	struct Material *material;
	struct Texture *texture;
	struct Lattice *lattice;
	const v3 center = { 0.f, 0.f, 0.f };
	const v3 period = { 2.f, 2.f, 2.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 0, 1, 1 };
	identity_setup(center, 1.5f, period, lower, upper, &prototype, &material, &texture, &lattice);
	TASK_CHECK(lattice != NULL);

	/* Equidistant from the copies centred at (0,0,0) and (0,2,2). */
	struct Ray ray = { { 1.f, 0.f, 0.f }, { -5.f, 1.f, 1.f } };
	struct LatticeHit hit;
	struct LatticeStats stats;
	TASK_CHECK(lattice_closest(lattice, &ray, 0.f, 100.f, &hit, &stats));
	TASK_CHECK(hit.cell[X] == 0 && hit.cell[Y] == 0 && hit.cell[Z] == 0);

	lattice_free(lattice);
	free(prototype);
	free(material);
	free(texture);
	return 0;
}

/* Identical prototypes at the same place: input index zero wins. */
static int identity_prototype_index(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material_a = material_new(zero, zero, zero, zero, zero, texture);
	struct Material *material_b = material_new(zero, zero, zero, zero, zero, texture);
	v3 center = { 0.f, 0.f, 0.f };
	struct Object *a = sphere_proto(material_a, center, 1.f);
	struct Object *b = sphere_proto(material_b, center, 1.f);
	struct Object *prototypes[2] = { a, b };
	const v3 period = { 4.f, 4.f, 4.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 0, 0, 0 };
	struct Lattice *lattice = lattice_build(prototypes, 2, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	struct Ray ray = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, -5.f } };
	struct LatticeHit hit;
	struct LatticeStats stats;
	TASK_CHECK(lattice_closest(lattice, &ray, 0.f, 100.f, &hit, &stats));
	TASK_CHECK(hit.prototype == 0);
	TASK_CHECK(fabsf(hit.distance - 4.f) < 1e-5f);

	/* Same geometry, reversed input order: index zero of the new table. */
	struct Object *reversed[2] = { b, a };
	struct Lattice *lattice_reversed = lattice_build(reversed, 2, period, lower, upper);
	TASK_CHECK(lattice_reversed != NULL);
	TASK_CHECK(lattice_closest(lattice_reversed, &ray, 0.f, 100.f, &hit, &stats));
	TASK_CHECK(hit.prototype == 0);

	lattice_free(lattice);
	lattice_free(lattice_reversed);
	free(a);
	free(b);
	free(material_a);
	free(material_b);
	free(texture);
	return 0;
}

/* Cell ordering outranks prototype ordering on equal distance. */
static int identity_cell_before_prototype(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);
	v3 center_a = { 0.f, 0.f, 0.f };
	v3 center_b = { 2.f, 0.f, 0.f };
	struct Object *a = sphere_proto(material, center_a, 0.9f);
	struct Object *b = sphere_proto(material, center_b, 0.9f);
	struct Object *prototypes[2] = { a, b };
	const v3 period = { 2.f, 2.f, 2.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 1, 0, 0 };
	struct Lattice *lattice = lattice_build(prototypes, 2, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	/* Cell 0's copy of prototype 1 and cell 1's copy of prototype 0 both sit
	 * at (2, 0, 0).  Cell 0 must win even though its prototype index is 1. */
	struct Ray ray = { { 0.f, 0.f, 1.f }, { 2.f, 0.f, -5.f } };
	struct LatticeHit hit;
	struct LatticeStats stats;
	TASK_CHECK(lattice_closest(lattice, &ray, 0.f, 100.f, &hit, &stats));
	TASK_CHECK(hit.cell[X] == 0 && hit.cell[Y] == 0 && hit.cell[Z] == 0);
	TASK_CHECK(hit.prototype == 1);

	lattice_free(lattice);
	free(a);
	free(b);
	free(material);
	free(texture);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* transport: integration with the renderer                                  */
/* ------------------------------------------------------------------------- */

static struct ObjectVTable fixed_light_vtable;
static v3 fixed_light_point;
static void fixed_get_light_point(const struct Object *object, const v3 point, v3 light_point)
{
	(void)object;
	(void)point;
	assign3(light_point, fixed_light_point);
}

static void scene_begin(size_t object_count, size_t emittant_count)
{
	num_objects = object_count;
	num_emittant_objects = emittant_count;
#ifdef UNBOUND_OBJECTS
	num_unbound_objects = 0;
#endif
	objects_init();
}

static void scene_end(void)
{
	accel_deinit();
	objects_deinit();
}

/* Cross-copy reflection: a mirror triangle's reflected ray must hit the copy
 * in the next cell.  Compared against ordinary triangles at the same world
 * positions. */
static int transport_reflection_across_copies(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	const v3 ambient_intensity = { 1.f, 1.f, 1.f };
	struct Texture *texture = texture_uniform_new((v3){ 0.25f, 0.5f, 0.75f });
	struct Material *material = material_new((v3){ 0.1f, 0.2f, 0.3f }, zero, (v3){ 0.5f, 0.5f, 0.5f }, zero, zero, texture);

	v3 vertices[3] = { { 0.f, -1.f, 0.f }, { 0.f, 1.f, 0.f }, { 2.f, -1.f, 2.f } };
	v3 vertices_shifted[3];
	vertices_shifted[0][X] = vertices[0][X] + 2.f;
	vertices_shifted[0][Y] = vertices[0][Y];
	vertices_shifted[0][Z] = vertices[0][Z];
	vertices_shifted[1][X] = vertices[1][X] + 2.f;
	vertices_shifted[1][Y] = vertices[1][Y];
	vertices_shifted[1][Z] = vertices[1][Z];
	vertices_shifted[2][X] = vertices[2][X] + 2.f;
	vertices_shifted[2][Y] = vertices[2][Y];
	vertices_shifted[2][Z] = vertices[2][Z];

	/* Ordinary control scene: the two mirrors as real objects. */
	scene_begin(2, 0);
	struct Object *ordinary_0 = triangle_proto(material, vertices);
	struct Object *ordinary_1 = triangle_proto(material, vertices_shifted);
	objects[0] = ordinary_0;
	objects[1] = ordinary_1;
	accel_init();

	struct Ray ray = { { 0.f, 0.f, 1.f }, { 0.5f, 0.f, -2.5f } };
	v3 ordinary_color = { 0.f, 0.f, 0.f };
	assign3(global_ambient_light_intensity, ambient_intensity);
	float ordinary_depth = cast_ray(&ray, (v3){ 1.f, 1.f, 1.f }, ordinary_color, 2, NULL);
	/* The mirror plane is x = z; the origin (0.5, 0, -2.5) meets it at
	 * z = 0.5, i.e. t = 3. */
	TASK_CHECK(fabsf(ordinary_depth - 3.f) < 1e-5f);
	/* The second mirror contributes its ambient term through the reflection,
	 * multiplied by the first mirror's kr and by the distance attenuation. */
	TASK_CHECK(ordinary_color[X] > 0.005f);

	/* Lattice: prototype is the first mirror, cells 0 and 1 both in x. */
	struct Object *prototype = triangle_proto(material, vertices);
	struct Object *prototypes[1] = { prototype };
	const v3 period = { 2.f, 2.f, 2.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 1, 0, 0 };
	struct Lattice *lattice = lattice_build(prototypes, 1, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	render_set_lattice(lattice, 100.f);
	v3 lattice_color = { 0.f, 0.f, 0.f };
	float lattice_depth = cast_ray(&ray, (v3){ 1.f, 1.f, 1.f }, lattice_color, 2, NULL);
	TASK_CHECK(fabsf(lattice_depth - ordinary_depth) < 3e-5f);
	size_t c;
	for (c = 0; c < 3; c++)
		TASK_CHECK(fabsf(lattice_color[c] - ordinary_color[c]) < 3e-5f);
	render_clear_lattice();

	/* Clearing restores the ordinary scene exactly. */
	v3 cleared_color = { 0.f, 0.f, 0.f };
	float cleared_depth = cast_ray(&ray, (v3){ 1.f, 1.f, 1.f }, cleared_color, 2, NULL);
	TASK_CHECK(cleared_depth == ordinary_depth);
	for (c = 0; c < 3; c++)
		TASK_CHECK(cleared_color[c] == ordinary_color[c]);

	/* The prototype was not translated by rendering. */
	v3 corners_before[2], corners_after[2];
	prototype->object_data->get_corners(prototype, corners_before);
	render_set_lattice(lattice, 100.f);
	v3 scratch = { 0.f, 0.f, 0.f };
	cast_ray(&ray, (v3){ 1.f, 1.f, 1.f }, scratch, 2, NULL);
	render_clear_lattice();
	prototype->object_data->get_corners(prototype, corners_after);
	TASK_CHECK(memcmp(corners_before, corners_after, sizeof(corners_before)) == 0);

	lattice_free(lattice);
	free(prototype);
	assign3(global_ambient_light_intensity, zero);
	scene_end();
	free(material);
	free(texture);
	return 0;
}

/* The prototype texture is sampled at the hit point minus the cell shift. */
static int transport_texture_local_space(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	v3 colors[2] = { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f } };
	struct Texture *texture = texture_checkerboard_new(colors, 1.f);
	struct Material *prototype_material = material_new(zero, zero, zero, zero, zero, texture);
	struct Material *emitter_material = material_new(zero, zero, zero, zero, (v3){ 10.f, 0.f, 0.f }, texture);

	v3 light = { 1.5f, 1.5f, -0.5f };
	assign3(fixed_light_point, light);
	v3 emitter_center = { 1.5f, 1.5f, -0.5f };
	struct Object *emitter = sphere_new(emitter_center, 0.05f);
	object_init(emitter, emitter_material, -1.f, 1, OBJECT_SPHERE);
	emitter->object_data->postinit(emitter);

	scene_begin(1, 1);
	objects[0] = emitter;
	emittant_objects[0] = emitter;
	fixed_light_vtable = *emitter->object_data;
	fixed_light_vtable.get_light_point = &fixed_get_light_point;
	emitter->object_data = &fixed_light_vtable;
	accel_init();

	v3 center = { 0.5f, 0.5f, 0.5f };
	struct Object *prototype = sphere_proto(prototype_material, center, 0.4f);
	struct Object *prototypes[1] = { prototype };
	const v3 period = { 1.f, 1.f, 1.f };
	const int32_t lower[3] = { 1, 1, 1 };
	const int32_t upper[3] = { 1, 1, 1 };
	struct Lattice *lattice = lattice_build(prototypes, 1, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	struct Ray ray = { { 0.f, 0.f, 1.f }, { 1.5f, 1.5f, 1.0f } };
	render_set_lattice(lattice, 100.f);
	v3 color = { 0.f, 0.f, 0.f };
	float depth = cast_ray(&ray, (v3){ 1.f, 1.f, 1.f }, color, 1, NULL);
	render_clear_lattice();
	TASK_CHECK(fabsf(depth - 0.1f) < 1e-5f);

	/* Local hit point is (0.5, 0.5, 0.1); checkerboard parity 0 -> red.  The
	 * light point is fixed and both light and object attenuation are square. */
	double light_distance = sqrt((1.5 - 1.5) * (1.5 - 1.5) + (1.5 - 1.5) * (1.5 - 1.5) + (1.6) * (1.6));
	double light_attenuation = 1.0 / (1.0 + light_distance * light_distance);
	/* The shader attenuates by the hit distance: 1 / (1 + t)^2 with t = 0.1. */
	double object_attenuation = 1.0 / ((1.0 + 0.1) * (1.0 + 0.1));
	double diffuse_factor = fabs((light[Z] - 1.1) / light_distance);
	double expected_red = 10.0 * light_attenuation * object_attenuation * diffuse_factor;
	TASK_CHECK(fabs((double)color[X] - expected_red) < 3e-5);
	TASK_CHECK(fabsf(color[Y]) < 3e-5f && fabsf(color[Z]) < 3e-5f);

	/* Sampling at the world point (1.5, 1.5, 1.1) would select the green
	 * checkerboard cell instead; make sure the test can tell them apart. */
	uint32_t world_parity = ((uint32_t)(1.5f * 1.f) + (uint32_t)(1.5f * 1.f) + (uint32_t)(1.1f * 1.f)) % 2u;
	uint32_t local_parity = ((uint32_t)(0.5f * 1.f) + (uint32_t)(0.5f * 1.f) + (uint32_t)(0.1f * 1.f)) % 2u;
	TASK_CHECK(world_parity == 1 && local_parity == 0);

	lattice_free(lattice);
	free(prototype);
	scene_end();
	free(prototype_material);
	free(emitter_material);
	free(texture);
	return 0;
}

/* render_set_lattice replaces the borrowed set; render_clear_lattice removes
 * it.  Ordinary objects still win when closer. */
static int transport_set_replace_clear(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);

	scene_begin(1, 0);
	v3 control_center = { 0.f, 0.f, 1.f };
	struct Object *control = sphere_proto(material, control_center, 0.5f);
	objects[0] = control;
	accel_init();

	struct Object *a = sphere_proto(material, (v3){ 0.f, 0.f, 3.f }, 1.f);
	struct Object *b = sphere_proto(material, (v3){ 0.f, 0.f, -2.f }, 0.5f);
	struct Object *prototypes_a[1] = { a };
	struct Object *prototypes_b[1] = { b };
	const v3 period = { 10.f, 10.f, 10.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 0, 0, 0 };
	struct Lattice *lattice_a = lattice_build(prototypes_a, 1, period, lower, upper);
	struct Lattice *lattice_b = lattice_build(prototypes_b, 1, period, lower, upper);
	TASK_CHECK(lattice_a != NULL && lattice_b != NULL);

	struct Ray ray = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, -5.f } };
	v3 color = { 0.f, 0.f, 0.f };
	const v3 kr = { 1.f, 1.f, 1.f };

	render_clear_lattice();
	TASK_CHECK(fabsf(cast_ray(&ray, kr, color, 1, NULL) - 5.5f) < 1e-5f);

	/* Lattice A sits inside the ordinary control sphere; the control wins. */
	render_set_lattice(lattice_a, 100.f);
	TASK_CHECK(fabsf(cast_ray(&ray, kr, color, 1, NULL) - 5.5f) < 1e-5f);

	/* Lattice B is closer than the control. */
	render_set_lattice(lattice_b, 100.f);
	TASK_CHECK(fabsf(cast_ray(&ray, kr, color, 1, NULL) - 2.5f) < 1e-5f);

	/* far_distance limits the lattice but never the ordinary objects. */
	render_set_lattice(lattice_b, 2.f);
	TASK_CHECK(fabsf(cast_ray(&ray, kr, color, 1, NULL) - 5.5f) < 1e-5f);
	render_set_lattice(lattice_b, 4.f);
	TASK_CHECK(fabsf(cast_ray(&ray, kr, color, 1, NULL) - 2.5f) < 1e-5f);

	/* A null lattice disables instancing. */
	render_set_lattice(NULL, 100.f);
	TASK_CHECK(fabsf(cast_ray(&ray, kr, color, 1, NULL) - 5.5f) < 1e-5f);

	render_clear_lattice();
	TASK_CHECK(fabsf(cast_ray(&ray, kr, color, 1, NULL) - 5.5f) < 1e-5f);

	lattice_free(lattice_a);
	lattice_free(lattice_b);
	free(a);
	free(b);
	scene_end();
	free(material);
	free(texture);
	return 0;
}

/* Shadow rays terminate at the actual light distance, not at far_distance. */
static int transport_shadow_distance(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new(zero);
	struct Material *material = material_new(zero, zero, zero, zero, zero, texture);

	scene_begin(1, 0);
	v3 control_center = { 0.f, 0.f, 100.f };
	struct Object *control = sphere_proto(material, control_center, 0.1f);
	objects[0] = control;
	accel_init();

	struct Object *blocker = sphere_proto(material, (v3){ 0.f, 0.f, 2.f }, 0.5f);
	struct Object *prototypes[1] = { blocker };
	const v3 period = { 10.f, 10.f, 10.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 0, 0, 0 };
	struct Lattice *lattice = lattice_build(prototypes, 1, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	render_set_lattice(lattice, 1000.f);
	struct Ray shadow = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, 0.f } };
	v3 intensity = { 1.f, 1.f, 1.f };
	/* The blocker's surface starts at t = 1.5. */
	TASK_CHECK(!is_light_blocked(&shadow, 1.4f, intensity, NULL));
	TASK_CHECK(is_light_blocked(&shadow, 1.6f, intensity, NULL));
	TASK_CHECK(is_light_blocked(&shadow, 3.0f, intensity, NULL));

	render_clear_lattice();
	TASK_CHECK(!is_light_blocked(&shadow, 3.0f, intensity, NULL));

	lattice_free(lattice);
	free(blocker);
	scene_end();
	free(material);
	free(texture);
	return 0;
}

/* Clearing the lattice restores colours and leaves the random sequence
 * untouched. */
static int transport_clear_preserves(void)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *texture = texture_uniform_new((v3){ 0.5f, 0.25f, 0.125f });
	struct Material *material = material_new((v3){ 0.25f, 0.5f, 1.f }, zero, (v3){ 0.5f, 0.5f, 0.5f }, zero, zero, texture);
	struct Material *lattice_material = material_new((v3){ 0.1f, 0.1f, 0.1f }, zero, (v3){ 0.9f, 0.9f, 0.9f }, zero, zero, texture);

	scene_begin(1, 0);
	v3 control_center = { 0.f, 0.f, 3.f };
	struct Object *control = sphere_proto(material, control_center, 0.5f);
	objects[0] = control;
	accel_init();

	struct Object *prototype = sphere_proto(lattice_material, (v3){ 0.f, 0.f, 1.f }, 0.5f);
	struct Object *prototypes[1] = { prototype };
	const v3 period = { 10.f, 10.f, 10.f };
	const int32_t lower[3] = { 0, 0, 0 };
	const int32_t upper[3] = { 0, 0, 0 };
	struct Lattice *lattice = lattice_build(prototypes, 1, period, lower, upper);
	TASK_CHECK(lattice != NULL);

	assign3(global_ambient_light_intensity, (v3){ 1.f, 1.f, 1.f });
	struct Ray ray = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, -5.f } };
	const v3 kr = { 1.f, 1.f, 1.f };
	v3 baseline = { 0.f, 0.f, 0.f };
	render_clear_lattice();
	cast_ray(&ray, kr, baseline, 1, NULL);

	srand(4242);
	int expected_random = rand();
	srand(4242);
	render_set_lattice(lattice, 100.f);
	v3 lattice_color = { 0.f, 0.f, 0.f };
	cast_ray(&ray, kr, lattice_color, 1, NULL);
	render_clear_lattice();
	v3 cleared = { 0.f, 0.f, 0.f };
	cast_ray(&ray, kr, cleared, 1, NULL);

	size_t c;
	for (c = 0; c < 3; c++) {
		TASK_CHECK(baseline[c] == cleared[c]);
		TASK_CHECK(lattice_color[c] != baseline[c]);
	}
	TASK_CHECK(rand() == expected_random);

	assign3(global_ambient_light_intensity, zero);
	lattice_free(lattice);
	free(prototype);
	scene_end();
	free(material);
	free(lattice_material);
	free(texture);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Registration                                                              */
/* ------------------------------------------------------------------------- */

const struct task_case task_cases[] = {
	{ "cells", "sphere_reference", &cells_sphere_reference },
	{ "cells", "triangle_reference", &cells_triangle_reference },
	{ "cells", "neighbour_cell", &cells_neighbour_cell },
	{ "cells", "negative_zero_inside", &cells_negative_zero_and_inside },
	{ "cells", "far_root_window", &cells_far_root_window },
	{ "cells", "occlusion_window", &cells_occlusion_window },
	{ "cells", "huge_domain_counters", &cells_huge_domain_counters },
	{ "cells", "many_prototypes_counters", &cells_many_prototypes_counters },
	{ "cells", "build_validation", &cells_build_validation },
	{ "identity", "cell_x", &identity_cell_x },
	{ "identity", "cell_y", &identity_cell_y },
	{ "identity", "cell_z", &identity_cell_z },
	{ "identity", "cell_y_before_z", &identity_cell_y_before_z },
	{ "identity", "prototype_index", &identity_prototype_index },
	{ "identity", "cell_before_prototype", &identity_cell_before_prototype },
	{ "transport", "reflection_across_copies", &transport_reflection_across_copies },
	{ "transport", "texture_local_space", &transport_texture_local_space },
	{ "transport", "set_replace_clear", &transport_set_replace_clear },
	{ "transport", "shadow_distance", &transport_shadow_distance },
	{ "transport", "clear_preserves", &transport_clear_preserves },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
