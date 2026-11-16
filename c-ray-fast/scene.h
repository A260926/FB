/* c-ray-fast - a simple raytracing filter based on c-ray-f and c-ray-mt.
 * Copyright (C) 2006-2021 John Tsiombikas <nuclear@member.fsf.org>
 *
 * You are free to use, modify and redistribute this program under the terms
 * of the GNU General Public License v3 or later. See COPYING for details.
 * ---------------------------------------------------------------------
 * Scene object declarations and the global scene state. The actual ray
 * tracing, acceleration structure and scene file parsing live in separate
 * translation units (c-ray-fast.c, octree.c, metaball.c) which all include
 * this header.
 */
#ifndef CRay_SCENE_H
#define CRay_SCENE_H

#include <stdio.h>
#include <stdint.h>
#include <float.h>

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

struct aabox {
	struct vec3 vmin, vmax;
};

/* A single compactly supported kernel of a fused ball field group:
 * f_i(p) = w * max(0, 1 - |p-c|^2 / R^2)^2, contributing only while the
 * sample point stays inside its supporting sphere (|p-c| < R). */
struct mb_component {
	struct vec3 c;
	double r;		/* positive radius R */
	double w;		/* finite non-zero weight (may be negative) */
};

/* A fused ball field group. The group field is the sum of all its component
 * kernels and its surface is F(p) = tau. Empty groups produce no surface. */
struct metaball {
	char *name;
	double tau;				/* positive isosurface value */
	struct material mat;	/* material of the whole group */
	struct mb_component *comp;
	int num_comp, max_comp;
	struct aabox bbox;		/* conservative union of all support spheres */
};

enum {
	OBJ_SPHERE = 1,
	OBJ_PLANE = 2,
	OBJ_METABALL = 3
};

struct object {
	int type;
	union {
		struct sphere sph;
		struct plane pln;
		struct metaball *mb;
	} o;
	struct material mat;
};

struct spoint {
	struct vec3 pos, normal, vref;	/* position, normal and view reflection */
	double dist;		/* parametric distance of intersection along the ray */
	struct object *obj;
};

struct camera {
	struct vec3 pos, targ;
	double fov;
};

struct light {
	struct vec3 pos;
	struct color col;
};

struct octnode {
	struct aabox aabb;
	struct object **obj;
	int num_obj, max_obj;
	struct octnode *sub[8];
};

struct block {
	struct color *start;
	int x, y, width, height;
};

#define RAY_MAG			1000.0			/* trace rays of this magnitude */
#define MAX_RAY_DEPTH	5				/* raytrace recursion limit */
#define ERR_MARGIN		1e-6			/* an arbitrary error margin to avoid surface acne */

#define OCT_MAX_OBJ		64
#define OCT_MAX_DEPTH	9

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

/* global state */
extern int xres, yres;
extern double aspect;
extern struct color *pixels;

extern struct object *objects;
extern struct object *infobj;	/* infinite objects (planes) */
extern struct material *materials;
extern struct light *lights;
extern struct metaball *metaballs;
extern int num_obj, num_inf, num_mtl, num_lt, num_mb;
extern struct camera cam;
extern double inv_gamma;
extern int verbose, progress;
extern struct color bgcol;
extern struct color ambcol;
extern int rays_per_pixel;
extern double rcp_samples;

extern struct octnode *octroot;

extern int blk_width, blk_height;
extern struct block *blocks;
extern int num_blk, cur_blk;

extern struct material def_mtl;

/* c-ray-fast.c: rendering, shading, primitive intersections and parsing */
void render(void);
void *thread_func(void *arg);
void trace(struct ray *ray, int depth, struct color *col);
void shade(struct ray *ray, struct spoint *sp, int depth, struct color *col);
int ray_scene(struct ray *ray, struct spoint *sp);
void reflect(struct vec3 *res, struct vec3 *v, struct vec3 *n);
void refract(struct vec3 *res, struct vec3 *v, struct vec3 *n, double from_ior, double to_ior);
void cross_product(struct vec3 *res, struct vec3 *v1, struct vec3 *v2);
void get_primary_ray(struct ray *ray, int x, int y, int sample);
void get_sample_pos(struct vec3 *res, int x, int y, int sample);
void jitter(struct vec3 *res, int x, int y, int s);
int ray_object(struct object *obj, struct ray *ray, struct spoint *sp);
int ray_sphere(struct sphere *sph, struct ray *ray, struct spoint *sp);
int ray_plane(struct plane *pln, struct ray *ray, struct spoint *sp);
int ray_aabox(struct aabox *aabox, struct ray *ray);
int box_sphere(struct aabox *box, struct sphere *sph);
int load_scene(FILE *fp);
void print_obj(void);
void print_progr(int x, int max);
unsigned long get_msec(void);
int detect_nproc(void);

struct material *find_material(const char *name);
struct metaball *find_metaball(const char *name);

/* octree.c: spatial index build/query and the repeatable prepare entry point */
int prepare_scene(void);
void free_octree(struct octnode *node);
int build_octree(struct octnode *node, int depth);
int populate_octnode(struct octnode *node, struct object **obj, int num_obj);
int isect_octnode(struct octnode *node, struct ray *ray, struct spoint *spret);
int box_metaball(struct aabox *box, struct metaball *mb);
void metaball_bounds(struct metaball *mb, struct aabox *box);

/* metaball.c: fused ball field production, evaluation and ray intersection */
int ray_metaball(struct metaball *mb, struct ray *ray, struct spoint *sp);
double mb_field(struct metaball *mb, struct vec3 *p);
void mb_gradient(struct metaball *mb, struct vec3 *p, struct vec3 *grad);

#endif	/* CRay_SCENE_H */
