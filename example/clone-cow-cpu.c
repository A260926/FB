/*
 * clone-cow-cpu - shared-curve character snapshot demo (headless).
 *
 * Builds a small two-bone character with keyframed motion, takes a snapshot
 * with anm_clone_node_tree (which shares the keyframe buffers), plays the two
 * copies at different times, edits one copy's track (copy-on-write detaches
 * only that track), destroys the source, and then skins a bind-space vertex
 * through the surviving copy to prove it still animates independently.
 *
 * Only C99, the C standard library, the bundled cgmath, and pthread are used.
 * No GLUT/OpenGL is required. Prints CLONE_COW_DEMO_OK on success.
 */
#include <stdio.h>
#include <math.h>

#include "anim.h"

#define NEAR(a, b)	(fabsf((a) - (b)) < 1e-3f)

/* apply a row-major 4x4 matrix to a point (x, y, z, 1) */
static void xform(const float *m, float x, float y, float z, float *res)
{
	res[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
	res[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
	res[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

/* world * inverse-bind applied to a bind-space vertex */
static void skin(const float *world, const float *invbind,
		float x, float y, float z, float *res)
{
	float full[16];
	int i, j;
	for(i=0; i<4; i++) {
		for(j=0; j<4; j++) {
			full[i * 4 + j] = world[i * 4 + 0] * invbind[0 + j] +
				world[i * 4 + 1] * invbind[4 + j] +
				world[i * 4 + 2] * invbind[8 + j] +
				world[i * 4 + 3] * invbind[12 + j];
		}
	}
	xform(full, x, y, z, res);
}

/* build a two-bone arm: root -> elbow, pivots cancel for identity rotation,
 * so the elbow world position at t is (root_x(t), 2, 0) */
static struct anm_node *build_character(struct anm_node **elbow_out)
{
	struct anm_node *root = anm_create_node();
	struct anm_node *elbow = anm_create_node();

	if(!root || !elbow) {
		if(root) anm_free_node_tree(root);
		if(elbow) anm_free_node_tree(elbow);
		return NULL;
	}
	anm_set_node_name(root, "shoulder");
	anm_set_node_name(elbow, "elbow");
	anm_set_pivot(root, 0.5f, 0.0f, 0.0f);
	anm_set_pivot(elbow, 0.0f, -1.0f, 0.0f);
	anm_link_node(root, elbow);

	anm_use_node_animation(root, 0);
	anm_use_node_animation(elbow, 0);

	/* root slides along x over 2 seconds */
	anm_set_position3f(root, 0, 0, 0, 0);
	anm_set_position3f(root, 4, 0, 0, 2000);

	/* elbow has a fixed local offset */
	anm_set_position3f(elbow, 0, 2, 0, 0);

	*elbow_out = elbow;
	return root;
}

int main(void)
{
	struct anm_node *src, *elbow, *copy, *celbow;
	float bind_inv[16];
	float wsrc[16], wcopy[16], icopy[16];
	float vsrc[3], vcopy[3];
	int ok = 1;

	if(!(src = build_character(&elbow))) {
		return 1;
	}

	/* capture the bind-pose inverse for the elbow at rest (t = 0) */
	anm_get_inv_matrix(elbow, bind_inv, 0);

	/* the two characters play at different times before the snapshot */
	anm_get_matrix(elbow, wsrc, 500);
	if(!NEAR(wsrc[12], 1.0f)) {	/* 4 * 500/2000 = 1 */
		ok = 0;
	}

	/* snapshot: shares keyframe buffers with the source */
	if(!(copy = anm_clone_node_tree(src))) {
		anm_free_node_tree(src);
		return 2;
	}
	celbow = copy->child;	/* same structure: root -> elbow */

	/* the copy samples the shared curves at a different time */
	anm_get_matrix(celbow, wcopy, 1500);
	if(!NEAR(wcopy[12], 3.0f)) {	/* 4 * 1500/2000 = 3 */
		ok = 0;
	}

	/* edit the copy's root track: only the copy's buffer detaches */
	anm_use_node_animation(copy, 0);
	anm_set_position3f(copy, 7, 0, 0, 1500);

	/* destroy the source; the copy must still sample on its own */
	anm_free_node_tree(src);

	anm_get_matrix(celbow, wcopy, 1500);
	anm_get_inv_matrix(celbow, icopy, 1500);
	if(!NEAR(wcopy[12], 7.0f)) {
		ok = 0;
	}

	/* skin a bind-space vertex through world * inverse-bind */
	skin(wcopy, bind_inv, 1, 0, 0, vcopy);
	/* bind pose: elbow world = (0,2,0), inverse = (0,-2,0)
	 * animated: elbow world = (7,2,0), so vertex (1,0,0) -> (8,0,0) */
	if(!NEAR(vcopy[0], 8.0f) || !NEAR(vcopy[1], 0.0f) || !NEAR(vcopy[2], 0.0f)) {
		ok = 0;
	}

	/* the source's pre-free pose (world x = 1) is remembered in wsrc */
	xform(wsrc, 1, 0, 0, vsrc);
	if(!NEAR(vsrc[0], 2.0f)) {	/* vertex x = elbow world x + 1 = 2 */
		ok = 0;
	}
	if(NEAR(vcopy[0], vsrc[0])) {	/* the two copies must differ now */
		ok = 0;
	}

	if(!ok) {
		fprintf(stderr, "clone-cow demo failed\n");
		anm_free_node_tree(copy);
		return 3;
	}

	printf("copy world=(%.2f, %.2f) skinned=(%.2f, %.2f, %.2f)\n",
			wcopy[12], wcopy[13], vcopy[0], vcopy[1], vcopy[2]);
	printf("CLONE_COW_DEMO_OK\n");

	anm_free_node_tree(copy);
	return 0;
}
