/*
libanim - hierarchical keyframe animation library
Copyright (C) 2012-2018 John Tsiombikas <nuclear@member.fsf.org>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include "anim.h"
#include "dynarr.h"

#include "cgmath/cgmath.h"

#define ROT_USE_SLERP

static void invalidate_cache(struct anm_node *node);

/* destroy all animation slots currently stored in the array, then free it */
static void clear_animations(struct anm_node *node)
{
	int i, num_anim;
	if(!node->animations) return;
	num_anim = anm_dynarr_size(node->animations);
	for(i=0; i<num_anim; i++) {
		anm_destroy_animation(node->animations + i);
	}
	anm_dynarr_free(node->animations);
	node->animations = 0;
}

/* deep-copies the node's animation array, sharing track key buffers.
 * dest must already have a valid (possibly empty) animations array.
 * returns 0 on success, -1 on allocation failure, leaving dest untouched. */
static int clone_animations(struct anm_node *dest, const struct anm_node *src)
{
	int i, j, num_anim, initialized = 0;
	void *newanim;

	num_anim = anm_get_animation_count(src);

	if(!(newanim = anm_dynarr_alloc(num_anim, sizeof(struct anm_animation)))) {
		return -1;
	}

	for(i=0; i<num_anim; i++) {
		struct anm_animation *anim = (struct anm_animation*)newanim + i;
		int ok = 1;

		if(anm_init_animation(anim) == -1) {
			ok = 0;
		}
		for(j=0; j<ANM_NUM_TRACKS && ok; j++) {
			struct anm_track *st = src->animations[i].tracks + j;
			/* copies name and scalar settings, shares the key buffer */
			if(anm_share_track(anim->tracks + j, st) == -1) {
				ok = 0;
			}
		}
		if(!ok) {
			anm_destroy_animation(anim);
			for(j=0; j<initialized; j++) {
				anm_destroy_animation((struct anm_animation*)newanim + j);
			}
			anm_dynarr_free(newanim);
			return -1;
		}
		initialized++;
	}

	/* copy animation names (track names were handled by anm_share_track) */
	for(i=0; i<num_anim; i++) {
		struct anm_animation *anim = (struct anm_animation*)newanim + i;
		if(src->animations[i].name) {
			if(!(anim->name = malloc(strlen(src->animations[i].name) + 1))) {
				for(j=0; j<num_anim; j++) {
					anm_destroy_animation((struct anm_animation*)newanim + j);
				}
				anm_dynarr_free(newanim);
				return -1;
			}
			strcpy(anim->name, src->animations[i].name);
		}
	}

	clear_animations(dest);
	dest->animations = newanim;
	return 0;
}

/* recursively clones a subtree; parent/next of the new root are cleared.
 * On failure returns 0, freeing the partially built subtree. */
static struct anm_node *clone_subtree(const struct anm_node *src)
{
	struct anm_node *node, *child, *prev = 0;

	if(!(node = malloc(sizeof *node))) {
		return 0;
	}
	/* start from a clean, cold-cache node, then overwrite the scalar state */
	if(anm_init_node(node) == -1) {
		free(node);
		return 0;
	}

	if(src->name) {
		if(!(node->name = malloc(strlen(src->name) + 1))) {
			anm_free_node(node);
			return 0;
		}
		strcpy(node->name, src->name);
	}

	if(clone_animations(node, src) == -1) {
		anm_free_node(node);
		return 0;
	}

	/* scalar state; matrices start zeroed by anm_init_node (cold cache) */
	node->cur_anim[0] = src->cur_anim[0];
	node->cur_anim[1] = src->cur_anim[1];
	node->cur_anim_offset[0] = src->cur_anim_offset[0];
	node->cur_anim_offset[1] = src->cur_anim_offset[1];
	node->cur_mix = src->cur_mix;
	node->blend_dur = src->blend_dur;
	node->pivot[0] = src->pivot[0];
	node->pivot[1] = src->pivot[1];
	node->pivot[2] = src->pivot[2];
	node->data = src->data;

	node->parent = 0;
	node->next = 0;
	node->child = 0;

	/* clone children, preserving order by appending to the tail */
	for(child = src->child; child; child = child->next) {
		struct anm_node *cclone = clone_subtree(child);
		if(!cclone) {
			anm_free_node_tree(node);
			return 0;
		}
		cclone->parent = node;
		if(prev) {
			prev->next = cclone;
		} else {
			node->child = cclone;
		}
		prev = cclone;
	}

	return node;
}

struct anm_node *anm_clone_node_tree(const struct anm_node *src)
{
	if(!src) {
		return 0;
	}
	return clone_subtree(src);
}

int anm_init_animation(struct anm_animation *anim)
{
	int i, j;
	static const float defaults[] = {
		0.0f, 0.0f, 0.0f,		/* default position */
		0.0f, 0.0f, 0.0f, 1.0f,	/* default rotation quat */
		1.0f, 1.0f, 1.0f		/* default scale factor */
	};

	/* zero the whole struct first, so a failure part-way through leaves the
	 * remaining tracks in a safe (empty) state for anm_destroy_animation */
	memset(anim, 0, sizeof *anim);

	for(i=0; i<ANM_NUM_TRACKS; i++) {
		if(anm_init_track(anim->tracks + i) == -1) {
			for(j=0; j<i; j++) {
				anm_destroy_track(anim->tracks + j);
			}
			return -1;
		}
		anm_set_track_default(anim->tracks + i, defaults[i]);
	}
	return 0;
}

void anm_destroy_animation(struct anm_animation *anim)
{
	int i;
	for(i=0; i<ANM_NUM_TRACKS; i++) {
		anm_destroy_track(anim->tracks + i);
	}
	free(anim->name);
}

void anm_set_animation_name(struct anm_animation *anim, const char *name)
{
	char *newname = malloc(strlen(name) + 1);
	if(!newname) return;

	strcpy(newname, name);

	free(anim->name);
	anim->name = newname;
}

/* ---- node implementation ----- */

int anm_init_node(struct anm_node *node)
{
	memset(node, 0, sizeof *node);

	node->cur_anim[1] = -1;
	node->blend_dur = -1;

	if(!(node->animations = anm_dynarr_alloc(1, sizeof *node->animations))) {
		return -1;
	}
	if(anm_init_animation(node->animations) == -1) {
		anm_dynarr_free(node->animations);
		return -1;
	}

#ifndef ANIM_THREAD_SAFE
	/* start with a cold matrix cache, so even a first query at time 0
	 * (which happens to match the zero-initialized cache time) recomputes */
	node->cache.time = ANM_TIME_INVAL;
	node->cache.inv_time = ANM_TIME_INVAL;
#endif

#ifdef ANIM_THREAD_SAFE
	/* initialize thread-local matrix cache */
	pthread_key_create(&node->cache_key, 0);
	pthread_mutex_init(&node->cache_list_lock, 0);
#endif

	return 0;
}

void anm_destroy_node(struct anm_node *node)
{
	int i, num_anim;
	free(node->name);

	num_anim = anm_get_animation_count(node);
	for(i=0; i<num_anim; i++) {
		anm_destroy_animation(node->animations + i);
	}
	anm_dynarr_free(node->animations);

#ifdef ANIM_THREAD_SAFE
	/* destroy thread-specific cache */
	pthread_key_delete(node->cache_key);

	while(node->cache_list) {
		struct mat_cache *tmp = node->cache_list;
		node->cache_list = tmp->next;
		free(tmp);
	}
#endif
}

void anm_destroy_node_tree(struct anm_node *tree)
{
	struct anm_node *c, *tmp;

	if(!tree) return;

	c = tree->child;
	while(c) {
		tmp = c;
		c = c->next;

		anm_destroy_node_tree(tmp);
	}
	anm_destroy_node(tree);
}

struct anm_node *anm_create_node(void)
{
	struct anm_node *n;

	if((n = malloc(sizeof *n))) {
		if(anm_init_node(n) == -1) {
			free(n);
			return 0;
		}
	}
	return n;
}

void anm_free_node(struct anm_node *node)
{
	anm_destroy_node(node);
	free(node);
}

void anm_free_node_tree(struct anm_node *tree)
{
	struct anm_node *c, *tmp;

	if(!tree) return;

	c = tree->child;
	while(c) {
		tmp = c;
		c = c->next;

		anm_free_node_tree(tmp);
	}

	anm_free_node(tree);
}

int anm_set_node_name(struct anm_node *node, const char *name)
{
	char *str;

	if(!(str = malloc(strlen(name) + 1))) {
		return -1;
	}
	strcpy(str, name);
	free(node->name);
	node->name = str;
	return 0;
}

const char *anm_get_node_name(struct anm_node *node)
{
	return node->name ? node->name : "";
}

void anm_link_node(struct anm_node *p, struct anm_node *c)
{
	c->next = p->child;
	p->child = c;

	c->parent = p;
	invalidate_cache(c);
}

int anm_unlink_node(struct anm_node *p, struct anm_node *c)
{
	struct anm_node *iter;

	if(p->child == c) {
		p->child = c->next;
		c->next = 0;
		invalidate_cache(c);
		return 0;
	}

	iter = p->child;
	while(iter->next) {
		if(iter->next == c) {
			iter->next = c->next;
			c->next = 0;
			invalidate_cache(c);
			return 0;
		}
	}
	return -1;
}

void anm_set_pivot(struct anm_node *node, float x, float y, float z)
{
	node->pivot[0] = x;
	node->pivot[1] = y;
	node->pivot[2] = z;
}

void anm_get_pivot(struct anm_node *node, float *x, float *y, float *z)
{
	*x = node->pivot[0];
	*y = node->pivot[1];
	*z = node->pivot[2];
}


/* animation management */

int anm_use_node_animation(struct anm_node *node, int aidx)
{
	if(aidx == node->cur_anim[0] && node->cur_anim[1] == -1) {
		return 0;	/* no change, no invalidation */
	}

	if(aidx < 0 || aidx >= anm_get_animation_count(node)) {
		return -1;
	}

	node->cur_anim[0] = aidx;
	node->cur_anim[1] = -1;
	node->cur_mix = 0;
	node->blend_dur = -1;

	invalidate_cache(node);
	return 0;
}

int anm_use_node_animations(struct anm_node *node, int aidx, int bidx, float t)
{
	int num_anim;

	if(node->cur_anim[0] == aidx && node->cur_anim[1] == bidx &&
			fabs(t - node->cur_mix) < 1e-6) {
		return 0;	/* no change, no invalidation */
	}

	num_anim = anm_get_animation_count(node);
	if(aidx < 0 || aidx >= num_anim) {
		return anm_use_animation(node, bidx);
	}
	if(bidx < 0 || bidx >= num_anim) {
		return anm_use_animation(node, aidx);
	}
	node->cur_anim[0] = aidx;
	node->cur_anim[1] = bidx;
	node->cur_mix = t;

	invalidate_cache(node);
	return 0;
}

int anm_use_animation(struct anm_node *node, int aidx)
{
	struct anm_node *child;

	if(anm_use_node_animation(node, aidx) == -1) {
		return -1;
	}

	child = node->child;
	while(child) {
		if(anm_use_animation(child, aidx) == -1) {
			return -1;
		}
		child = child->next;
	}
	return 0;
}

int anm_use_animations(struct anm_node *node, int aidx, int bidx, float t)
{
	struct anm_node *child;

	if(anm_use_node_animations(node, aidx, bidx, t) == -1) {
		return -1;
	}

	child = node->child;
	while(child) {
		if(anm_use_animations(child, aidx, bidx, t) == -1) {
			return -1;
		}
		child = child->next;
	}
	return 0;

}

void anm_set_node_animation_offset(struct anm_node *node, anm_time_t offs, int which)
{
	if(which < 0 || which >= 2) {
		return;
	}
	node->cur_anim_offset[which] = offs;
}

anm_time_t anm_get_animation_offset(const struct anm_node *node, int which)
{
	if(which < 0 || which >= 2) {
		return 0;
	}
	return node->cur_anim_offset[which];
}

void anm_set_animation_offset(struct anm_node *node, anm_time_t offs, int which)
{
	struct anm_node *c = node->child;
	while(c) {
		anm_set_animation_offset(c, offs, which);
		c = c->next;
	}

	anm_set_node_animation_offset(node, offs, which);
}

int anm_get_active_animation_index(const struct anm_node *node, int which)
{
	if(which < 0 || which >= 2) return -1;
	return node->cur_anim[which];
}

struct anm_animation *anm_get_active_animation(const struct anm_node *node, int which)
{
	int idx = anm_get_active_animation_index(node, which);
	if(idx < 0 || idx >= anm_get_animation_count(node)) {
		return 0;
	}
	return node->animations + idx;
}

float anm_get_active_animation_mix(const struct anm_node *node)
{
	return node->cur_mix;
}

int anm_get_animation_count(const struct anm_node *node)
{
	return anm_dynarr_size(node->animations);
}

int anm_add_node_animation(struct anm_node *node)
{
	struct anm_animation newanim;
	anm_init_animation(&newanim);

	node->animations = anm_dynarr_push(node->animations, &newanim);
	return 0;
}

int anm_remove_node_animation(struct anm_node *node, int idx)
{
	fprintf(stderr, "anm_remove_animation: unimplemented!");
	abort();
	return 0;
}

int anm_add_animation(struct anm_node *node)
{
	struct anm_node *child;

	if(anm_add_node_animation(node) == -1) {
		return -1;
	}

	child = node->child;
	while(child) {
		if(anm_add_animation(child)) {
			return -1;
		}
		child = child->next;
	}
	return 0;
}

int anm_remove_animation(struct anm_node *node, int idx)
{
	struct anm_node *child;

	if(anm_remove_node_animation(node, idx) == -1) {
		return -1;
	}

	child = node->child;
	while(child) {
		if(anm_remove_animation(child, idx) == -1) {
			return -1;
		}
		child = child->next;
	}
	return 0;
}

struct anm_animation *anm_get_animation(struct anm_node *node, int idx)
{
	if(idx < 0 || idx > anm_get_animation_count(node)) {
		return 0;
	}
	return node->animations + idx;
}

struct anm_animation *anm_get_animation_by_name(struct anm_node *node, const char *name)
{
	return anm_get_animation(node, anm_find_animation(node, name));
}

int anm_find_animation(struct anm_node *node, const char *name)
{
	int i, count = anm_get_animation_count(node);
	for(i=0; i<count; i++) {
		if(strcmp(node->animations[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

/* all the rest act on the current animation(s) */

void anm_set_interpolator(struct anm_node *node, enum anm_interpolator in)
{
	int i;
	struct anm_animation *anim = anm_get_active_animation(node, 0);
	if(!anim) return;

	for(i=0; i<ANM_NUM_TRACKS; i++) {
		anm_set_track_interpolator(anim->tracks + i, in);
	}
	invalidate_cache(node);
}

void anm_set_extrapolator(struct anm_node *node, enum anm_extrapolator ex)
{
	int i;
	struct anm_animation *anim = anm_get_active_animation(node, 0);
	if(!anim) return;

	for(i=0; i<ANM_NUM_TRACKS; i++) {
		anm_set_track_extrapolator(anim->tracks + i, ex);
	}
	invalidate_cache(node);
}

void anm_set_node_active_animation_name(struct anm_node *node, const char *name)
{
	struct anm_animation *anim = anm_get_active_animation(node, 0);
	if(!anim) return;

	anm_set_animation_name(anim, name);
}

void anm_set_active_animation_name(struct anm_node *node, const char *name)
{
	struct anm_node *child;

	anm_set_node_active_animation_name(node, name);

	child = node->child;
	while(child) {
		anm_set_active_animation_name(child, name);
		child = child->next;
	}
}

const char *anm_get_active_animation_name(struct anm_node *node)
{
	struct anm_animation *anim = anm_get_active_animation(node, 0);
	if(anim) {
		return anim->name;
	}
	return 0;
}

/* ---- high level animation blending ---- */
void anm_transition(struct anm_node *node, int anmidx, anm_time_t start, anm_time_t dur)
{
	struct anm_node *c = node->child;

	if(anmidx == node->cur_anim[0]) {
		return;
	}

	while(c) {
		anm_transition(c, anmidx, start, dur);
		c = c->next;
	}

	anm_node_transition(node, anmidx, start, dur);
}

void anm_node_transition(struct anm_node *node, int anmidx, anm_time_t start, anm_time_t dur)
{
	if(anmidx == node->cur_anim[0]) {
		return;
	}

	node->cur_anim[1] = anmidx;
	node->cur_anim_offset[1] = start;
	node->blend_dur = dur;
}


#define BLEND_START_TM	node->cur_anim_offset[1]

static anm_time_t animation_time(struct anm_node *node, anm_time_t tm, int which)
{
	float t;

	if(node->blend_dur >= 0) {
		/* we're in transition... */
		t = (float)(tm - BLEND_START_TM) / (float)node->blend_dur;
		if(t < 0.0) t = 0.0;

		node->cur_mix = t;

		if(t > 1.0) {
			/* switch completely over to the target animation and stop blending */
			anm_use_node_animation(node, node->cur_anim[1]);
			node->cur_anim_offset[0] = node->cur_anim_offset[1];
		}
	}

	return tm - node->cur_anim_offset[which];
}


void anm_set_position(struct anm_node *node, const float *pos, anm_time_t tm)
{
	anm_set_position3f(node, pos[0], pos[1], pos[2], tm);
}

void anm_set_position3f(struct anm_node *node, float x, float y, float z, anm_time_t tm)
{
	struct anm_animation *anim = anm_get_active_animation(node, 0);
	if(!anim) return;

	anm_set_value(anim->tracks + ANM_TRACK_POS_X, tm, x);
	anm_set_value(anim->tracks + ANM_TRACK_POS_Y, tm, y);
	anm_set_value(anim->tracks + ANM_TRACK_POS_Z, tm, z);
	invalidate_cache(node);
}

void anm_get_node_position(struct anm_node *node, float *pos, anm_time_t tm)
{
	anm_time_t tm0 = animation_time(node, tm, 0);
	struct anm_animation *anim0 = anm_get_active_animation(node, 0);
	struct anm_animation *anim1 = anm_get_active_animation(node, 1);

	if(!anim0) {
		pos[0] = pos[1] = pos[2] = 0.0f;
		return;
	}

	pos[0] = anm_get_value(anim0->tracks + ANM_TRACK_POS_X, tm0);
	pos[1] = anm_get_value(anim0->tracks + ANM_TRACK_POS_Y, tm0);
	pos[2] = anm_get_value(anim0->tracks + ANM_TRACK_POS_Z, tm0);

	if(anim1) {
		anm_time_t tm1 = animation_time(node, tm, 1);
		float x1 = anm_get_value(anim1->tracks + ANM_TRACK_POS_X, tm1);
		float y1 = anm_get_value(anim1->tracks + ANM_TRACK_POS_Y, tm1);
		float z1 = anm_get_value(anim1->tracks + ANM_TRACK_POS_Z, tm1);

		pos[0] = pos[0] + (x1 - pos[0]) * node->cur_mix;
		pos[1] = pos[1] + (y1 - pos[1]) * node->cur_mix;
		pos[2] = pos[2] + (z1 - pos[2]) * node->cur_mix;
	}
}

void anm_set_rotation(struct anm_node *node, const float *qrot, anm_time_t tm)
{
	anm_set_rotation4f(node, qrot[0], qrot[1], qrot[2], qrot[3], tm);
}

void anm_set_rotation4f(struct anm_node *node, float x, float y, float z, float w, anm_time_t tm)
{
	struct anm_animation *anim = anm_get_active_animation(node, 0);
	if(!anim) return;

	anm_set_value(anim->tracks + ANM_TRACK_ROT_X, tm, x);
	anm_set_value(anim->tracks + ANM_TRACK_ROT_Y, tm, y);
	anm_set_value(anim->tracks + ANM_TRACK_ROT_Z, tm, z);
	anm_set_value(anim->tracks + ANM_TRACK_ROT_W, tm, w);
	invalidate_cache(node);
}

void anm_set_rotation_axis(struct anm_node *node, float angle, float x, float y, float z, anm_time_t tm)
{
	cgm_quat q;
	cgm_qrotation(&q, angle, x, y, z);

	anm_set_rotation(node, (float*)&q, tm);
}

static void get_node_rotation(cgm_quat *qres, struct anm_node *node, anm_time_t tm, struct anm_animation *anim)
{
#ifndef ROT_USE_SLERP
	qres->x = anm_get_value(anim->tracks + ANM_TRACK_ROT_X, tm);
	qres->y = anm_get_value(anim->tracks + ANM_TRACK_ROT_Y, tm);
	qres->z = anm_get_value(anim->tracks + ANM_TRACK_ROT_Z, tm);
	qres->w = anm_get_value(anim->tracks + ANM_TRACK_ROT_W, tm);
#else
	int idx0, idx1, last_idx;
	anm_time_t tstart, tend;
	float t, dt;
	struct anm_track *track_x, *track_y, *track_z, *track_w;
	cgm_quat q1, q2;

	track_x = anim->tracks + ANM_TRACK_ROT_X;
	track_y = anim->tracks + ANM_TRACK_ROT_Y;
	track_z = anim->tracks + ANM_TRACK_ROT_Z;
	track_w = anim->tracks + ANM_TRACK_ROT_W;

	if(!track_x->count) {
		qres->x = track_x->def_val;
		qres->y = track_y->def_val;
		qres->z = track_z->def_val;
		qres->w = track_w->def_val;
		return;
	}

	last_idx = track_x->count - 1;

	tstart = track_x->keys[0].time;
	tend = track_x->keys[last_idx].time;

	if(tstart == tend) {
		qres->x = track_x->keys[0].val;
		qres->y = track_y->keys[0].val;
		qres->z = track_z->keys[0].val;
		qres->w = track_w->keys[0].val;
		return;
	}

	tm = anm_remap_time(track_x, tm, tstart, tend);

	idx0 = anm_get_key_interval(track_x, tm);
	assert(idx0 >= 0 && idx0 < track_x->count);
	idx1 = idx0 + 1;

	if(idx0 == last_idx) {
		qres->x = track_x->keys[idx0].val;
		qres->y = track_y->keys[idx0].val;
		qres->z = track_z->keys[idx0].val;
		qres->w = track_w->keys[idx0].val;
		return;
	}

	dt = (float)(track_x->keys[idx1].time - track_x->keys[idx0].time);
	t = (float)(tm - track_x->keys[idx0].time) / dt;

	q1.x = track_x->keys[idx0].val;
	q1.y = track_y->keys[idx0].val;
	q1.z = track_z->keys[idx0].val;
	q1.w = track_w->keys[idx0].val;

	q2.x = track_x->keys[idx1].val;
	q2.y = track_y->keys[idx1].val;
	q2.z = track_z->keys[idx1].val;
	q2.w = track_w->keys[idx1].val;

	cgm_qslerp(qres, &q1, &q2, t);
#endif
}

//get_node_rotation(cgm_quat *qres, struct anm_node *node, anm_time_t tm, struct anm_animation *anim)
void anm_get_node_rotation(struct anm_node *node, float *qrot, anm_time_t tm)
{
	anm_time_t tm0 = animation_time(node, tm, 0);
	struct anm_animation *anim0 = anm_get_active_animation(node, 0);
	struct anm_animation *anim1 = anm_get_active_animation(node, 1);

	if(!anim0) {
		qrot[0] = qrot[1] = qrot[2] = 0.0f;
		qrot[3] = 1.0f;
		return;
	}


	if(anim1) {
		cgm_quat q0, q1;
		anm_time_t tm1 = animation_time(node, tm, 1);

		get_node_rotation(&q0, node, tm0, anim0);
		get_node_rotation(&q1, node, tm1, anim1);

		cgm_qslerp((cgm_quat*)qrot, &q0, &q1, node->cur_mix);
	} else {
		get_node_rotation((cgm_quat*)qrot, node, tm0, anim0);
	}
}

void anm_set_scaling(struct anm_node *node, const float *scale, anm_time_t tm)
{
	anm_set_scaling3f(node, scale[0], scale[1], scale[2], tm);
}

void anm_set_scaling3f(struct anm_node *node, float x, float y, float z, anm_time_t tm)
{
	struct anm_animation *anim = anm_get_active_animation(node, 0);
	if(!anim) return;

	anm_set_value(anim->tracks + ANM_TRACK_SCL_X, tm, x);
	anm_set_value(anim->tracks + ANM_TRACK_SCL_Y, tm, y);
	anm_set_value(anim->tracks + ANM_TRACK_SCL_Z, tm, z);
	invalidate_cache(node);
}

void anm_get_node_scaling(struct anm_node *node, float *scale, anm_time_t tm)
{
	anm_time_t tm0 = animation_time(node, tm, 0);
	struct anm_animation *anim0 = anm_get_active_animation(node, 0);
	struct anm_animation *anim1 = anm_get_active_animation(node, 1);

	if(!anim0) {
		scale[0] = scale[1] = scale[2] = 1.0f;
		return;
	}

	scale[0] = anm_get_value(anim0->tracks + ANM_TRACK_SCL_X, tm0);
	scale[1] = anm_get_value(anim0->tracks + ANM_TRACK_SCL_Y, tm0);
	scale[2] = anm_get_value(anim0->tracks + ANM_TRACK_SCL_Z, tm0);

	if(anim1) {
		anm_time_t tm1 = animation_time(node, tm, 1);
		float x1 = anm_get_value(anim1->tracks + ANM_TRACK_SCL_X, tm1);
		float y1 = anm_get_value(anim1->tracks + ANM_TRACK_SCL_Y, tm1);
		float z1 = anm_get_value(anim1->tracks + ANM_TRACK_SCL_Z, tm1);

		scale[0] = scale[0] + (x1 - scale[0]) * node->cur_mix;
		scale[1] = scale[1] + (y1 - scale[1]) * node->cur_mix;
		scale[2] = scale[2] + (z1 - scale[2]) * node->cur_mix;
	}
}

void anm_get_position(struct anm_node *node, float *pos, anm_time_t tm)
{
	if(!node->parent) {
		anm_get_node_position(node, pos, tm);
	} else {
		float xform[16];
		anm_get_matrix(node, xform, tm);
		cgm_mget_translation(xform, (cgm_vec3*)pos);
	}
}

void anm_get_rotation(struct anm_node *node, float *qrot, anm_time_t tm)
{
	if(!node->parent) {
		anm_get_node_rotation(node, qrot, tm);
	} else {
		cgm_quat rot;
		anm_get_node_rotation(node, &rot.x, tm);
		anm_get_rotation(node->parent, qrot, tm);
		cgm_qmul((cgm_quat*)qrot, &rot);
	}
}

void anm_get_scaling(struct anm_node *node, float *scale, anm_time_t tm)
{
	anm_get_node_scaling(node, scale, tm);
	if(node->parent) {
		cgm_vec3 ps;
		anm_get_scaling(node->parent, &ps.x, tm);
		cgm_vmul((cgm_vec3*)scale, &ps);
	}
}

void anm_get_node_matrix(struct anm_node *node, float *mat, anm_time_t tm)
{
	int i;
	float rmat[16];
	cgm_vec3 pos, scale;
	cgm_quat rot;

	anm_get_node_position(node, &pos.x, tm);
	anm_get_node_rotation(node, &rot.x, tm);
	anm_get_node_scaling(node, &scale.x, tm);

	cgm_mtranslation(mat, node->pivot[0], node->pivot[1], node->pivot[2]);
	cgm_mrotation_quat(rmat, &rot);

	/*
	for(i=0; i<3; i++) {
		mat[i * 4] = rmat[i * 4];
		mat[i * 4 + 1] = rmat[i * 4 + 1];
		mat[i * 4 + 2] = rmat[i * 4 + 2];
	}
	*/
	for(i=0; i<3; i++) {
		mat[i] = rmat[i];
		mat[4 + i] = rmat[4 + i];
		mat[8 + i] = rmat[8 + i];
	}

	mat[0] *= scale.x; mat[4] *= scale.y; mat[8] *= scale.z; mat[12] += pos.x;
	mat[1] *= scale.x; mat[5] *= scale.y; mat[9] *= scale.z; mat[13] += pos.y;
	mat[2] *= scale.x; mat[6] *= scale.y; mat[10] *= scale.z; mat[14] += pos.z;

	cgm_mpretranslate(mat, -node->pivot[0], -node->pivot[1], -node->pivot[2]);

	/* that's basically: pivot * rotation * translation * scaling * -pivot */
}

void anm_get_node_inv_matrix(struct anm_node *node, float *mat, anm_time_t tm)
{
	anm_get_node_matrix(node, mat, tm);
	cgm_minverse(mat);
}

void anm_eval_node(struct anm_node *node, anm_time_t tm)
{
	anm_get_node_matrix(node, node->matrix, tm);
}

void anm_eval(struct anm_node *node, anm_time_t tm)
{
	struct anm_node *c;

	anm_eval_node(node, tm);

	if(node->parent) {
		/* due to post-order traversal, the parent matrix is already evaluated */
		cgm_mmul(node->matrix, node->parent->matrix);
	}

	/* recersively evaluate all children */
	c = node->child;
	while(c) {
		anm_eval(c, tm);
		c = c->next;
	}
}

float *anm_get_matrix(struct anm_node *node, float *mat, anm_time_t tm)
{
#ifdef ANIM_THREAD_SAFE
	struct mat_cache *cache = pthread_getspecific(node->cache_key);
	if(!cache) {
		cache = malloc(sizeof *cache);
		assert(cache);

		pthread_mutex_lock(&node->cache_list_lock);
		cache->next = node->cache_list;
		node->cache_list = cache;
		pthread_mutex_unlock(&node->cache_list_lock);

		cache->time = ANM_TIME_INVAL;
		cache->inv_time = ANM_TIME_INVAL;
		pthread_setspecific(node->cache_key, cache);
	}
#else
	struct mat_cache *cache = &node->cache;
#endif

	if(cache->time != tm) {
		anm_get_node_matrix(node, cache->matrix, tm);

		if(node->parent) {
			float parent_mat[16];

			anm_get_matrix(node->parent, parent_mat, tm);
			cgm_mmul(cache->matrix, parent_mat);
		}
		cache->time = tm;
	}

	if(mat) {
		cgm_mcopy(mat, cache->matrix);
	}
	return cache->matrix;
}

float *anm_get_inv_matrix(struct anm_node *node, float *mat, anm_time_t tm)
{
#ifdef ANIM_THREAD_SAFE
	struct mat_cache *cache = pthread_getspecific(node->cache_key);
	if(!cache) {
		cache = malloc(sizeof *cache);
		assert(cache);

		pthread_mutex_lock(&node->cache_list_lock);
		cache->next = node->cache_list;
		node->cache_list = cache;
		pthread_mutex_unlock(&node->cache_list_lock);

		cache->time = ANM_TIME_INVAL;
		cache->inv_time = ANM_TIME_INVAL;
		pthread_setspecific(node->cache_key, cache);
	}
#else
	struct mat_cache *cache = &node->cache;
#endif

	if(cache->inv_time != tm) {
		float tmp[16];

		anm_get_matrix(node, tmp, tm);
		cgm_mcopy(cache->inv_matrix, tmp);
		cgm_minverse(cache->inv_matrix);
		cache->inv_time = tm;
	}

	if(mat) {
		cgm_mcopy(mat, cache->inv_matrix);
	}
	return cache->inv_matrix;
}

anm_time_t anm_get_start_time(struct anm_node *node)
{
	int i, j;
	struct anm_node *c;
	anm_time_t res = LONG_MAX;

	for(j=0; j<2; j++) {
		struct anm_animation *anim = anm_get_active_animation(node, j);
		if(!anim) break;

		for(i=0; i<ANM_NUM_TRACKS; i++) {
			if(anim->tracks[i].count) {
				anm_time_t tm = anim->tracks[i].keys[0].time;
				if(tm < res) {
					res = tm;
				}
			}
		}
	}

	c = node->child;
	while(c) {
		anm_time_t tm = anm_get_start_time(c);
		if(tm < res) {
			res = tm;
		}
		c = c->next;
	}
	return res;
}

anm_time_t anm_get_end_time(struct anm_node *node)
{
	int i, j;
	struct anm_node *c;
	anm_time_t res = LONG_MIN;

	for(j=0; j<2; j++) {
		struct anm_animation *anim = anm_get_active_animation(node, j);
		if(!anim) break;

		for(i=0; i<ANM_NUM_TRACKS; i++) {
			if(anim->tracks[i].count) {
				anm_time_t tm = anim->tracks[i].keys[anim->tracks[i].count - 1].time;
				if(tm > res) {
					res = tm;
				}
			}
		}
	}

	c = node->child;
	while(c) {
		anm_time_t tm = anm_get_end_time(c);
		if(tm > res) {
			res = tm;
		}
		c = c->next;
	}
	return res;
}

static void invalidate_cache(struct anm_node *node)
{
	struct anm_node *c;

#ifdef ANIM_THREAD_SAFE
	struct mat_cache *cache = pthread_getspecific(node->cache_key);
	if(cache) {
	   cache->time = cache->inv_time = ANM_TIME_INVAL;
	}
#else
	node->cache.time = node->cache.inv_time = ANM_TIME_INVAL;
#endif

	c = node->child;
	while(c) {
		invalidate_cache(c);
		c = c->next;
	}
}
