/* c-ray-fast - projective instancing
 * Copyright (C) 2006-2021 John Tsiombikas <nuclear@member.fsf.org>
 *
 * You are free to use, modify and redistribute this program under the terms
 * of the GNU General Public License v3 or later. See COPYING for details.
 *
 * Projective prototypes and instances.
 *
 * A prototype is a named group of local spheres (each carrying its own
 * material). An instance places a prototype with a generic 4x4 homogeneous
 * local-to-world matrix. The prototype geometry is never baked into a mesh:
 * a single local octree is built per prototype and shared by every instance
 * that references it.
 *
 * Data lifetime:
 *   proj_add_proto_sphere()/proj_add_instance() accumulate scene data during
 *   load_scene(). All acceleration structures (the shared local octrees, the
 *   top-level bounded-instance tree and the unbounded candidate list) are
 *   built by proj_prepare(), which is invoked through prepare_scene().
 *   Calling prepare_scene() again first releases every previously built index
 *   and rebuilds them from the current sphere parameters and instance
 *   matrices, so mutating geometry and re-preparing keeps the geometry, the
 *   candidate ranges and the shared trees in sync. proj_clear() releases
 *   everything (prototypes, instances and indexes) and is called at the start
 *   of every load_scene().
 *
 * Thread safety:
 *   After prepare_scene() returns, all indexed data is read-only. Query-time
 *   temporaries (inverse-mapped ray coefficients, pole intervals, traversal
 *   stacks, statistics) live on the caller's stack, so pthread render workers
 *   never mutate shared intersection state.
 *
 * Hit identities:
 *   A spoint coming from an instance carries non-NULL inst and psph pointers
 *   (sp->obj stays NULL). Read the two identities with
 *   proj_hit_instance() (the instance name) and proj_hit_prototype() plus
 *   proj_hit_sphere() (the prototype name and the local sphere index inside
 *   it). Legacy sphere/plane hits leave the pointers NULL and keep sp->obj.
 */
#ifndef PROJECTIVE_H
#define PROJECTIVE_H

#include <stdio.h>
#include <stddef.h>

#define ERR_MARGIN		1e-6	/* an arbitrary error margin to avoid surface acne */

/* some helpful macros... */
#define SQ(x)		((x) * (x))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define CLAMP(x, a, b)	MIN(MAX(x, a), b)
#define DOT(a, b)	((a).x * (b).x + (a).y * (b).y + (a).z * (b).z)
#define NORMALIZE(a)  do {\
	double len = sqrt(DOT(a, a));\
	(a).x /= len; (a).y /= len; (a).z /= len;\
} while(0);
#define LERP(a, b, t)	((a) + ((b) - (a)) * (t))

struct vec3 {
	double x, y, z;
};

struct color {
	float r, g, b, a;
};

struct ray {
	struct vec3 orig, dir, invdir;
};

struct material {
	char *name;
	struct color kd, ks; /* color (diffuse/specular) */
	double spow;		/* specular power */
	double refl;		/* reflection intensity */
	double refr;		/* refraction (transmission) intensity */
	double ior;			/* index of refraction */
};

struct sphere {
	struct vec3 pos;
	double rad;
};

struct plane {
	struct vec3 pos;
	struct vec3 norm;
};

enum {
	OBJ_SPHERE = 1,
	OBJ_PLANE = 2
};

struct object {
	int type;
	union {
		struct sphere sph;
		struct plane pln;
	} o;
	struct material mat;
};

struct aabox {
	struct vec3 vmin, vmax;
};

struct prototype;
struct instance;
struct proto_sphere;

struct spoint {
	struct vec3 pos, normal, vref;	/* position, normal and view reflection */
	double dist;		/* parametric distance of intersection along the ray */
	struct object *obj;
	struct material *mat;		/* resolved material, valid on every hit */
	struct instance *inst;		/* NULL for legacy sphere/plane hits */
	struct proto_sphere *psph;	/* local sphere that was hit, NULL for legacy */
};

struct camera {
	struct vec3 pos, targ;
	double fov;
};

struct light {
	struct vec3 pos;
	struct color col;
};

/* a sphere entry inside a prototype (local coordinates plus a material index
 * into the scene material table).
 */
struct proto_sphere {
	struct sphere sph;
	int mat_index;
};

/* octree node over a prototype's local spheres, stores sphere indices so a
 * single tree is shared by every instance of the prototype.
 */
struct poctnode {
	struct aabox aabb;
	int *sph;			/* leaf: local sphere indices, NULL on internal nodes */
	int num_sph;
	struct poctnode *sub[8];
};

struct prototype {
	char *name;
	struct proto_sphere *sph;
	int num_sph, max_sph;
	struct aabox bounds;
	struct poctnode *tree;		/* shared local octree, built in proj_prepare */
};

struct instance {
	char *name;
	struct prototype *proto;
	double m[16];			/* row-major local-to-world homogeneous matrix */
	double invm[16];		/* inverse, recomputed on every prepare */
	int bounded;			/* nonzero when the projective image is finite */
	struct aabox wbox;		/* world bounds, valid when bounded != 0 */
};

/* per-query statistics, always allocated by the caller so concurrent queries
 * never touch shared state.
 */
struct proj_query_stats {
	unsigned long leaf_tests;	/* local sphere quadratic evaluations */
	unsigned long node_tests;	/* local octree node culls */
};

/* scene accumulation (used by load_scene) */
void proj_clear(void);
int proj_add_proto_sphere(const char *name, const struct sphere *sph, int mat_index);
int proj_add_instance(const char *name, const char *proto_name, const double m[16]);

/* lookup of the accumulated scene data */
struct prototype *proj_find_prototype(const char *name);
struct instance *proj_find_instance(const char *name);
int proj_instance_count(void);
int proj_unbounded_count(void);

/* material table binding; indices stored in proto spheres get resolved here */
void proj_bind_materials(struct material *mtl, int num_mtl);

/* build (after releasing the old ones) all shared local trees, the bounded
 * instance top tree and the unbounded candidate table. Returns 0 on success,
 * -1 on allocation failure or a matrix that stopped being invertible.
 */
int proj_prepare(void);

/* production intersection over every instance. ret == NULL asks for any hit
 * (occlusion queries); st may be NULL. Ray convention is O+tD with
 * ERR_MARGIN <= t <= 1 and D not necessarily normalized.
 */
int proj_intersect(const struct ray *ray, struct spoint *ret, struct proj_query_stats *st);

/* identity accessors for a projective hit, NULL when not an instance hit */
const char *proj_hit_instance(const struct spoint *sp);
const char *proj_hit_prototype(const struct spoint *sp);
int proj_hit_sphere(const struct spoint *sp); /* sphere index within prototype */

/* legacy vector helper provided by c-ray-fast.c, shared with the tracer */
void reflect(struct vec3 *res, const struct vec3 *v, const struct vec3 *n);
int ray_aabox(const struct aabox *box, const struct ray *ray);

/* whole-scene preparation entry implemented in c-ray-fast.c: rebuilds the
 * legacy sphere octree and all projective indexes. Safe to call repeatedly.
 */
int prepare_scene(void);

/* parse a scene description stream (legacy records plus p/i projective ones) */
int load_scene(FILE *fp);

#endif	/* PROJECTIVE_H */
