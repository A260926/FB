# cgltf_motion — windowless animation playback and root motion

`cgltf_motion.h` / `cgltf_motion.c` add a windowless glTF 2.0 animation
player and a looping root-motion extractor on top of `cgltf`. They only
**read** the parsed `cgltf_data`: no sampled value is ever written back
into a `cgltf_node`, and accessor/buffer memory is never modified.

## Minimal call

```c
#define CGLTF_IMPLEMENTATION   /* in exactly one translation unit */
#include "cgltf.h"
#include "cgltf_motion.h"

cgltf_options options = {0};
cgltf_data* data = NULL;
if (cgltf_parse_file(&options, "scene.gltf", &data) != cgltf_result_success) { /* ... */ }
cgltf_load_buffers(&options, data, "scene.gltf");
if (cgltf_validate(data) != cgltf_result_success) { /* ... */ }

cgltf_motion_player* player = NULL;
cgltf_motion_result r = cgltf_motion_create(data,
		/*animation*/ 0, /*extract node*/ 2, &player);
if (r != CGLTF_MOTION_OK) {
	fputs(cgltf_motion_result_string(r), stderr); /* see error enum in the header */
}

/* Absolute seconds; negative, pre-start and multi-loop times are valid. */
cgltf_motion_pose pose;        /* nodes_count * 16 column-major floats   */
cgltf_motion_matrix motion;    /* one 16-float accumulated root matrix   */
cgltf_motion_evaluate(player, 2.5f, &pose, &motion);

/* In-place character pinned to its start pose; multiply the accumulated
 * motion on the left to reconstruct the extended, looping character. */
cgltf_motion_matrix delta;
cgltf_motion_delta(player, 0.0f, 2.5f, &delta); /* inverse(M(from))*M(to) */

cgltf_motion_release(player);  /* frees player storage, not the data     */
cgltf_free(data);
```

Build with no window dependency and nothing beyond libm:

```sh
cc -std=c99 -I. my_app.c examples/cgltf_motion.c -lm
```

## Outputs and ownership

* `pose.world_matrices` is `nodes_count * 16` contiguous column-major
  floats, one 16-float matrix per node in document order. The storage is
  owned by the player and overwritten by that player's next evaluation;
  separate players have independent buffers and may be queried
  interleaved.
* Every routine returns a `cgltf_motion_result`; `CGLTF_MOTION_OK` (0) is
  the only success value. On create failure the player handle is left
  untouched.

## Semantics in one paragraph

The time interval `[a, b]` spans the earliest and latest sampler input.
Samplers clamp outside their own range, uncovered TRS keeps the static
file values, and STEP / LINEAR / CUBICSPLINE are all supported (linear
rotation takes the shortest arc; cubic rotation is the component Hermite
result, normalized, with tangents scaled by the real inter-key seconds).
For an absolute time `t`, `k = floor((t-a)/(b-a))` and the wrapped phase
`u` is in `[a, b)`; the accumulated motion is
`M(t) = G0 * C^k * A * inverse(G0)` with `G0 = G(a)`,
`C = inverse(G0) * G(b)` and `A = inverse(G0) * G(u)`, valid for
negative `k` as well. The in-place pose is
`G0 * inverse(G(u)) * W(u)`. See `test/root_motion_test.c` for numeric
coverage and the rejection paths.
