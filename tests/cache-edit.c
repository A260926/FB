/* cache-edit.c - verify the invalidation chain from track edits to the
 * cached hierarchical transformation matrices of libanim.
 *
 * Registered groups: track_edits, ownership, thread_cache.
 * Built and run in both the plain and the ANIM_THREAD_SAFE configuration;
 * the thread_cache cases fall back to equivalent single-threaded numeric
 * checks in the plain build.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "anim.h"
#include "task_support/check.h"

#ifdef ANIM_THREAD_SAFE
#include <pthread.h>
#endif

#define MAT_TOL	1e-3f
#define VAL_TOL	1e-4f

static int nearf(float a, float b)
{
	return fabsf(a - b) <= VAL_TOL;
}

/* ---- independent 4x4 matrix helpers (column-major, OpenGL order) ---- */

static void ref_midentity(float *m)
{
	static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	memcpy(m, id, 16 * sizeof(float));
}

/* r = a * b (column-vector convention: b is applied first, then a) */
static void ref_mmul(float *r, const float *a, const float *b)
{
	int i, j, k;
	float res[16];

	for(j = 0; j < 4; j++) {
		for(i = 0; i < 4; i++) {
			float v = 0.0f;
			for(k = 0; k < 4; k++) {
				v += a[k * 4 + i] * b[j * 4 + k];
			}
			res[j * 4 + i] = v;
		}
	}
	memcpy(r, res, sizeof res);
}

static void ref_xform(const float *m, const float *v, float *out)
{
	int i;
	for(i = 0; i < 3; i++) {
		out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i];
	}
}

static int mat_near(const float *a, const float *b)
{
	int i;
	for(i = 0; i < 16; i++) {
		if(fabsf(a[i] - b[i]) > MAT_TOL) return 0;
	}
	return 1;
}

/* ---- fresh references, re-sampled through the uncached per-node calls ---- */

static void ref_world(struct anm_node *node, float *out, anm_time_t tm)
{
	float local[16];

	anm_get_node_matrix(node, local, tm);
	if(node->parent) {
		float pw[16];
		ref_world(node->parent, pw, tm);
		ref_mmul(out, pw, local);
	} else {
		memcpy(out, local, 16 * sizeof(float));
	}
}

static void ref_inv_world(struct anm_node *node, float *out, anm_time_t tm)
{
	float invlocal[16];

	anm_get_node_inv_matrix(node, invlocal, tm);
	if(node->parent) {
		float piw[16];
		ref_inv_world(node->parent, piw, tm);
		ref_mmul(out, invlocal, piw);
	} else {
		memcpy(out, invlocal, 16 * sizeof(float));
	}
}

/* the cached world/inverse at tm must agree with a fresh re-sample, and
 * must be each other's inverse under our own matrix multiply
 */
static int check_node_fresh(struct anm_node *node, anm_time_t tm)
{
	float m[16], inv[16], rw[16], riw[16], prod[16], id[16];

	anm_get_matrix(node, m, tm);
	anm_get_inv_matrix(node, inv, tm);
	ref_world(node, rw, tm);
	ref_inv_world(node, riw, tm);

	TASK_CHECK(mat_near(m, rw));
	TASK_CHECK(mat_near(inv, riw));

	ref_mmul(prod, m, inv);
	ref_midentity(id);
	TASK_CHECK(mat_near(prod, id));
	return 0;
}

/* transform an actual model-space vertex by the cached world matrix and
 * compare it against the fresh reference world
 */
static int check_vertex(struct anm_node *node, const float *v, anm_time_t tm)
{
	float m[16], rw[16], a[3], b[3];

	anm_get_matrix(node, m, tm);
	ref_world(node, rw, tm);
	ref_xform(m, v, a);
	ref_xform(rw, v, b);
	TASK_CHECK(nearf(a[0], b[0]) && nearf(a[1], b[1]) && nearf(a[2], b[2]));
	return 0;
}

/* white-box: the node's cache for the calling thread is still stamped tm */
static int cache_time_is(struct anm_node *node, anm_time_t tm)
{
#ifdef ANIM_THREAD_SAFE
	struct mat_cache *mc = pthread_getspecific(node->cache_key);
	return mc && mc->time == tm && mc->inv_time == tm;
#else
	return node->cache.time == tm && node->cache.inv_time == tm;
#endif
}

/* ---- multi-level branching skeleton ----
 *            root
 *           /   \
 *          a     b
 *        /  \     \
 *      a1   a2     b1
 */
struct skel {
	struct anm_node root, a, b, a1, a2, b1;
};

static void skel_list(struct skel *s, struct anm_node **n)
{
	n[0] = &s->root;
	n[1] = &s->a;
	n[2] = &s->b;
	n[3] = &s->a1;
	n[4] = &s->a2;
	n[5] = &s->b1;
}

static int skel_init(struct skel *s)
{
	TASK_CHECK(anm_init_node(&s->root) == 0);
	TASK_CHECK(anm_init_node(&s->a) == 0);
	TASK_CHECK(anm_init_node(&s->b) == 0);
	TASK_CHECK(anm_init_node(&s->a1) == 0);
	TASK_CHECK(anm_init_node(&s->a2) == 0);
	TASK_CHECK(anm_init_node(&s->b1) == 0);

	anm_link_node(&s->root, &s->a);
	anm_link_node(&s->root, &s->b);
	anm_link_node(&s->a, &s->a1);
	anm_link_node(&s->a, &s->a2);
	anm_link_node(&s->b, &s->b1);

	anm_set_position3f(&s->root, 0, 0, 0, 0);
	anm_set_position3f(&s->root, 10, 0, 0, 1000);

	anm_set_position3f(&s->a, 0, 2, 0, 0);
	anm_set_position3f(&s->a, 0, 4, 0, 1000);
	anm_set_rotation_axis(&s->a, 0.0f, 0, 1, 0, 0);
	anm_set_rotation_axis(&s->a, 1.0f, 0, 1, 0, 1000);

	anm_set_position3f(&s->b, 0, -2, 0, 0);

	anm_set_position3f(&s->a1, 1, 1, 0, 0);
	anm_set_position3f(&s->a1, 3, 1, 0, 1000);
	anm_set_scaling3f(&s->a1, 1, 1, 1, 0);
	anm_set_scaling3f(&s->a1, 2, 2, 2, 1000);

	anm_set_position3f(&s->a2, -1, 1, 0, 0);
	anm_set_rotation_axis(&s->a2, 0.0f, 1, 0, 0, 0);
	anm_set_rotation_axis(&s->a2, 0.8f, 1, 0, 0, 1000);

	anm_set_position3f(&s->b1, 2, 0, 1, 0);
	anm_set_position3f(&s->b1, 2, 0, 3, 1000);
	return 0;
}

static void skel_destroy(struct skel *s)
{
	anm_destroy_node_tree(&s->root);
}

static void skel_warm(struct skel *s, anm_time_t tm)
{
	struct anm_node *n[6];
	int i;

	skel_list(s, n);
	for(i = 0; i < 6; i++) {
		anm_get_matrix(n[i], 0, tm);
		anm_get_inv_matrix(n[i], 0, tm);
	}
}

static int skel_check(struct skel *s, anm_time_t tm)
{
	struct anm_node *n[6];
	int i;

	skel_list(s, n);
	for(i = 0; i < 6; i++) {
		if(check_node_fresh(n[i], tm)) return 1;
	}
	return 0;
}

/* ---- track_edits ---- */

/* warm the cache, then edit keyframes of an ancestor through the public
 * track API: the whole subtree must follow at the same query time
 */
static int case_keyframe_edits_invalidate(void)
{
	struct skel s;
	static const float vtx[3] = {0.25f, -0.5f, 2.0f};
	struct anm_animation *anim;
	struct anm_keyframe key;
	float before[16], after[16];

	if(skel_init(&s)) return 1;
	skel_warm(&s, 500);
	if(skel_check(&s, 500)) return 1;

	anm_get_matrix(&s.b1, before, 500);

	anim = anm_get_animation(&s.root, 0);
	TASK_CHECK(anim != 0);
	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 60) == 0);

	if(skel_check(&s, 500)) return 1;
	if(check_vertex(&s.a1, vtx, 500)) return 1;

	anm_get_matrix(&s.b1, after, 500);
	TASK_CHECK(!mat_near(before, after));
	TASK_CHECK(nearf(after[12], 37));	/* root x: 60->10 over 0->1000, +2 from b1 */

	/* anm_set_keyframe on a deeper node: update existing, then insert new */
	anim = anm_get_animation(&s.a, 0);
	TASK_CHECK(anim != 0);
	key.time = 1000;
	key.val = -30;
	TASK_CHECK(anm_set_keyframe(anim->tracks + ANM_TRACK_POS_Y, &key) == 0);
	if(skel_check(&s, 500)) return 1;

	key.time = 250;
	key.val = 8;
	TASK_CHECK(anm_set_keyframe(anim->tracks + ANM_TRACK_POS_Y, &key) == 0);
	if(skel_check(&s, 500)) return 1;
	if(check_vertex(&s.a2, vtx, 500)) return 1;

	skel_destroy(&s);
	return 0;
}

/* track defaults, interpolation and extrapolation modes are pose-affecting
 * edits and must invalidate the cached matrices
 */
static int case_default_interp_extrap(void)
{
	struct anm_node n, c;
	struct anm_animation *anim;
	struct anm_track *trk;
	float m[16], id[16];

	TASK_CHECK(anm_init_node(&n) == 0);
	TASK_CHECK(anm_init_node(&c) == 0);
	anm_link_node(&n, &c);
	anim = anm_get_animation(&n, 0);
	TASK_CHECK(anim != 0);

	/* empty tracks: the pose comes from the track defaults (identity) */
	anm_get_matrix(&c, m, 100);
	ref_midentity(id);
	TASK_CHECK(mat_near(m, id));

	trk = anim->tracks + ANM_TRACK_POS_X;
	anm_set_track_default(trk, 7);
	anm_get_matrix(&c, m, 100);
	TASK_CHECK(nearf(m[12], 7));
	if(check_node_fresh(&c, 100)) return 1;
	anm_set_track_default(trk, 0);
	anm_get_matrix(&c, m, 100);
	TASK_CHECK(mat_near(m, id));

	/* interpolation mode */
	trk = anim->tracks + ANM_TRACK_POS_Y;
	TASK_CHECK(anm_set_value(trk, 0, 0) == 0);
	TASK_CHECK(anm_set_value(trk, 1000, 10) == 0);
	anm_get_matrix(&c, m, 500);
	TASK_CHECK(nearf(m[13], 5));
	anm_set_track_interpolator(trk, ANM_INTERP_STEP);
	anm_get_matrix(&c, m, 500);
	TASK_CHECK(nearf(m[13], 0));
	if(check_node_fresh(&c, 500)) return 1;
	anm_set_track_interpolator(trk, ANM_INTERP_LINEAR);
	anm_get_matrix(&c, m, 500);
	TASK_CHECK(nearf(m[13], 5));

	/* extrapolation mode */
	anm_get_matrix(&c, m, 1500);
	TASK_CHECK(nearf(m[13], 10));	/* clamped to the last key */
	anm_set_track_extrapolator(trk, ANM_EXTRAP_REPEAT);
	anm_get_matrix(&c, m, 1500);
	TASK_CHECK(nearf(m[13], 5));	/* 1500 wraps around to 500 */
	if(check_node_fresh(&c, 1500)) return 1;

	/* same operations through the high level node API */
	anm_set_interpolator(&n, ANM_INTERP_STEP);
	anm_get_matrix(&c, m, 500);
	TASK_CHECK(nearf(m[13], 0));
	anm_set_interpolator(&n, ANM_INTERP_LINEAR);
	anm_set_extrapolator(&n, ANM_EXTRAP_CLAMP);
	anm_get_matrix(&c, m, 1500);
	TASK_CHECK(nearf(m[13], 10));
	if(check_node_fresh(&c, 1500)) return 1;

	anm_destroy_node(&c);
	anm_destroy_node(&n);
	return 0;
}

/* high level setters: PRS, pivot, animation offset, active animation
 * blending and transition setup
 */
static int case_high_level_edits(void)
{
	struct skel s;
	struct anm_animation *anim1;
	float before[16], m[16];

	if(skel_init(&s)) return 1;
	skel_warm(&s, 500);
	if(skel_check(&s, 500)) return 1;

	/* PRS setters at the very query time */
	anm_set_position3f(&s.root, 100, 0, 0, 500);
	if(skel_check(&s, 500)) return 1;
	anm_get_matrix(&s.root, m, 500);
	TASK_CHECK(nearf(m[12], 100));

	anm_set_rotation_axis(&s.a, 1.2f, 0, 1, 0, 500);
	if(skel_check(&s, 500)) return 1;

	anm_set_scaling3f(&s.b1, 2, 3, 4, 500);
	if(skel_check(&s, 500)) return 1;

	/* pivot (a1 has non-identity scaling, so the pivot changes the pose) */
	anm_get_matrix(&s.a1, before, 500);
	anm_set_pivot(&s.a1, 0.5f, 0.25f, 0);
	anm_get_matrix(&s.a1, m, 500);
	TASK_CHECK(!mat_near(before, m));
	if(skel_check(&s, 500)) return 1;
	anm_set_pivot(&s.a1, 0, 0, 0);
	if(skel_check(&s, 500)) return 1;

	/* animation time offset shifts the sampled time */
	anm_get_matrix(&s.root, before, 500);
	anm_set_node_animation_offset(&s.root, 200, 0);
	anm_get_matrix(&s.root, m, 500);
	TASK_CHECK(!mat_near(before, m));
	TASK_CHECK(nearf(m[12], 60));	/* root x sampled at 300: 0->100 over 0->500 */
	if(skel_check(&s, 500)) return 1;
	anm_set_node_animation_offset(&s.root, 0, 0);
	if(skel_check(&s, 500)) return 1;

	/* active animation blending */
	TASK_CHECK(anm_add_animation(&s.root) == 0);
	anim1 = anm_get_animation(&s.root, 1);
	TASK_CHECK(anim1 != 0);
	TASK_CHECK(anm_set_value(anim1->tracks + ANM_TRACK_POS_X, 0, 1000) == 0);

	TASK_CHECK(anm_use_animations(&s.root, 0, 1, 0.5f) == 0);
	anm_get_matrix(&s.root, m, 500);
	TASK_CHECK(nearf(m[12], 550));	/* 100 * .5 + 1000 * .5 */
	if(skel_check(&s, 500)) return 1;

	TASK_CHECK(anm_use_animations(&s.root, 0, 1, 0.25f) == 0);
	anm_get_matrix(&s.root, m, 500);
	TASK_CHECK(nearf(m[12], 325));
	if(skel_check(&s, 500)) return 1;

	TASK_CHECK(anm_use_animation(&s.root, 0) == 0);
	if(skel_check(&s, 500)) return 1;

	/* high level transition setup invalidates as well */
	skel_warm(&s, 1500);
	if(skel_check(&s, 1500)) return 1;
	anm_transition(&s.root, 1, 1000, 1000);
	anm_get_matrix(&s.root, m, 1500);
	TASK_CHECK(nearf(m[12], 505));	/* mix .5 at 1500: anim0 10, anim1 1000 */
	if(skel_check(&s, 1500)) return 1;

	skel_destroy(&s);
	return 0;
}

/* editing one tree must not touch the caches of an unrelated tree */
static int case_unrelated_tree_keeps_cache(void)
{
	struct skel s1, s2;
	struct anm_animation *anim;
	float before[16], after[16], binv[16], ainv[16];

	if(skel_init(&s1)) return 1;
	if(skel_init(&s2)) return 1;
	skel_warm(&s1, 500);
	skel_warm(&s2, 500);

	anm_get_matrix(&s2.b1, before, 500);
	anm_get_inv_matrix(&s2.b1, binv, 500);

	anim = anm_get_animation(&s1.root, 0);
	TASK_CHECK(anim != 0);
	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 77) == 0);
	if(skel_check(&s1, 500)) return 1;

	anm_get_matrix(&s2.b1, after, 500);
	anm_get_inv_matrix(&s2.b1, ainv, 500);
	TASK_CHECK(mat_near(before, after));
	TASK_CHECK(mat_near(binv, ainv));
	/* the other tree's cache entries were not even invalidated */
	TASK_CHECK(cache_time_is(&s2.b1, 500));
	TASK_CHECK(cache_time_is(&s2.root, 500));

	skel_destroy(&s1);
	skel_destroy(&s2);
	return 0;
}

/* renaming nodes, animations or tracks is not a pose change */
static int case_name_edits_not_pose(void)
{
	struct skel s;
	struct anm_animation *anim;
	float before[16], after[16];

	if(skel_init(&s)) return 1;
	skel_warm(&s, 500);
	anm_get_matrix(&s.a1, before, 500);

	TASK_CHECK(anm_set_node_name(&s.a1, "renamed") == 0);
	anm_set_node_active_animation_name(&s.a1, "gait");
	anim = anm_get_animation(&s.a1, 0);
	TASK_CHECK(anim != 0);
	TASK_CHECK(anm_set_track_name(anim->tracks + ANM_TRACK_POS_X, "posx") == 0);

	TASK_CHECK(strcmp(anm_get_node_name(&s.a1), "renamed") == 0);
	TASK_CHECK(strcmp(anm_get_active_animation_name(&s.a1), "gait") == 0);
	TASK_CHECK(strcmp(anm_get_track_name(anim->tracks + ANM_TRACK_POS_X), "posx") == 0);

	anm_get_matrix(&s.a1, after, 500);
	TASK_CHECK(mat_near(before, after));
	TASK_CHECK(cache_time_is(&s.a1, 500));	/* renames didn't invalidate */

	skel_destroy(&s);
	return 0;
}

/* ---- ownership ---- */

/* a track copied out of a node into a standalone track keeps no link to
 * the source node, and survives its destruction
 */
static int case_copy_to_standalone_detaches(void)
{
	struct anm_node n;
	struct anm_animation *anim;
	struct anm_track *solo;
	float before[16], after[16];

	TASK_CHECK(anm_init_node(&n) == 0);
	anim = anm_get_animation(&n, 0);
	TASK_CHECK(anim != 0);
	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 5) == 0);
	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 1000, 15) == 0);

	anm_get_matrix(&n, before, 500);
	anm_get_inv_matrix(&n, 0, 500);
	TASK_CHECK(nearf(before[12], 10));

	solo = anm_create_track();
	TASK_CHECK(solo != 0);
	anm_copy_track(solo, anim->tracks + ANM_TRACK_POS_X);
	TASK_CHECK(nearf(anm_get_value(solo, 500), 10));

	/* editing the standalone copy must not reach the node's cache */
	TASK_CHECK(anm_set_value(solo, 0, -100) == 0);
	anm_get_matrix(&n, after, 500);
	TASK_CHECK(mat_near(before, after));
	TASK_CHECK(cache_time_is(&n, 500));

	/* nor the other way around: the copy is independent storage */
	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 1) == 0);
	TASK_CHECK(nearf(anm_get_value(solo, 0), -100));
	anm_get_matrix(&n, after, 500);
	TASK_CHECK(nearf(after[12], 8));	/* 1 -> 15 over 0 -> 1000 */

	/* the standalone copy keeps working after the source node is gone */
	anm_destroy_node(&n);
	TASK_CHECK(nearf(anm_get_value(solo, 500), -42.5f));
	TASK_CHECK(anm_set_value(solo, 500, 3) == 0);
	TASK_CHECK(nearf(anm_get_value(solo, 500), 3));
	anm_free_track(solo);
	return 0;
}

/* copying a track into another node's track notifies the destination
 * owner (and its subtree), never the source owner
 */
static int case_copy_between_nodes(void)
{
	struct anm_node na, nb, nc;
	struct anm_animation *anim_a, *anim_b;
	float m[16], before_a[16], after_a[16];

	TASK_CHECK(anm_init_node(&na) == 0);
	TASK_CHECK(anm_init_node(&nb) == 0);
	TASK_CHECK(anm_init_node(&nc) == 0);
	anm_link_node(&nb, &nc);

	anim_a = anm_get_animation(&na, 0);
	anim_b = anm_get_animation(&nb, 0);
	TASK_CHECK(anim_a != 0 && anim_b != 0);

	TASK_CHECK(anm_set_value(anim_a->tracks + ANM_TRACK_POS_X, 0, 3) == 0);
	TASK_CHECK(anm_set_value(anim_a->tracks + ANM_TRACK_POS_X, 1000, 9) == 0);
	TASK_CHECK(anm_set_value(anim_b->tracks + ANM_TRACK_POS_X, 0, 100) == 0);
	TASK_CHECK(anm_set_value(anim_b->tracks + ANM_TRACK_POS_X, 1000, 200) == 0);
	TASK_CHECK(anm_set_value(anm_get_animation(&nc, 0)->tracks + ANM_TRACK_POS_X, 0, 2) == 0);

	anm_get_matrix(&na, before_a, 500);
	anm_get_inv_matrix(&na, 0, 500);
	anm_get_matrix(&nb, m, 500);
	anm_get_inv_matrix(&nb, 0, 500);
	anm_get_matrix(&nc, 0, 500);
	anm_get_inv_matrix(&nc, 0, 500);
	TASK_CHECK(nearf(m[12], 150));

	anm_copy_track(anim_b->tracks + ANM_TRACK_POS_X, anim_a->tracks + ANM_TRACK_POS_X);
	anm_get_matrix(&nb, m, 500);
	TASK_CHECK(nearf(m[12], 6));	/* now follows A's track: 3 -> 9 */
	anm_get_matrix(&nc, m, 500);
	TASK_CHECK(nearf(m[12], 8));	/* child of B follows along */
	if(check_node_fresh(&nb, 500)) return 1;
	if(check_node_fresh(&nc, 500)) return 1;

	/* the source node is untouched: same values, cache still valid */
	anm_get_matrix(&na, after_a, 500);
	TASK_CHECK(mat_near(before_a, after_a));
	TASK_CHECK(cache_time_is(&na, 500));

	anm_destroy_node(&nc);
	anm_destroy_node(&nb);
	anm_destroy_node(&na);
	return 0;
}

/* animation slot growth reallocates the array; re-fetched track pointers
 * keep their notification wiring
 */
static int case_slot_growth_refetch(void)
{
	struct anm_node n;
	struct anm_animation *anim;
	float m[16];
	int i;

	TASK_CHECK(anm_init_node(&n) == 0);
	anim = anm_get_animation(&n, 0);
	TASK_CHECK(anim != 0);
	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 1) == 0);
	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 1000, 11) == 0);
	anm_get_matrix(&n, m, 500);
	TASK_CHECK(nearf(m[12], 6));

	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 2) == 0);
	anm_get_matrix(&n, m, 500);
	TASK_CHECK(nearf(m[12], 6.5f));

	for(i = 0; i < 8; i++) {
		TASK_CHECK(anm_add_node_animation(&n) == 0);
	}
	TASK_CHECK(anm_get_animation_count(&n) == 9);
	TASK_CHECK(anm_get_animation(&n, 9) == 0);
	TASK_CHECK(anm_get_animation(&n, -1) == 0);

	/* re-fetch after growth: editing still invalidates */
	anim = anm_get_animation(&n, 0);
	TASK_CHECK(anim != 0);
	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 4) == 0);
	anm_get_matrix(&n, m, 500);
	TASK_CHECK(nearf(m[12], 7.5f));

	/* a freshly added slot is wired to the node as well */
	TASK_CHECK(anm_use_node_animation(&n, 8) == 0);
	anim = anm_get_animation(&n, 8);
	TASK_CHECK(anim != 0);
	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 50) == 0);
	anm_get_matrix(&n, m, 500);
	TASK_CHECK(nearf(m[12], 50));
	if(check_node_fresh(&n, 500)) return 1;

	anm_destroy_node(&n);
	return 0;
}

/* construction, copying and destruction of standalone and node-embedded
 * tracks must not leave dangling ownership behind
 */
static int case_destroy_keeps_standalone(void)
{
	struct anm_node *root, *kid;
	struct anm_animation *anim;
	struct anm_track *copy, *solo;
	float m[16];

	root = anm_create_node();
	kid = anm_create_node();
	copy = anm_create_track();
	solo = anm_create_track();
	TASK_CHECK(root != 0 && kid != 0 && copy != 0 && solo != 0);
	anm_link_node(root, kid);

	anim = anm_get_animation(kid, 0);
	TASK_CHECK(anim != 0);
	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_Y, 0, 8) == 0);
	anm_copy_track(copy, anim->tracks + ANM_TRACK_POS_Y);

	/* copy from a standalone track into a node-owned track notifies the node */
	TASK_CHECK(anm_set_value(solo, 0, 4) == 0);
	TASK_CHECK(anm_set_value(solo, 1000, 14) == 0);
	anim = anm_get_animation(root, 0);
	TASK_CHECK(anim != 0);
	anm_get_matrix(root, m, 500);	/* warm the cache */
	anm_copy_track(anim->tracks + ANM_TRACK_POS_X, solo);
	anm_get_matrix(root, m, 500);
	TASK_CHECK(nearf(m[12], 9));

	/* destroying the standalone source leaves the node's copy intact */
	anm_free_track(solo);
	anm_get_matrix(root, m, 500);
	TASK_CHECK(nearf(m[12], 9));

	/* destroying the whole node tree leaves the standalone copy intact */
	anm_free_node_tree(root);
	TASK_CHECK(nearf(anm_get_value(copy, 0), 8));
	TASK_CHECK(anm_set_value(copy, 500, 3) == 0);
	TASK_CHECK(nearf(anm_get_value(copy, 500), 3));
	anm_free_track(copy);
	return 0;
}

/* ---- thread_cache ---- */

/* a fresh node queried through its inverse first (at time 0) must not
 * hand back uninitialized cache contents
 */
static int case_inv_first_query(void)
{
	struct anm_node n, c, p, q;
	float m[16], inv[16], rw[16], riw[16], prod[16], id[16];

	TASK_CHECK(anm_init_node(&n) == 0);
	TASK_CHECK(anm_init_node(&c) == 0);
	anm_link_node(&n, &c);

	ref_midentity(id);
	anm_get_inv_matrix(&c, inv, 0);	/* inverse first, at time 0 */
	TASK_CHECK(mat_near(inv, id));
	anm_get_matrix(&c, m, 0);
	TASK_CHECK(mat_near(m, id));
	ref_mmul(prod, m, inv);
	TASK_CHECK(mat_near(prod, id));

	anm_destroy_node(&c);
	anm_destroy_node(&n);

	/* same order (inverse first) on a posed chain, at a non-zero time */
	TASK_CHECK(anm_init_node(&p) == 0);
	TASK_CHECK(anm_init_node(&q) == 0);
	anm_link_node(&p, &q);
	anm_set_position3f(&p, 4, 0, 0, 0);
	anm_set_position3f(&q, 0, 6, 0, 0);

	anm_get_inv_matrix(&q, inv, 250);
	ref_inv_world(&q, riw, 250);
	TASK_CHECK(mat_near(inv, riw));
	TASK_CHECK(nearf(inv[12], -4) && nearf(inv[13], -6));
	anm_get_matrix(&q, m, 250);
	ref_world(&q, rw, 250);
	TASK_CHECK(mat_near(m, rw));
	TASK_CHECK(nearf(m[12], 4) && nearf(m[13], 6));
	ref_mmul(prod, m, inv);
	TASK_CHECK(mat_near(prod, id));

	anm_destroy_node(&q);
	anm_destroy_node(&p);
	return 0;
}

#ifdef ANIM_THREAD_SAFE
struct phase_shared {
	struct anm_node *leaf;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int phase;		/* advances after each edit */
	int arrivals;		/* worker query completions */
	float m[2][3][16];
	float inv[2][3][16];
};

struct phase_arg {
	struct phase_shared *sh;
	int slot;
};

static void *phase_worker(void *arg)
{
	struct phase_arg *wa = arg;
	struct phase_shared *sh = wa->sh;
	int p;

	for(p = 0; p < 3; p++) {
		/* query phase: both workers may query concurrently, each
		 * reusing its own thread-specific cache across the phases */
		anm_get_matrix(sh->leaf, sh->m[wa->slot][p], 500);
		anm_get_inv_matrix(sh->leaf, sh->inv[wa->slot][p], 500);

		pthread_mutex_lock(&sh->lock);
		sh->arrivals++;
		pthread_cond_broadcast(&sh->cond);
		while(sh->phase <= p) {
			pthread_cond_wait(&sh->cond, &sh->lock);
		}
		pthread_mutex_unlock(&sh->lock);
	}
	return 0;
}
#endif	/* ANIM_THREAD_SAFE */

/* two worker threads keep querying the same skeleton across edit phases,
 * synchronized with a mutex/condition variable: edits and queries never
 * overlap, and every edit must invalidate the workers' caches
 */
static int case_two_workers_cross_edit(void)
{
	static const float expect_x[3] = {7, 37, 202};
	struct skel s;
	struct anm_animation *anim;
	float m[16], inv[16], rw[16], riw[16];

	if(skel_init(&s)) return 1;
	skel_warm(&s, 500);

#ifdef ANIM_THREAD_SAFE
	{
		struct phase_shared sh;
		struct phase_arg warg[2];
		pthread_t th[2];
		float prod[16], id[16];
		int i, p;

		memset(&sh, 0, sizeof sh);
		sh.leaf = &s.b1;
		TASK_CHECK(pthread_mutex_init(&sh.lock, 0) == 0);
		TASK_CHECK(pthread_cond_init(&sh.cond, 0) == 0);

		for(i = 0; i < 2; i++) {
			warg[i].sh = &sh;
			warg[i].slot = i;
			TASK_CHECK(pthread_create(th + i, 0, phase_worker, warg + i) == 0);
		}

		for(p = 0; p < 3; p++) {
			pthread_mutex_lock(&sh.lock);
			while(sh.arrivals < 2 * (p + 1)) {
				pthread_cond_wait(&sh.cond, &sh.lock);
			}
			/* edit phase: both workers are parked, no query in flight */
			if(p == 0) {
				/* low level track edit on the root */
				anim = anm_get_animation(&s.root, 0);
				TASK_CHECK(anim != 0);
				TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 60) == 0);
			} else if(p == 1) {
				/* the high level setter hits the same chain */
				anm_set_position3f(&s.root, 200, 0, 0, 500);
			}
			sh.phase++;
			pthread_cond_broadcast(&sh.cond);
			pthread_mutex_unlock(&sh.lock);
		}

		for(i = 0; i < 2; i++) {
			TASK_CHECK(pthread_join(th[i], 0) == 0);
		}
		TASK_CHECK(pthread_mutex_destroy(&sh.lock) == 0);
		TASK_CHECK(pthread_cond_destroy(&sh.cond) == 0);

		/* both workers must have seen the current values in every phase */
		ref_midentity(id);
		for(i = 0; i < 2; i++) {
			for(p = 0; p < 3; p++) {
				TASK_CHECK(nearf(sh.m[i][p][12], expect_x[p]));
				TASK_CHECK(nearf(sh.m[i][p][13], -2));
				TASK_CHECK(nearf(sh.inv[i][p][12], -expect_x[p]));
				ref_mmul(prod, sh.m[i][p], sh.inv[i][p]);
				TASK_CHECK(mat_near(prod, id));
			}
		}
	}
#else
	/* single-threaded equivalent: same edits, same expected numbers */
	anm_get_matrix(&s.b1, m, 500);
	anm_get_inv_matrix(&s.b1, inv, 500);
	TASK_CHECK(nearf(m[12], expect_x[0]));
	TASK_CHECK(nearf(inv[12], -expect_x[0]));

	anim = anm_get_animation(&s.root, 0);
	TASK_CHECK(anim != 0);
	TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 60) == 0);
	anm_get_matrix(&s.b1, m, 500);
	anm_get_inv_matrix(&s.b1, inv, 500);
	TASK_CHECK(nearf(m[12], expect_x[1]));
	TASK_CHECK(nearf(inv[12], -expect_x[1]));

	anm_set_position3f(&s.root, 200, 0, 0, 500);
	anm_get_matrix(&s.b1, m, 500);
	anm_get_inv_matrix(&s.b1, inv, 500);
	TASK_CHECK(nearf(m[12], expect_x[2]));
	TASK_CHECK(nearf(inv[12], -expect_x[2]));
#endif	/* ANIM_THREAD_SAFE */

	/* the main thread's own cache agrees with a fresh sample as well */
	anm_get_matrix(&s.b1, m, 500);
	anm_get_inv_matrix(&s.b1, inv, 500);
	ref_world(&s.b1, rw, 500);
	ref_inv_world(&s.b1, riw, 500);
	TASK_CHECK(mat_near(m, rw));
	TASK_CHECK(mat_near(inv, riw));
	TASK_CHECK(nearf(m[12], expect_x[2]));

	skel_destroy(&s);
	return 0;
}

#ifdef ANIM_THREAD_SAFE
struct inv_shared {
	struct anm_node *leaf;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int phase;
	int arrivals;
	float inv0[16], m0[16], inv1[16], m1[16];
};

static void *inv_first_worker(void *arg)
{
	struct inv_shared *sh = arg;

	anm_get_inv_matrix(sh->leaf, sh->inv0, 500);	/* inverse before forward */
	anm_get_matrix(sh->leaf, sh->m0, 500);

	pthread_mutex_lock(&sh->lock);
	sh->arrivals++;
	pthread_cond_broadcast(&sh->cond);
	while(sh->phase == 0) {
		pthread_cond_wait(&sh->cond, &sh->lock);
	}
	pthread_mutex_unlock(&sh->lock);

	anm_get_inv_matrix(sh->leaf, sh->inv1, 500);
	anm_get_matrix(sh->leaf, sh->m1, 500);

	pthread_mutex_lock(&sh->lock);
	sh->arrivals++;
	pthread_cond_broadcast(&sh->cond);
	pthread_mutex_unlock(&sh->lock);
	return 0;
}
#endif	/* ANIM_THREAD_SAFE */

/* a worker which queries the inverse before the forward matrix must get
 * consistent results both before and after an edit
 */
static int case_worker_inv_first(void)
{
	struct skel s;
	struct anm_animation *anim;
	float prod[16], id[16];

	if(skel_init(&s)) return 1;
	ref_midentity(id);

#ifdef ANIM_THREAD_SAFE
	{
		struct inv_shared sh;
		pthread_t th;

		memset(&sh, 0, sizeof sh);
		sh.leaf = &s.b1;
		TASK_CHECK(pthread_mutex_init(&sh.lock, 0) == 0);
		TASK_CHECK(pthread_cond_init(&sh.cond, 0) == 0);
		TASK_CHECK(pthread_create(&th, 0, inv_first_worker, &sh) == 0);

		pthread_mutex_lock(&sh.lock);
		while(sh.arrivals < 1) {
			pthread_cond_wait(&sh.cond, &sh.lock);
		}
		anim = anm_get_animation(&s.root, 0);
		TASK_CHECK(anim != 0);
		TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 60) == 0);
		sh.phase = 1;
		pthread_cond_broadcast(&sh.cond);
		pthread_mutex_unlock(&sh.lock);

		pthread_mutex_lock(&sh.lock);
		while(sh.arrivals < 2) {
			pthread_cond_wait(&sh.cond, &sh.lock);
		}
		pthread_mutex_unlock(&sh.lock);
		TASK_CHECK(pthread_join(th, 0) == 0);

		TASK_CHECK(nearf(sh.inv0[12], -7));
		TASK_CHECK(nearf(sh.m0[12], 7));
		TASK_CHECK(nearf(sh.inv1[12], -37));
		TASK_CHECK(nearf(sh.m1[12], 37));
		ref_mmul(prod, sh.m1, sh.inv1);
		TASK_CHECK(mat_near(prod, id));

		TASK_CHECK(pthread_mutex_destroy(&sh.lock) == 0);
		TASK_CHECK(pthread_cond_destroy(&sh.cond) == 0);
	}
#else
	{
		float m[16], inv[16];

		skel_warm(&s, 500);
		anm_get_inv_matrix(&s.b1, inv, 500);
		anm_get_matrix(&s.b1, m, 500);
		TASK_CHECK(nearf(inv[12], -7));
		TASK_CHECK(nearf(m[12], 7));

		anim = anm_get_animation(&s.root, 0);
		TASK_CHECK(anim != 0);
		TASK_CHECK(anm_set_value(anim->tracks + ANM_TRACK_POS_X, 0, 60) == 0);

		anm_get_inv_matrix(&s.b1, inv, 500);
		anm_get_matrix(&s.b1, m, 500);
		TASK_CHECK(nearf(inv[12], -37));
		TASK_CHECK(nearf(m[12], 37));
		ref_mmul(prod, m, inv);
		TASK_CHECK(mat_near(prod, id));
	}
#endif	/* ANIM_THREAD_SAFE */

	skel_destroy(&s);
	return 0;
}

const struct task_case task_cases[] = {
	{"track_edits", "keyframe_edits_invalidate", case_keyframe_edits_invalidate},
	{"track_edits", "default_interp_extrap", case_default_interp_extrap},
	{"track_edits", "high_level_edits", case_high_level_edits},
	{"track_edits", "unrelated_tree_keeps_cache", case_unrelated_tree_keeps_cache},
	{"track_edits", "name_edits_not_pose", case_name_edits_not_pose},

	{"ownership", "copy_to_standalone_detaches", case_copy_to_standalone_detaches},
	{"ownership", "copy_between_nodes", case_copy_between_nodes},
	{"ownership", "slot_growth_refetch", case_slot_growth_refetch},
	{"ownership", "destroy_keeps_standalone", case_destroy_keeps_standalone},

	{"thread_cache", "inv_first_query", case_inv_first_query},
	{"thread_cache", "two_workers_cross_edit", case_two_workers_cross_edit},
	{"thread_cache", "worker_inv_first", case_worker_inv_first}
};

const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];
