/* c-ray-fast - octree spatial index, build/teardown and ray queries.
 * Copyright (C) 2006-2021 John Tsiombikas <nuclear@member.fsf.org>
 *
 * You are free to use, modify and redistribute this program under the terms
 * of the GNU General Public License v3 or later. See COPYING for details.
 * ---------------------------------------------------------------------
 * Spheres enter the octree through their own bounds; a fused ball field
 * group enters through the conservative union of all its supporting spheres.
 * The same group may be referenced by several leaves (or repeatedly by one
 * leaf), so every query de-duplicates group references before intersecting:
 * each group is mixed exactly once per ray, and its full local contribution
 * is evaluated by the single production root finder in metaball.c. Nearest
 * and occlusion queries share that intersection entry point.
 */
#include <stdlib.h>
#include <stdio.h>
#include <float.h>

#include "scene.h"
#include "metaball.h"

#define SEEN_STACK	64		/* larger scenes fall back to a heap buffer */

void free_octree(struct octnode *node)
{
	int i;

	if(!node) return;
	for(i=0; i<8; i++) {
		if(node->sub[i]) {
			free_octree(node->sub[i]);
			node->sub[i] = 0;
		}
	}
	free(node->obj);
	node->obj = 0;
	free(node);
}

/* conservative overlap test between a node box and a group's support union */
int box_metaball(struct aabox *box, struct metaball *mb)
{
	if(mb->bbox.vmax.x < box->vmin.x || mb->bbox.vmin.x > box->vmax.x) return 0;
	if(mb->bbox.vmax.y < box->vmin.y || mb->bbox.vmin.y > box->vmax.y) return 0;
	if(mb->bbox.vmax.z < box->vmin.z || mb->bbox.vmin.z > box->vmax.z) return 0;
	return 1;
}

int populate_octnode(struct octnode *node, struct object **objlist, int num)
{
	int i;
	struct object *obj;

	for(i=0; i<num; i++) {
		obj = *objlist++;

		if(obj->type == OBJ_SPHERE) {
			if(box_sphere(&node->aabb, &obj->o.sph)) {
				node->obj[node->num_obj++] = obj;
			}
		} else if(obj->type == OBJ_METABALL) {
			if(box_metaball(&node->aabb, obj->o.mb)) {
				node->obj[node->num_obj++] = obj;
			}
		}
	}
	return node->num_obj;
}

int build_octree(struct octnode *node, int depth)
{
	int i;
	struct octnode *sub;
	double hx, hy, hz;

	if(node->num_obj < OCT_MAX_OBJ || depth >= OCT_MAX_DEPTH) {
		return 0;
	}

	hx = (node->aabb.vmin.x + node->aabb.vmax.x) * 0.5;
	hy = (node->aabb.vmin.y + node->aabb.vmax.y) * 0.5;
	hz = (node->aabb.vmin.z + node->aabb.vmax.z) * 0.5;

	for(i=0; i<8; i++) {
		if(!(sub = malloc(sizeof *sub)) || !(sub->obj = malloc(node->num_obj * sizeof *sub->obj))) {
			perror("failed to allocate octree node");
			free(sub);
			return -1;
		}
		sub->num_obj = 0;
		sub->sub[0] = sub->sub[1] = sub->sub[2] = sub->sub[3] = 0;
		sub->sub[4] = sub->sub[5] = sub->sub[6] = sub->sub[7] = 0;

		sub->aabb.vmin.x = LERP(node->aabb.vmin.x, hx, i & 1);
		sub->aabb.vmax.x = LERP(hx, node->aabb.vmax.x, i & 1);
		sub->aabb.vmin.y = LERP(node->aabb.vmin.y, hy, (i >> 1) & 1);
		sub->aabb.vmax.y = LERP(hy, node->aabb.vmax.y, (i >> 1) & 1);
		sub->aabb.vmin.z = LERP(node->aabb.vmin.z, hz, (i >> 2) & 1);
		sub->aabb.vmax.z = LERP(hz, node->aabb.vmax.z, (i >> 2) & 1);

		if(!populate_octnode(sub, node->obj, node->num_obj)) {
			/* no objects intersect this node's bbox, drop it */
			free(sub->obj);
			free(sub);
			sub = 0;
		} else {
			if(build_octree(sub, depth + 1) == -1) {
				return -1;
			}
		}
		node->sub[i] = sub;
	}

	free(node->obj);
	node->obj = 0;
	node->num_obj = 0;
	return 0;
}

/* Rebuild the spatial index for the currently loaded scene. Any previous
 * octree is discarded first, so component edits made between preparations
 * (centers, radii, weights) take effect, including the conservative bounds.
 * Returns 0 on success and -1 on failure. */
int prepare_scene(void)
{
	int i;
	struct object *obj;
	struct aabox box;
	int have;

	if(octroot) {
		free_octree(octroot);
		octroot = 0;
	}

	if(!(octroot = malloc(sizeof *octroot))) {
		perror("failed to allocate octree root node");
		return -1;
	}
	octroot->obj = 0;
	octroot->num_obj = octroot->max_obj = 0;
	for(i=0; i<8; i++) octroot->sub[i] = 0;

	box.vmin.x = box.vmin.y = box.vmin.z = DBL_MAX;
	box.vmax.x = box.vmax.y = box.vmax.z = -DBL_MAX;
	have = 0;

	if(num_obj > 0) {
		if(!(octroot->obj = malloc(num_obj * sizeof *octroot->obj))) {
			perror("failed to allocate octree root object list");
			free_octree(octroot);
			octroot = 0;
			return -1;
		}

		for(i=0; i<num_obj; i++) {
			obj = objects + i;

			if(obj->type == OBJ_SPHERE) {
				struct sphere *sph = &obj->o.sph;
				double d;

				d = sph->pos.x - sph->rad;
				if(d < box.vmin.x) box.vmin.x = d;
				d = sph->pos.x + sph->rad;
				if(d > box.vmax.x) box.vmax.x = d;
				d = sph->pos.y - sph->rad;
				if(d < box.vmin.y) box.vmin.y = d;
				d = sph->pos.y + sph->rad;
				if(d > box.vmax.y) box.vmax.y = d;
				d = sph->pos.z - sph->rad;
				if(d < box.vmin.z) box.vmin.z = d;
				d = sph->pos.z + sph->rad;
				if(d > box.vmax.z) box.vmax.z = d;

				octroot->obj[octroot->num_obj++] = obj;
				have = 1;
			} else if(obj->type == OBJ_METABALL) {
				struct metaball *mb = obj->o.mb;
				struct aabox mbbox;

				metaball_bounds(mb, &mbbox);
				mb->bbox = mbbox;

				if(mb->num_comp > 0) {
					if(mbbox.vmin.x < box.vmin.x) box.vmin.x = mbbox.vmin.x;
					if(mbbox.vmax.x > box.vmax.x) box.vmax.x = mbbox.vmax.x;
					if(mbbox.vmin.y < box.vmin.y) box.vmin.y = mbbox.vmin.y;
					if(mbbox.vmax.y > box.vmax.y) box.vmax.y = mbbox.vmax.y;
					if(mbbox.vmin.z < box.vmin.z) box.vmin.z = mbbox.vmin.z;
					if(mbbox.vmax.z > box.vmax.z) box.vmax.z = mbbox.vmax.z;

					octroot->obj[octroot->num_obj++] = obj;
					have = 1;
				}
			}
		}
	}

	if(!have) {
		/* no finite objects: an empty degenerate box rejects every ray */
		box.vmin.x = box.vmin.y = box.vmin.z = 0.0;
		box.vmax.x = box.vmax.y = box.vmax.z = 0.0;
	}
	octroot->aabb = box;

	if(build_octree(octroot, 0) == -1) {
		free_octree(octroot);
		octroot = 0;
		return -1;
	}
	return 0;
}

static int group_seen(struct metaball **seen, int nseen, struct metaball *mb)
{
	int i;
	for(i=0; i<nseen; i++) {
		if(seen[i] == mb) return 1;
	}
	return 0;
}

/* Recursive query. The seen list is shared by the whole traversal so a group
 * referenced by several nodes (or repeated in a leaf) intersects exactly once. */
static int isect_rec(struct octnode *node, struct ray *ray, struct spoint *spret,
		struct metaball **seen, int *nseen, struct spoint *best)
{
	int i;

	if(!ray_aabox(&node->aabb, ray)) return 0;

	if(!spret) {
		/* occlusion: any intersection is enough */
		if(node->obj) {
			for(i=0; i<node->num_obj; i++) {
				struct object *obj = node->obj[i];

				if(obj->type == OBJ_SPHERE) {
					if(ray_object(obj, ray, 0)) return 1;
				} else if(obj->type == OBJ_METABALL) {
					struct metaball *mb = obj->o.mb;
					if(group_seen(seen, *nseen, mb)) continue;
					seen[(*nseen)++] = mb;
					if(ray_metaball(mb, ray, 0)) return 1;
				}
			}
			return 0;
		}

		for(i=0; i<8; i++) {
			if(node->sub[i] && isect_rec(node->sub[i], ray, 0, seen, nseen, best)) {
				return 1;
			}
		}
		return 0;
	}

	/* nearest intersection */
	if(node->obj) {
		struct spoint sp;

		for(i=0; i<node->num_obj; i++) {
			struct object *obj = node->obj[i];

			if(obj->type == OBJ_SPHERE) {
				if(ray_object(obj, ray, &sp) && sp.dist < best->dist) {
					sp.obj = obj;
					*best = sp;
				}
			} else if(obj->type == OBJ_METABALL) {
				struct metaball *mb = obj->o.mb;
				if(group_seen(seen, *nseen, mb)) continue;
				seen[(*nseen)++] = mb;
				if(ray_metaball(mb, ray, &sp) && sp.dist < best->dist) {
					sp.obj = obj;
					*best = sp;
				}
			}
		}
	} else {
		for(i=0; i<8; i++) {
			if(node->sub[i]) {
				isect_rec(node->sub[i], ray, spret, seen, nseen, best);
			}
		}
	}
	return best->obj != 0;
}

int isect_octnode(struct octnode *node, struct ray *ray, struct spoint *spret)
{
	struct metaball *seen_stack[SEEN_STACK];
	struct metaball **seen;
	int nseen = 0, ret;
	struct spoint best;

	seen = seen_stack;
	if(num_mb > SEEN_STACK) {
		if(!(seen = malloc(num_mb * sizeof *seen))) {
			return 0;
		}
	}

	best.dist = DBL_MAX;
	best.obj = 0;

	ret = isect_rec(node, ray, spret, seen, &nseen, &best);
	if(ret && spret) *spret = best;

	if(seen != seen_stack) free(seen);
	return ret;
}
