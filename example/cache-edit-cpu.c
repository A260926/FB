/* cache-edit-cpu.c - CPU-only demonstration of the track-edit -> matrix
 * cache invalidation chain.
 *
 * Builds a parent->child bind chain containing both rotation and
 * translation, skins a fixed bind vertex with (current world * inverse
 * bind), and asserts the resulting coordinates before and after editing
 * the child's rotation track through the public track API.
 *
 * Prints CACHE_EDIT_DEMO_OK on success.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "anim.h"

#define PI	3.14159265358979323846
#define TOL	1e-4f

/* independent 4x4 helpers (column-major, OpenGL order) */

static void mmul(float *r, const float *a, const float *b)
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

static void mxform(float *out, const float *m, const float *v)
{
	int i;
	for(i = 0; i < 3; i++) {
		out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i];
	}
}

static int vnear(const float *a, const float *b)
{
	int i;
	for(i = 0; i < 3; i++) {
		if(fabsf(a[i] - b[i]) > TOL) return 0;
	}
	return 1;
}

static int mnear(const float *a, const float *b)
{
	int i;
	for(i = 0; i < 16; i++) {
		if(fabsf(a[i] - b[i]) > TOL) return 0;
	}
	return 1;
}

int main(void)
{
	static const float bind_vtx[3] = {1, 2, 0};
	static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	struct anm_node *parent, *child;
	struct anm_animation *anim;
	float invbind[16], world[16], inv[16], skin[16], prod[16];
	float p[3], expect[3];

	parent = anm_create_node();
	child = anm_create_node();
	if(!parent || !child) {
		fprintf(stderr, "failed to create nodes\n");
		return 1;
	}
	anm_link_node(parent, child);

	/* bind pose: translation on both levels, 90deg Z rotation on the child */
	anm_set_position3f(parent, 10, 0, 0, 0);
	anm_set_position3f(child, 0, 5, 0, 0);
	anm_set_rotation_axis(child, (float)(PI / 2), 0, 0, 1, 0);

	/* inverse-bind matrix of the child at the bind time */
	anm_get_inv_matrix(child, invbind, 0);

	/* skin the fixed bind vertex with the current pose; at the bind time
	 * this must reproduce the bind vertex itself */
	anm_get_matrix(child, world, 0);
	mmul(skin, world, invbind);
	mxform(p, skin, bind_vtx);
	expect[0] = 1;
	expect[1] = 2;
	expect[2] = 0;
	if(!vnear(p, expect)) {
		fprintf(stderr, "bind-pose skin mismatch: got (%g, %g, %g)\n", p[0], p[1], p[2]);
		return 1;
	}
	printf("bind pose:  vertex (%g, %g, %g) -> (%g, %g, %g)\n",
			bind_vtx[0], bind_vtx[1], bind_vtx[2], p[0], p[1], p[2]);

	/* low level track edit through the public API: change the child's
	 * rotation key at t=0 from 90deg to 180deg about Z
	 */
	anim = anm_get_animation(child, 0);
	if(!anim) {
		fprintf(stderr, "failed to get child animation\n");
		return 1;
	}
	if(anm_set_value(anim->tracks + ANM_TRACK_ROT_Z, 0, 1.0f) == -1 ||
			anm_set_value(anim->tracks + ANM_TRACK_ROT_W, 0, 0.0f) == -1) {
		fprintf(stderr, "failed to edit rotation track\n");
		return 1;
	}

	/* the cached world matrix must reflect the edited track immediately */
	anm_get_matrix(child, world, 0);
	mmul(skin, world, invbind);
	mxform(p, skin, bind_vtx);
	expect[0] = 13;
	expect[1] = -4;
	expect[2] = 0;
	if(!vnear(p, expect)) {
		fprintf(stderr, "post-edit skin mismatch: got (%g, %g, %g), expected (%g, %g, %g)\n",
				p[0], p[1], p[2], expect[0], expect[1], expect[2]);
		return 1;
	}
	printf("post edit:  vertex (%g, %g, %g) -> (%g, %g, %g)\n",
			bind_vtx[0], bind_vtx[1], bind_vtx[2], p[0], p[1], p[2]);

	/* the cached inverse must agree: world * inverse == identity */
	anm_get_inv_matrix(child, inv, 0);
	mmul(prod, world, inv);
	if(!mnear(prod, ident)) {
		fprintf(stderr, "world * inverse is not identity after the edit\n");
		return 1;
	}

	anm_free_node_tree(parent);

	printf("CACHE_EDIT_DEMO_OK\n");
	return 0;
}
