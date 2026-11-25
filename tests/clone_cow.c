/* Tests for shared-curve character snapshots (anm_clone_node_tree) and
 * copy-on-write track sharing (anm_share_track).
 *
 * Covers three groups, registered through the task_cases table:
 *   sharing  - buffer addresses, copy-on-write detach scope, empty tracks
 *   playback - actual sampled poses (matrices, inverses, skinned vertices)
 *   lifetime - destroy order, nested snapshots, source-before-copy teardown
 */
#include "../task_support/check.h"
#include "anim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NEAR(a, b)	(fabsf((a) - (b)) < 1e-4f)

static int mat_near(const float *a, const float *b)
{
	int i;
	for(i=0; i<16; i++) {
		if(!NEAR(a[i], b[i])) return 0;
	}
	return 1;
}

/* apply a row-major 4x4 matrix to a point (x, y, z, 1), as cgmath does */
static void xform_pt(const float *m, float x, float y, float z, float *res)
{
	res[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
	res[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
	res[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

/* world * inverse-bind applied to a bind-space vertex */
static void skin_pt(const float *world, const float *invbind, float x, float y, float z, float *res)
{
	float tmp[3], full[16];
	int i, j;
	/* full = world * invbind */
	for(i=0; i<4; i++) {
		for(j=0; j<4; j++) {
			full[i * 4 + j] = world[i * 4 + 0] * invbind[0 + j] +
				world[i * 4 + 1] * invbind[4 + j] +
				world[i * 4 + 2] * invbind[8 + j] +
				world[i * 4 + 3] * invbind[12 + j];
		}
	}
	xform_pt(full, x, y, z, tmp);
	res[0] = tmp[0]; res[1] = tmp[1]; res[2] = tmp[2];
}

static const struct anm_track *ctrack(const struct anm_node *node, int aidx, int tidx)
{
	return node->animations[aidx].tracks + tidx;
}

static struct anm_track *mtrack(struct anm_node *node, int aidx, int tidx)
{
	return node->animations[aidx].tracks + tidx;
}

/* walk the child chain looking for a named node */
static struct anm_node *find_child(struct anm_node *parent, const char *name)
{
	struct anm_node *c;
	for(c = parent->child; c; c = c->next) {
		if(c->name && strcmp(c->name, name) == 0) return c;
	}
	return NULL;
}

/* fill a sparse position track: keys at 0, 500, 1500 (non-contiguous) */
static void sparse_pos(struct anm_node *node, int aidx, float base)
{
	anm_use_node_animation(node, aidx);
	anm_set_position3f(node, base, 0, 0, 0);
	anm_set_position3f(node, base, 0, 0, 500);
	anm_set_position3f(node, base + 2, 0, 0, 1500);
}


/* ------------------------------------------------------------------ */
/* group: sharing                                                      */
/* ------------------------------------------------------------------ */

/* sharing address identity and detach scope on a sparse, non-empty track */
static int case_share_address(void)
{
	struct anm_track a, b, c;
	int idx;

	TASK_CHECK(!anm_init_track(&a));
	TASK_CHECK(!anm_init_track(&b));
	TASK_CHECK(!anm_init_track(&c));

	/* sparse track: 0, 500, 1500 */
	anm_set_value(&a, 0, 1.0f);
	anm_set_value(&a, 500, 3.0f);
	anm_set_value(&a, 1500, 5.0f);

	/* self share: success and nothing changes */
	idx = a.count;
	TASK_CHECK(anm_share_track(&a, &a) == 0);
	TASK_CHECK(a.count == idx);
	TASK_CHECK(NEAR(anm_get_value(&a, 500), 3.0f));

	/* share into b and c: same key buffer address, same sampled values */
	TASK_CHECK(anm_share_track(&b, &a) == 0);
	TASK_CHECK(anm_share_track(&c, &a) == 0);
	TASK_CHECK(a.keys == b.keys && a.keys == c.keys);
	TASK_CHECK(NEAR(anm_get_value(&b, 500), 3.0f));
	TASK_CHECK(NEAR(anm_get_value(&c, 750), 3.5f));

	/* tracks stay independent in their scalar settings */
	anm_set_track_default(&b, 42.0f);
	anm_set_track_interpolator(&b, ANM_INTERP_STEP);
	TASK_CHECK(NEAR(anm_get_value(&b, 1000), 3.0f));	/* step at 500 */
	TASK_CHECK(NEAR(anm_get_value(&a, 1000), 4.0f));	/* linear */

	/* overwrite an existing key in b: only b's buffer detaches */
	TASK_CHECK(!anm_set_value(&b, 500, 30.0f));
	TASK_CHECK(b.keys != a.keys);
	TASK_CHECK(a.keys == c.keys);
	TASK_CHECK(NEAR(anm_get_value(&a, 500), 3.0f));
	TASK_CHECK(NEAR(anm_get_value(&c, 500), 3.0f));
	TASK_CHECK(NEAR(anm_get_value(&b, 500), 30.0f));

	/* insert a middle key in c: only c's buffer detaches */
	TASK_CHECK(!anm_set_value(&c, 750, 7.0f));
	TASK_CHECK(c.keys != a.keys);
	TASK_CHECK(c.count == 4);
	/* a keeps its original key order and values */
	TASK_CHECK(a.count == 3);
	TASK_CHECK(a.keys[0].time == 0 && a.keys[1].time == 500 && a.keys[2].time == 1500);
	TASK_CHECK(NEAR(anm_get_value(&a, 750), 3.5f));
	/* c sees its own inserted key */
	TASK_CHECK(NEAR(anm_get_keyframe(&c, 2)->val, 7.0f));

	anm_destroy_track(&a);
	anm_destroy_track(&b);
	anm_destroy_track(&c);
	return 0;
}

/* empty tracks share and detach correctly, and the failure contract */
static int case_share_empty(void)
{
	struct anm_track a, b, uninit;
	int i;

	TASK_CHECK(!anm_init_track(&a));
	TASK_CHECK(!anm_init_track(&b));

	/* empty track: shares, and a write detaches without disturbing source */
	TASK_CHECK(anm_share_track(&b, &a) == 0);
	TASK_CHECK(a.keys == b.keys);
	TASK_CHECK(!anm_set_value(&b, 100, 9.0f));
	TASK_CHECK(b.keys != a.keys);
	TASK_CHECK(a.count == 0 && b.count == 1);
	TASK_CHECK(NEAR(anm_get_value(&a, 100), 0.0f));	/* default */

	/* self share succeeds and changes nothing */
	TASK_CHECK(anm_share_track(&a, &a) == 0);
	TASK_CHECK(a.count == 0);

	/* uninitialized destination: fail, destination untouched */
	memset(&uninit, 0, sizeof uninit);
	i = uninit.count;
	TASK_CHECK(anm_share_track(&uninit, &a) == -1);
	TASK_CHECK(uninit.count == i && uninit.keys == NULL);

	/* uninitialized source: fail */
	TASK_CHECK(anm_share_track(&a, &uninit) == -1);

	anm_destroy_track(&a);
	anm_destroy_track(&b);
	return 0;
}

/* anm_copy_track must remain a deep copy with distinct key addresses */
static int case_copy_is_deep(void)
{
	struct anm_track a, b;

	TASK_CHECK(!anm_init_track(&a));
	TASK_CHECK(!anm_init_track(&b));
	anm_set_value(&a, 0, 1.0f);
	anm_set_value(&a, 1000, 5.0f);

	anm_copy_track(&b, &a);
	TASK_CHECK(a.keys != b.keys);
	TASK_CHECK(NEAR(anm_get_value(&b, 500), 3.0f));

	TASK_CHECK(!anm_set_value(&a, 0, 9.0f));
	TASK_CHECK(NEAR(anm_get_value(&b, 500), 3.0f));

	anm_destroy_track(&a);
	anm_destroy_track(&b);
	return 0;
}

/* multiple animation slots are all snapshotted and share their buffers */
static int case_share_multi_anim(void)
{
	struct anm_node *src, *copy;
	int i, num;

	src = anm_create_node();
	TASK_CHECK(src);

	/* two animation slots, distinct sparse tracks */
	anm_use_node_animation(src, 0);
	anm_set_animation_name(anm_get_animation(src, 0), "walk");
	anm_set_position3f(src, 1, 0, 0, 0);
	anm_set_position3f(src, 3, 0, 0, 1000);

	TASK_CHECK(anm_add_node_animation(src) == 0);
	anm_set_animation_name(anm_get_animation(src, 1), "jump");
	anm_use_node_animation(src, 1);
	anm_set_position3f(src, 10, 0, 0, 0);
	anm_set_position3f(src, 20, 0, 0, 1000);

	num = anm_get_animation_count(src);
	TASK_CHECK(num == 2);
	anm_use_node_animation(src, 0);

	copy = anm_clone_node_tree(src);
	TASK_CHECK(copy);
	TASK_CHECK(anm_get_animation_count(copy) == num);

	for(i=0; i<num; i++) {
		int j;
		TASK_CHECK(strcmp(copy->animations[i].name, src->animations[i].name) == 0);
		for(j=0; j<ANM_NUM_TRACKS; j++) {
			TASK_CHECK(ctrack(copy, i, j)->keys == ctrack(src, i, j)->keys);
			TASK_CHECK(ctrack(copy, i, j)->count == ctrack(src, i, j)->count);
		}
	}

	/* editing slot 1 of the copy must not touch the source */
	anm_use_node_animation(copy, 1);
	anm_set_position3f(copy, 99, 0, 0, 500);
	TASK_CHECK(ctrack(copy, 1, ANM_TRACK_POS_X)->keys !=
			ctrack(src, 1, ANM_TRACK_POS_X)->keys);
	/* slot 0 is still shared */
	TASK_CHECK(ctrack(copy, 0, ANM_TRACK_POS_X)->keys ==
			ctrack(src, 0, ANM_TRACK_POS_X)->keys);
	TASK_CHECK(NEAR(anm_get_value(ctrack(src, 1, ANM_TRACK_POS_X), 500), 15.0f));

	anm_free_node_tree(src);
	anm_free_node_tree(copy);
	return 0;
}

/* anm_share_track, tree snapshots, and nested snapshots mixed together */
static int case_share_with_clone(void)
{
	struct anm_node *src, *copy;
	struct anm_track ext;
	int posx = ANM_TRACK_POS_X;

	src = anm_create_node();
	TASK_CHECK(src);
	anm_use_node_animation(src, 0);
	anm_set_position3f(src, 2, 0, 0, 0);
	anm_set_position3f(src, 6, 0, 0, 1000);

	copy = anm_clone_node_tree(src);
	TASK_CHECK(copy);

	/* share the node's track with a standalone track */
	TASK_CHECK(!anm_init_track(&ext));
	TASK_CHECK(anm_share_track(&ext, mtrack(src, 0, posx)) == 0);

	/* node, snapshot, and standalone track all reference the same buffer */
	TASK_CHECK(mtrack(src, 0, posx)->keys == mtrack(copy, 0, posx)->keys);
	TASK_CHECK(ext.keys == mtrack(src, 0, posx)->keys);
	TASK_CHECK(NEAR(anm_get_value(&ext, 500), 4.0f));

	/* edit the standalone track: only it detaches */
	TASK_CHECK(!anm_set_value(&ext, 500, 50.0f));
	TASK_CHECK(ext.keys != mtrack(src, 0, posx)->keys);
	TASK_CHECK(mtrack(src, 0, posx)->keys == mtrack(copy, 0, posx)->keys);
	TASK_CHECK(NEAR(anm_get_value(&ext, 500), 50.0f));
	TASK_CHECK(NEAR(anm_get_value(mtrack(src, 0, posx), 500), 4.0f));

	/* edit the snapshot: only the snapshot detaches */
	anm_use_node_animation(copy, 0);
	anm_set_position3f(copy, 60, 0, 0, 500);
	TASK_CHECK(mtrack(copy, 0, posx)->keys != mtrack(src, 0, posx)->keys);
	TASK_CHECK(NEAR(anm_get_value(mtrack(src, 0, posx), 500), 4.0f));

	/* the standalone track survives its source's and sibling's teardown */
	anm_free_node_tree(src);
	anm_free_node_tree(copy);
	TASK_CHECK(NEAR(anm_get_value(&ext, 500), 50.0f));
	anm_destroy_track(&ext);
	return 0;
}

/* ------------------------------------------------------------------ */
/* group: playback                                                     */
/* ------------------------------------------------------------------ */

/* build a forked skeleton with pivots; returns root, sets named outputs */
static struct anm_node *build_fork(struct anm_node **mid, struct anm_node **leaf,
		struct anm_node **branch)
{
	struct anm_node *root = anm_create_node();
	struct anm_node *m = anm_create_node();
	struct anm_node *l = anm_create_node();
	struct anm_node *b = anm_create_node();

	if(!root || !m || !l || !b) {
		return 0;
	}

	anm_link_node(root, m);
	anm_link_node(root, b);
	anm_link_node(m, l);

	anm_set_node_name(root, "root");
	anm_set_node_name(m, "mid");
	anm_set_node_name(l, "leaf");
	anm_set_node_name(b, "branch");

	anm_set_pivot(root, 0.5f, 1.0f, 0.0f);
	anm_set_pivot(m, 0.0f, -1.0f, 0.0f);
	anm_set_pivot(l, 0.25f, 0.0f, 0.0f);
	anm_set_pivot(b, -0.5f, 0.0f, 0.0f);

	anm_use_node_animation(root, 0);
	anm_use_node_animation(m, 0);
	anm_use_node_animation(l, 0);
	anm_use_node_animation(b, 0);

	anm_set_position3f(root, 1, 0, 0, 0);
	anm_set_position3f(m, 0, 2, 0, 0);
	anm_set_position3f(l, 0, 0, 1, 0);
	anm_set_position3f(b, 0, 0, 0, 0);

	*mid = m; *leaf = l; *branch = b;
	return root;
}

static int case_playback_world_inverse(void)
{
	struct anm_node *root, *mid, *leaf, *branch;
	struct anm_node *copy, *cmid, *cleaf, *cbranch;
	float w1[16], i1[16], w2[16], i2[16];
	float v1[3], v2[3];

	root = build_fork(&mid, &leaf, &branch);
	copy = anm_clone_node_tree(root);
	TASK_CHECK(copy);

	cmid = find_child(copy, "mid");
	cbranch = find_child(copy, "branch");
	TASK_CHECK(cmid && cbranch);
	cleaf = find_child(cmid, "leaf");
	TASK_CHECK(cleaf);
	/* preserved child order: branch was linked later so it heads the chain */
	TASK_CHECK(cbranch->next == cmid && cmid->next == NULL);

	/* hierarchy and pivots preserved */
	TASK_CHECK(cmid->parent == copy && cleaf->parent == cmid);
	TASK_CHECK(NEAR(cleaf->pivot[0], 0.25f) && NEAR(cleaf->pivot[1], 0.0f));

	anm_get_matrix(leaf, w1, 0);
	anm_get_inv_matrix(leaf, i1, 0);
	anm_get_matrix(cleaf, w2, 0);
	anm_get_inv_matrix(cleaf, i2, 0);

	/* query the copy at time zero first: must match exactly */
	TASK_CHECK(mat_near(w1, w2));
	TASK_CHECK(mat_near(i1, i2));

	/* world-inverse must be consistent */
	skin_pt(w1, i1, 1, 2, 3, v1);
	skin_pt(w2, i2, 1, 2, 3, v2);
	TASK_CHECK(NEAR(v1[0], v2[0]) && NEAR(v1[1], v2[1]) && NEAR(v1[2], v2[2]));

	anm_free_node_tree(root);
	anm_free_node_tree(copy);
	return 0;
}

/* query the copy's inverse matrix first (cold cache), then compare */
static int case_playback_inverse_first(void)
{
	struct anm_node *root, *mid, *leaf, *branch;
	struct anm_node *copy, *cleaf;
	float inv2[16], w2[16], inv1[16], w1[16];
	float v1[3], v2[3];

	root = build_fork(&mid, &leaf, &branch);
	copy = anm_clone_node_tree(root);
	TASK_CHECK(copy);
	cleaf = find_child(find_child(copy, "mid"), "leaf");
	TASK_CHECK(cleaf);

	anm_get_inv_matrix(cleaf, inv2, 0);	/* inverse first, cold cache */
	anm_get_inv_matrix(leaf, inv1, 0);
	TASK_CHECK(mat_near(inv1, inv2));

	anm_get_matrix(cleaf, w2, 0);
	anm_get_matrix(leaf, w1, 0);
	TASK_CHECK(mat_near(w1, w2));

	skin_pt(w2, inv2, 1, 2, 3, v2);
	skin_pt(w1, inv1, 1, 2, 3, v1);
	TASK_CHECK(NEAR(v1[0], v2[0]) && NEAR(v1[1], v2[1]) && NEAR(v1[2], v2[2]));

	anm_free_node_tree(root);
	anm_free_node_tree(copy);
	return 0;
}

/* independent playback: different times, editing one side, source freed */
static int case_playback_independent(void)
{
	struct anm_node *root, *mid, *leaf, *branch;
	struct anm_node *copy, *cmid, *cleaf, *cbranch;
	float wa[16], wb[16];
	float va[3], vb[3];

	root = build_fork(&mid, &leaf, &branch);

	/* animate root over time so sampling differs between times */
	anm_set_position3f(root, 1, 0, 0, 0);
	anm_set_position3f(root, 5, 0, 0, 1000);

	copy = anm_clone_node_tree(root);
	TASK_CHECK(copy);
	cmid = find_child(copy, "mid");
	cleaf = find_child(cmid, "leaf");
	cbranch = find_child(copy, "branch");
	TASK_CHECK(cmid && cleaf && cbranch);

	/* source plays at t=250, copy at t=750 */
	anm_get_matrix(leaf, wa, 250);
	anm_get_matrix(cleaf, wb, 750);
	TASK_CHECK(!mat_near(wa, wb));

	/* edit the copy's root track (detaches copy only) and sample again */
	anm_set_position3f(copy, 100, 0, 0, 750);
	TASK_CHECK(ctrack(root, 0, ANM_TRACK_POS_X)->keys !=
			ctrack(copy, 0, ANM_TRACK_POS_X)->keys);
	anm_get_matrix(cleaf, wb, 750);
	/* source remains unaffected */
	anm_get_matrix(leaf, wa, 750);
	TASK_CHECK(NEAR(wa[12], 4.0f));	/* 1 + (5-1)*0.75 */

	/* destroy the source, then sample the copy: it must stay valid */
	anm_free_node_tree(root);
	{
		float wc[16], ic[16], v[3];
		anm_get_matrix(cleaf, wc, 500);
		anm_get_inv_matrix(cleaf, ic, 500);
		skin_pt(wc, ic, 1, 2, 3, v);
		TASK_CHECK(isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]));
		/* copy's edited root still holds its own key buffer */
		TASK_CHECK(NEAR(anm_get_value(ctrack(copy, 0, ANM_TRACK_POS_X), 750), 100.0f));
	}

	anm_free_node_tree(copy);
	return 0;
}

/* in-progress transition survives the snapshot and blends identically */
static int case_playback_transition(void)
{
	struct anm_node *src, *copy;
	float m1[16], m2[16];
	float q1[4], q2[4];

	src = anm_create_node();
	TASK_CHECK(src);
	anm_use_node_animation(src, 0);
	anm_set_position3f(src, 0, 0, 0, 0);
	anm_set_position3f(src, 0, 0, 0, 1000);

	TASK_CHECK(anm_add_node_animation(src) == 0);
	anm_use_node_animation(src, 1);
	anm_set_position3f(src, 10, 0, 0, 0);
	anm_set_position3f(src, 10, 0, 0, 1000);

	/* start a transition into slot 1 at t=1000 lasting 1000ms, then snap */
	anm_use_node_animation(src, 0);
	anm_node_transition(src, 1, 1000, 1000);

	copy = anm_clone_node_tree(src);
	TASK_CHECK(copy);

	/* scalar transition state copied verbatim */
	TASK_CHECK(copy->cur_anim[0] == src->cur_anim[0]);
	TASK_CHECK(copy->cur_anim[1] == src->cur_anim[1]);
	TASK_CHECK(copy->cur_anim_offset[0] == src->cur_anim_offset[0]);
	TASK_CHECK(copy->cur_anim_offset[1] == src->cur_anim_offset[1]);
	TASK_CHECK(copy->blend_dur == src->blend_dur);

	/* sample mid-transition on both: same blended pose */
	anm_get_matrix(src, m1, 1500);
	anm_get_matrix(copy, m2, 1500);
	TASK_CHECK(mat_near(m1, m2));
	/* transition started at 1000 with dur 1000, so t=1500 is halfway to 10 */
	TASK_CHECK(NEAR(m1[12], 5.0f));

	/* rotation quaternions blend identically on both sides */
	anm_get_node_rotation(src, q1, 1500);
	anm_get_node_rotation(copy, q2, 1500);
	TASK_CHECK(NEAR(q1[0], q2[0]) && NEAR(q1[1], q2[1]) &&
			NEAR(q1[2], q2[2]) && NEAR(q1[3], q2[3]));

	anm_free_node_tree(src);
	anm_free_node_tree(copy);
	return 0;
}

/* the snapshot keeps the source's user data pointer by value */
static int case_playback_data_borrowed(void)
{
	struct anm_node *src, *copy;
	int marker = 7;

	src = anm_create_node();
	TASK_CHECK(src);
	anm_use_node_animation(src, 0);
	anm_set_position3f(src, 2, 0, 0, 0);
	src->data = &marker;

	copy = anm_clone_node_tree(src);
	TASK_CHECK(copy);
	TASK_CHECK(copy->data == &marker);

	/* freeing either side must not free the borrowed data */
	copy->data = NULL;
	anm_free_node_tree(src);
	anm_free_node_tree(copy);
	TASK_CHECK(marker == 7);
	return 0;
}

/* ------------------------------------------------------------------ */
/* group: lifetime                                                     */
/* ------------------------------------------------------------------ */

/* two-level snapshot: destroy in every order, buffers outlive holders */
static int case_lifetime_nested(void)
{
	struct anm_node *src, *mid, *leaf, *branch;
	struct anm_node *a, *b;
	float wa[16], wb[16];

	src = build_fork(&mid, &leaf, &branch);
	anm_set_position3f(src, 3, 0, 0, 0);

	a = anm_clone_node_tree(src);		/* snapshot of source */
	TASK_CHECK(a);
	b = anm_clone_node_tree(a);			/* snapshot of the snapshot */
	TASK_CHECK(b);

	/* all three share the key buffers */
	TASK_CHECK(ctrack(a, 0, ANM_TRACK_POS_X)->keys ==
			ctrack(src, 0, ANM_TRACK_POS_X)->keys);
	TASK_CHECK(ctrack(b, 0, ANM_TRACK_POS_X)->keys ==
			ctrack(src, 0, ANM_TRACK_POS_X)->keys);

	/* destroy the middle holder first; the outer trees keep working */
	anm_free_node_tree(src);
	anm_get_matrix(find_child(find_child(a, "mid"), "leaf"), wa, 0);
	anm_get_matrix(find_child(find_child(b, "mid"), "leaf"), wb, 0);
	TASK_CHECK(mat_near(wa, wb));

	/* now destroy a; b must still sample from the shared buffer */
	anm_free_node_tree(a);
	anm_get_matrix(find_child(find_child(b, "mid"), "leaf"), wb, 0);
	TASK_CHECK(NEAR(wb[12], 3.0f));

	/* finally release the last holder */
	anm_free_node_tree(b);
	return 0;
}

/* editing a deeper snapshot must detach only that track, not the outer holders */
static int case_lifetime_detach_order(void)
{
	struct anm_node *src, *a, *b;
	float wb[16];

	src = anm_create_node();
	TASK_CHECK(src);
	anm_use_node_animation(src, 0);
	anm_set_position3f(src, 1, 0, 0, 0);

	a = anm_clone_node_tree(src);
	b = anm_clone_node_tree(a);
	TASK_CHECK(a && b);

	/* write through b: detaches b, leaves src and a sharing */
	anm_set_position3f(b, 9, 0, 0, 0);
	TASK_CHECK(ctrack(b, 0, ANM_TRACK_POS_X)->keys !=
			ctrack(a, 0, ANM_TRACK_POS_X)->keys);
	TASK_CHECK(ctrack(src, 0, ANM_TRACK_POS_X)->keys ==
			ctrack(a, 0, ANM_TRACK_POS_X)->keys);

	/* free src first, a still holds the original buffer */
	anm_free_node_tree(src);
	anm_get_matrix(a, wb, 0);
	TASK_CHECK(NEAR(wb[12], 1.0f));

	/* b reflects its own edit */
	anm_get_matrix(b, wb, 0);
	TASK_CHECK(NEAR(wb[12], 9.0f));

	anm_free_node_tree(a);
	anm_free_node_tree(b);
	return 0;
}

/* NULL input yields NULL; siblings of the root are excluded */
static int case_lifetime_null_and_siblings(void)
{
	struct anm_node *parent, *root, *sib, *copy;

	TASK_CHECK(anm_clone_node_tree(NULL) == NULL);

	parent = anm_create_node();
	root = anm_create_node();
	sib = anm_create_node();
	TASK_CHECK(parent && root && sib);

	anm_link_node(parent, root);
	anm_link_node(parent, sib);
	anm_set_node_name(root, "root");
	anm_set_node_name(sib, "sibling");
	anm_use_node_animation(root, 0);
	anm_set_position3f(root, 1, 0, 0, 0);

	copy = anm_clone_node_tree(root);
	TASK_CHECK(copy);
	TASK_CHECK(copy->parent == NULL);
	TASK_CHECK(copy->next == NULL);
	TASK_CHECK(strcmp(copy->name, "root") == 0);
	TASK_CHECK(copy->child == NULL);	/* no descendants on root */

	anm_free_node_tree(copy);
	anm_free_node_tree(parent);
	return 0;
}

/* the root's own children keep their order across the snapshot */
static int case_lifetime_child_order(void)
{
	struct anm_node *parent, *a, *b, *c, *copy, *it;

	parent = anm_create_node();
	a = anm_create_node();
	b = anm_create_node();
	c = anm_create_node();
	TASK_CHECK(parent && a && b && c);

	/* link_node pushes to the head; this yields child order c, b, a */
	anm_link_node(parent, a);
	anm_link_node(parent, b);
	anm_link_node(parent, c);
	anm_set_node_name(a, "a");
	anm_set_node_name(b, "b");
	anm_set_node_name(c, "c");

	copy = anm_clone_node_tree(parent);
	TASK_CHECK(copy);

	it = copy->child;
	TASK_CHECK(it && it->parent == copy && strcmp(it->name, "c") == 0);
	it = it->next;
	TASK_CHECK(it && strcmp(it->name, "b") == 0);
	it = it->next;
	TASK_CHECK(it && strcmp(it->name, "a") == 0);
	TASK_CHECK(it->next == NULL);

	anm_free_node_tree(parent);
	anm_free_node_tree(copy);
	return 0;
}

/* monotonically increasing transition timing convention is unchanged */
static int case_lifetime_transition_timing(void)
{
	struct anm_node *src, *copy;
	anm_time_t off;

	src = anm_create_node();
	TASK_CHECK(src);
	anm_use_node_animation(src, 0);
	anm_set_position3f(src, 0, 0, 0, 0);
	anm_set_position3f(src, 0, 0, 0, 1000);
	TASK_CHECK(anm_add_node_animation(src) == 0);
	anm_use_node_animation(src, 1);
	anm_set_position3f(src, 8, 0, 0, 0);

	anm_use_node_animation(src, 0);
	anm_node_transition(src, 1, 2000, 500);

	copy = anm_clone_node_tree(src);
	TASK_CHECK(copy);
	TASK_CHECK(copy->cur_anim_offset[1] == 2000);
	TASK_CHECK(copy->blend_dur == 500);
	off = anm_get_animation_offset(copy, 1);
	TASK_CHECK(off == 2000);

	/* before the transition starts, mix clamps to 0 */
	{
		float p[3];
		anm_get_node_position(copy, p, 1000);
		TASK_CHECK(NEAR(p[0], 0.0f));
	}
	/* after it completes, the copy switches to the target animation */
	{
		float p[3];
		anm_get_node_position(copy, p, 2600);
		TASK_CHECK(NEAR(p[0], 8.0f));
	}

	anm_free_node_tree(src);
	anm_free_node_tree(copy);
	return 0;
}


/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	{"sharing",  "share_address",      case_share_address},
	{"sharing",  "share_empty",        case_share_empty},
	{"sharing",  "copy_is_deep",       case_copy_is_deep},
	{"sharing",  "share_multi_anim",   case_share_multi_anim},
	{"sharing",  "share_with_clone",   case_share_with_clone},

	{"playback", "world_inverse",      case_playback_world_inverse},
	{"playback", "inverse_first",      case_playback_inverse_first},
	{"playback", "independent",        case_playback_independent},
	{"playback", "transition",         case_playback_transition},
	{"playback", "data_borrowed",      case_playback_data_borrowed},

	{"lifetime", "nested",             case_lifetime_nested},
	{"lifetime", "detach_order",       case_lifetime_detach_order},
	{"lifetime", "null_and_siblings",  case_lifetime_null_and_siblings},
	{"lifetime", "child_order",        case_lifetime_child_order},
	{"lifetime", "transition_timing",  case_lifetime_transition_timing},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
