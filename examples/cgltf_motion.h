/*
 * cgltf_motion.h - windowless glTF 2.0 animation playback and looping
 *                  root-motion extraction on top of cgltf.
 *
 * The player is a pure evaluator: it never writes into cgltf_node (no
 * sampled TRS is stored back), never modifies accessor/buffer data, and
 * needs no window or graphics context. Every output matrix is column
 * major, 16 floats laid out as in cgltf_node_transform_world().
 *
 * Two or more players may exist at the same time; each player owns only
 * the memory returned through its own cgltf_motion_player handle and keeps
 * no mutable per-query state, so interleaved queries at arbitrary absolute
 * times (paused, reversed, far-then-near) always return the same answer
 * for the same time.
 *
 * The cgltf_data passed in is borrowed: it must stay loaded (buffers
 * loaded, cgltf_validate() accepted) and alive for the whole lifetime of
 * every player built from it. This module never frees it; call
 * cgltf_free() yourself only after all players are released.
 *
 * Minimal call sequence:
 *
 *   cgltf_motion_player* p = NULL;
 *   cgltf_motion_result r = cgltf_motion_create(&data, anim_index,
 *                                               extract_node_index, &p);
 *   if (r != CGLTF_MOTION_OK) { fprintf(stderr, "%s\n",
 *                                       cgltf_motion_result_string(r)); ... }
 *
 *   cgltf_motion_pose pose;       // world matrices: nodes_count * 16 floats
 *   cgltf_motion_matrix motion;   // one 16-float accumulated matrix
 *   r = cgltf_motion_evaluate(p, seconds, &pose, &motion);
 *   ... use pose.world_matrices[i] (16 floats each) and motion.m ...
 *
 *   cgltf_motion_delta(p, t0, t1, &motion);   // inverse(M(t0)) * M(t1)
 *   cgltf_motion_release(p);
 */
#ifndef CGLTF_MOTION_H
#define CGLTF_MOTION_H

#include "cgltf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All functions return one of these. CGLTF_MOTION_OK is the only
 * non-error value and compares equal to 0. */
typedef enum cgltf_motion_result {
	CGLTF_MOTION_OK = 0,
	CGLTF_MOTION_ERROR_INVALID_ARGUMENT = 1,
	CGLTF_MOTION_ERROR_OUT_OF_MEMORY = 2,
	CGLTF_MOTION_ERROR_ANIMATION_INDEX = 3,
	CGLTF_MOTION_ERROR_NODE_INDEX = 4,
	CGLTF_MOTION_ERROR_EMPTY_ANIMATION = 5,       /* no samplers / channels */
	CGLTF_MOTION_ERROR_ZERO_DURATION = 6,         /* earliest == latest input */
	CGLTF_MOTION_ERROR_NON_FINITE_TIME = 7,       /* NaN/Inf input key */
	CGLTF_MOTION_ERROR_KEYS_NOT_STRICTLY_ASCENDING = 8,
	CGLTF_MOTION_ERROR_DUPLICATE_TARGET = 9,      /* two channels, same node+path */
	CGLTF_MOTION_ERROR_WEIGHTS_UNSUPPORTED = 10,  /* morph weights channel */
	CGLTF_MOTION_ERROR_MATRIX_NODE_ANIMATED = 11, /* TRS channel on matrix node */
	CGLTF_MOTION_ERROR_NON_RIGID_CHAIN = 12,      /* scale/shear/reflection on the
	                                               * extraction node or an ancestor */
	CGLTF_MOTION_ERROR_BAD_ACCESSOR = 13,         /* unreadable / malformed data */
	CGLTF_MOTION_ERROR_INTERNAL = 14
} cgltf_motion_result;

/* Column-major 4x4, identical layout to cgltf_node_transform_world(). */
typedef struct cgltf_motion_matrix {
	cgltf_float m[16];
} cgltf_motion_matrix;

/*
 * A pose at one absolute time.
 *
 * world_matrices points to nodes_count * 16 contiguous floats; node i
 * occupies world_matrices[i * 16 .. i * 16 + 15]. The pointer is owned by
 * the player, stays valid until cgltf_motion_release(), and is overwritten
 * in place by the next evaluation of THIS player. Other players have
 * their own storage.
 */
typedef struct cgltf_motion_pose {
	cgltf_float* world_matrices; /* nodes_count matrices, column major */
	cgltf_size nodes_count;
} cgltf_motion_pose;

typedef struct cgltf_motion_player cgltf_motion_player;

/* Human-readable, never-NULL English description of a result code. */
const char* cgltf_motion_result_string(cgltf_motion_result result);

/*
 * Build a player for animation animation_index of data, using
 * extract_node_index as the node whose rigid body motion is unwrapped
 * into the accumulated root-motion matrix.
 *
 * data must have its buffers loaded and must have passed
 * cgltf_validate(). On success *out_player is non-NULL and must be passed
 * to cgltf_motion_release(); on failure *out_player is left untouched.
 * Returns an error code (never succeeds partially) for empty or
 * zero-duration animations, weights channels, duplicated node/path
 * targets, non-strictly-ascending or non-finite sampler keys, TRS
 * animation of a matrix node, and a non-rigid extraction chain
 * (non-unit scale, shear or reflection on the extraction node or any
 * of its ancestors). Side branches outside that ancestor chain may
 * scale freely.
 */
cgltf_motion_result cgltf_motion_create(
		const cgltf_data* data,
		cgltf_size animation_index,
		cgltf_size extract_node_index,
		cgltf_motion_player** out_player);

/* Number of nodes in the bound document; the pose buffer holds this many
 * 16-float matrices. */
cgltf_size cgltf_motion_nodes_count(const cgltf_motion_player* player);

/* Earliest (a) and latest (b) sampler input times of the animation. */
cgltf_float cgltf_motion_time_begin(const cgltf_motion_player* player);
cgltf_float cgltf_motion_time_end(const cgltf_motion_player* player);

/*
 * Evaluate at an arbitrary finite absolute time t (seconds; may be before
 * a, after b or negative). Each sampler clamps outside its own key range,
 * unanimated TRS keeps the static file values, and matrix nodes keep
 * participating through their static matrix.
 *
 * pose receives the IN-PLACE world matrices
 *     P_i(t) = G0 * inverse(G(u)) * W_i(u),
 * where G(u) is the rigid world matrix of the extraction node in the
 * ordinary sampled hierarchy, G0 = G(a), u is the wrapped phase time in
 * [a, b) and W_i(u) is node i's ordinary sampled world matrix. The
 * extraction node therefore stays fixed at its start pose.
 *
 * motion receives the accumulated root motion
 *     M(t) = G0 * C^k * A * inverse(G0),
 * with C = inverse(G0) * G(b), A = inverse(G0) * G(u),
 * k = floor((t - a) / (b - a)); the formula holds for negative k as well.
 * t == b lands on the next cycle (k = 1, u = a) and stays continuous.
 *
 * Either output pointer may be NULL to skip that output. Returns an error
 * only for a non-finite t or internal corruption; ordinary playback never
 * fails after a successful create.
 */
cgltf_motion_result cgltf_motion_evaluate(
		cgltf_motion_player* player,
		cgltf_float time_seconds,
		cgltf_motion_pose* out_pose,
		cgltf_motion_matrix* out_motion);

/*
 * Motion increment between two absolute times:
 *     delta = inverse(M(from)) * M(to).
 * Chunked products over arbitrary unequal intervals recompose exactly
 * (within floating point): delta(t0,t1) * delta(t1,t2) == delta(t0,t2),
 * and delta(t,t) is identity. Order of queries does not matter.
 */
cgltf_motion_result cgltf_motion_delta(
		cgltf_motion_player* player,
		cgltf_float from_seconds,
		cgltf_float to_seconds,
		cgltf_motion_matrix* out_delta);

/* Free every allocation owned by the player. Safe on NULL. The bound
 * cgltf_data is not freed. */
void cgltf_motion_release(cgltf_motion_player* player);

#ifdef __cplusplus
}
#endif

#endif /* CGLTF_MOTION_H */
