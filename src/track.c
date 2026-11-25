/*
libanim - hierarchical keyframe animation library
Copyright (C) 2012-2023 John Tsiombikas <nuclear@member.fsf.org>

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

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "track.h"
#include "dynarr.h"

#include "cgmath/cgmath.h"

static int keycmp(const void *a, const void *b);
static int find_prev_key(const struct anm_keyframe *arr, int start, int end, anm_time_t tm);
static void release_keys(struct anm_track *track);
static int detach_keys(struct anm_track *track);

static float interp_step(float v0, float v1, float v2, float v3, float t);
static float interp_linear(float v0, float v1, float v2, float v3, float t);
static float interp_cubic(float v0, float v1, float v2, float v3, float t);

static anm_time_t remap_extend(anm_time_t tm, anm_time_t start, anm_time_t end);
static anm_time_t remap_clamp(anm_time_t tm, anm_time_t start, anm_time_t end);
static anm_time_t remap_repeat(anm_time_t tm, anm_time_t start, anm_time_t end);
static anm_time_t remap_pingpong(anm_time_t tm, anm_time_t start, anm_time_t end);

/* XXX keep this in sync with enum anm_interpolator at track.h */
static float (*interp[])(float, float, float, float, float) = {
	interp_step,
	interp_linear,
	interp_cubic,
	0
};

/* XXX keep this in sync with enum anm_extrapolator at track.h */
static anm_time_t (*remap_time[])(anm_time_t, anm_time_t, anm_time_t) = {
	remap_extend,
	remap_clamp,
	remap_repeat,
	remap_pingpong,
	0
};

int anm_init_track(struct anm_track *track)
{
	memset(track, 0, sizeof *track);

	if(!(track->keys = anm_dynarr_alloc(0, sizeof *track->keys))) {
		return -1;
	}
	if(!(track->keys_refcount = malloc(sizeof *track->keys_refcount))) {
		anm_dynarr_free(track->keys);
		track->keys = 0;
		return -1;
	}
	*track->keys_refcount = 1;

	track->interp = ANM_INTERP_LINEAR;
	track->extrap = ANM_EXTRAP_CLAMP;
	return 0;
}

void anm_destroy_track(struct anm_track *track)
{
	release_keys(track);
	track->keys = 0;
	track->keys_refcount = 0;
}

/* drop the track's reference to its key buffer, freeing it when it was the
 * last holder */
static void release_keys(struct anm_track *track)
{
	if(track->keys_refcount) {
		if(--*track->keys_refcount <= 0) {
			free(track->keys_refcount);
			track->keys_refcount = 0;
			anm_dynarr_free(track->keys);
			track->keys = 0;
		}
	} else if(track->keys) {
		/* no reference count: this buffer is not shared through the API */
		anm_dynarr_free(track->keys);
	}
}

/* if the key buffer is shared with other tracks, give this track a private
 * copy of it, so writes don't affect the other sharers */
static int detach_keys(struct anm_track *track)
{
	struct anm_keyframe *newkeys;
	int *newref;

	if(!track->keys_refcount || *track->keys_refcount <= 1) {
		return 0;
	}

	if(!(newkeys = anm_dynarr_alloc(track->count, sizeof *newkeys))) {
		return -1;
	}
	if(!(newref = malloc(sizeof *newref))) {
		anm_dynarr_free(newkeys);
		return -1;
	}
	if(track->count) {
		memcpy(newkeys, track->keys, track->count * sizeof *newkeys);
	}

	release_keys(track);

	track->keys = newkeys;
	track->keys_refcount = newref;
	*newref = 1;
	return 0;
}

struct anm_track *anm_create_track(void)
{
	struct anm_track *track;

	if((track = malloc(sizeof *track))) {
		if(anm_init_track(track) == -1) {
			free(track);
			return 0;
		}
	}
	return track;
}

void anm_free_track(struct anm_track *track)
{
	anm_destroy_track(track);
	free(track);
}

void anm_copy_track(struct anm_track *dest, const struct anm_track *src)
{
	release_keys(dest);
	free(dest->name);
	dest->name = 0;

	if(src->name) {
		dest->name = malloc(strlen(src->name) + 1);
		strcpy(dest->name, src->name);
	}

	dest->count = src->count;
	dest->keys = anm_dynarr_alloc(src->count, sizeof *dest->keys);
	if(!dest->keys) {
		dest->count = 0;
		return;
	}
	if(!(dest->keys_refcount = malloc(sizeof *dest->keys_refcount))) {
		anm_dynarr_free(dest->keys);
		dest->keys = 0;
		dest->count = 0;
		return;
	}
	*dest->keys_refcount = 1;
	if(src->count) {
		memcpy(dest->keys, src->keys, src->count * sizeof *dest->keys);
	}

	dest->def_val = src->def_val;
	dest->interp = src->interp;
	dest->extrap = src->extrap;
}

int anm_share_track(struct anm_track *dest, const struct anm_track *src)
{
	if(dest == src) {
		return 0;	/* sharing with itself is a no-op */
	}
	if(!dest->keys || !src->keys || !src->keys_refcount) {
		return -1;	/* not initialized */
	}

	/* the destination must end up holding its own reference to the buffer */
	if(dest->keys_refcount) {
		release_keys(dest);
	} else {
		anm_dynarr_free(dest->keys);
	}

	dest->keys = src->keys;
	dest->keys_refcount = src->keys_refcount;
	dest->count = src->count;
	++*dest->keys_refcount;

	/* scalar settings and name are independent copies of the source's */
	dest->def_val = src->def_val;
	dest->interp = src->interp;
	dest->extrap = src->extrap;

	if(src->name) {
		size_t len = strlen(src->name) + 1;
		char *new_name = malloc(len);
		if(new_name) {
			memcpy(new_name, src->name, len);
			free(dest->name);
			dest->name = new_name;
		}
	}

	return 0;
}

int anm_set_track_name(struct anm_track *track, const char *name)
{
	char *tmp;

	if(!(tmp = malloc(strlen(name) + 1))) {
		return -1;
	}
	free(track->name);
	track->name = tmp;
	return 0;
}

const char *anm_get_track_name(const struct anm_track *track)
{
	return track->name;
}

void anm_set_track_interpolator(struct anm_track *track, enum anm_interpolator in)
{
	track->interp = in;
}

void anm_set_track_extrapolator(struct anm_track *track, enum anm_extrapolator ex)
{
	track->extrap = ex;
}

anm_time_t anm_remap_time(const struct anm_track *track, anm_time_t tm, anm_time_t start, anm_time_t end)
{
	return remap_time[track->extrap](tm, start, end);
}

void anm_set_track_default(struct anm_track *track, float def)
{
	track->def_val = def;
}

int anm_set_keyframe(struct anm_track *track, struct anm_keyframe *key)
{
	int idx;
	void *tmp;

	/* copy-on-write: detach the key buffer before writing to it, if it is
	 * shared with other tracks */
	if(detach_keys(track) == -1) {
		return -1;
	}

	idx = anm_get_key_interval(track, key->time);

	/* if we got a valid keyframe index, compare them... */
	if(idx >= 0 && idx < track->count && keycmp(key, track->keys + idx) == 0) {
		/* ... it's the same key, just update the value */
		track->keys[idx].val = key->val;
	} else {
		/* ... it's a new key, add it and re-sort them */
		if(!(tmp = anm_dynarr_push(track->keys, key))) {
			return -1;
		}
		track->keys = tmp;
		/* TODO lazy qsort */
		qsort(track->keys, ++track->count, sizeof *track->keys, keycmp);
	}
	return 0;
}

static int keycmp(const void *a, const void *b)
{
	return ((struct anm_keyframe*)a)->time - ((struct anm_keyframe*)b)->time;
}

struct anm_keyframe *anm_get_keyframe(const struct anm_track *track, int idx)
{
	if(idx < 0 || idx >= track->count) {
		return 0;
	}
	return track->keys + idx;
}

int anm_get_key_interval(const struct anm_track *track, anm_time_t tm)
{
	int last;

	if(!track->count || tm < track->keys[0].time) {
		return -1;
	}

	last = track->count - 1;
	if(tm > track->keys[last].time) {
		return last;
	}

	return find_prev_key(track->keys, 0, last, tm);
}

static int find_prev_key(const struct anm_keyframe *arr, int start, int end, anm_time_t tm)
{
	int mid;

	if(end - start <= 1) {
		return start;
	}

	mid = (start + end) / 2;
	if(tm < arr[mid].time) {
		return find_prev_key(arr, start, mid, tm);
	}
	if(tm > arr[mid].time) {
		return find_prev_key(arr, mid, end, tm);
	}
	return mid;
}

int anm_set_value(struct anm_track *track, anm_time_t tm, float val)
{
	struct anm_keyframe key;
	key.time = tm;
	key.val = val;

	return anm_set_keyframe(track, &key);
}

float anm_get_value(const struct anm_track *track, anm_time_t tm)
{
	int idx0, idx1, last_idx;
	anm_time_t tstart, tend;
	float t, dt;
	float v0, v1, v2, v3;

	if(!track->count) {
		return track->def_val;
	}

	last_idx = track->count - 1;

	tstart = track->keys[0].time;
	tend = track->keys[last_idx].time;

	if(tstart == tend) {
		return track->keys[0].val;
	}

	tm = remap_time[track->extrap](tm, tstart, tend);

	idx0 = anm_get_key_interval(track, tm);
	assert(idx0 >= 0 && idx0 < track->count);
	idx1 = idx0 + 1;

	if(idx0 == last_idx) {
		return track->keys[idx0].val;
	}

	dt = (float)(track->keys[idx1].time - track->keys[idx0].time);
	t = (float)(tm - track->keys[idx0].time) / dt;

	v1 = track->keys[idx0].val;
	v2 = track->keys[idx1].val;

	/* get the neigboring values to allow for cubic interpolation */
	v0 = idx0 > 0 ? track->keys[idx0 - 1].val : v1;
	v3 = idx1 < last_idx ? track->keys[idx1 + 1].val : v2;

	return interp[track->interp](v0, v1, v2, v3, t);
}


void anm_get_quat(const struct anm_track *xtrk, const struct anm_track *ytrk,
		const struct anm_track *ztrk, const struct anm_track *wtrk, anm_time_t tm, float *qres)
{
	int idx0, idx1, last_idx;
	anm_time_t tstart, tend;
	float t, dt;
	cgm_quat q1, q2;

	if(!xtrk->count) {
		qres[0] = xtrk->def_val;
		qres[1] = ytrk->def_val;
		qres[2] = ztrk->def_val;
		qres[3] = wtrk->def_val;
		return;
	}

	last_idx = xtrk->count - 1;

	tstart = xtrk->keys[0].time;
	tend = xtrk->keys[last_idx].time;

	if(tstart == tend) {
		qres[0] = xtrk->keys[0].val;
		qres[1] = ytrk->keys[0].val;
		qres[2] = ztrk->keys[0].val;
		qres[3] = wtrk->keys[0].val;
		return;
	}

	tm = anm_remap_time(xtrk, tm, tstart, tend);

	idx0 = anm_get_key_interval(xtrk, tm);
	assert(idx0 >= 0 && idx0 < xtrk->count);
	idx1 = idx0 + 1;

	if(idx0 == last_idx) {
		qres[0] = xtrk->keys[idx0].val;
		qres[1] = ytrk->keys[idx0].val;
		qres[2] = ztrk->keys[idx0].val;
		qres[3] = wtrk->keys[idx0].val;
		return;
	}

	dt = (float)(xtrk->keys[idx1].time - xtrk->keys[idx0].time);
	t = (float)(tm - xtrk->keys[idx0].time) / dt;

	q1.x = xtrk->keys[idx0].val;
	q1.y = ytrk->keys[idx0].val;
	q1.z = ztrk->keys[idx0].val;
	q1.w = wtrk->keys[idx0].val;

	q2.x = xtrk->keys[idx1].val;
	q2.y = ytrk->keys[idx1].val;
	q2.z = ztrk->keys[idx1].val;
	q2.w = wtrk->keys[idx1].val;

	cgm_qslerp((cgm_quat*)qres, &q1, &q2, t);
}


static float interp_step(float v0, float v1, float v2, float v3, float t)
{
	return v1;
}

static float interp_linear(float v0, float v1, float v2, float v3, float t)
{
	return v1 + (v2 - v1) * t;
}

static float interp_cubic(float a, float b, float c, float d, float t)
{
	float x, y, z, w;
	float tsq = t * t;

	x = -a + 3.0 * b - 3.0 * c + d;
	y = 2.0 * a - 5.0 * b + 4.0 * c - d;
	z = c - a;
	w = 2.0 * b;

	return 0.5 * (x * tsq * t + y * tsq + z * t + w);
}

static anm_time_t remap_extend(anm_time_t tm, anm_time_t start, anm_time_t end)
{
	return remap_repeat(tm, start, end);
}

static anm_time_t remap_clamp(anm_time_t tm, anm_time_t start, anm_time_t end)
{
	if(start == end) {
		return start;
	}
	return tm < start ? start : (tm >= end ? end : tm);
}

static anm_time_t remap_repeat(anm_time_t tm, anm_time_t start, anm_time_t end)
{
	anm_time_t x, interv = end - start;

	if(interv == 0) {
		return start;
	}

	x = (tm - start) % interv;
	if(x < 0) {
		x += interv;
	}
	return x + start;

	/*if(tm < start) {
		while(tm < start) {
			tm += interv;
		}
		return tm;
	}
	return (tm - start) % interv + start;*/
}

static anm_time_t remap_pingpong(anm_time_t tm, anm_time_t start, anm_time_t end)
{
	anm_time_t interv = end - start;
	anm_time_t x = remap_repeat(tm, start, end + interv);

	return x > end ? end + interv - x : x;
}
