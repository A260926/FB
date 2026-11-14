/*
 * cgltf_retime - time-warp an animation inside a cgltf asset.
 *
 * This is an optional add-on module for cgltf.  It has no implementation
 * requirements of its own: link it in the same way as cgltf_write.h (the
 * CGLTF_IMPLEMENTATION / CGLTF_WRITE_IMPLEMENTATION definitions must live in
 * exactly one other translation unit).
 *
 * A call takes a piecewise-linear time mapping defined by two equally sized
 * knot arrays.  `source_times` selects a span of the source animation;
 * `output_times` is the corresponding span in the retimed animation.  Both
 * arrays must be finite and strictly increasing, `count` must be at least two
 * and `output_times[0]` must be zero.  Interior knots are speed-change
 * boundaries of the mapping.
 *
 * The returned asset is fully independent of `source` (including all buffer
 * bytes, which are stored as embedded data URIs) and must be released with
 * `cgltf_free`.  `source` is not modified.
 */
#ifndef CGLTF_RETIME_H_INCLUDED__
#define CGLTF_RETIME_H_INCLUDED__

#include "cgltf.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Rewrite one animation of `source` with the piecewise-linear time mapping
 * described by `source_times` / `output_times` and store a newly allocated
 * asset in `*result`.
 *
 * On failure `*result` is set to NULL.  Returned errors include:
 *   cgltf_result_invalid_options - bad arguments, invalid knot arrays, source
 *                                  times outside the animation's time range,
 *                                  or an inserted knot inside a cubic-spline
 *                                  rotation segment that is not an existing key;
 *   cgltf_result_invalid_gltf    - malformed channel target, key sequence or
 *                                  output shape, or duplicate target attributes;
 *   cgltf_result_data_too_short  - accessor data cannot be read (buffers are
 *                                  not loaded, or accessor ranges overflow);
 *   cgltf_result_unknown_format  - the source uses a compressed buffer or mesh
 *                                  extension;
 *   cgltf_result_out_of_memory   - allocation failure.
 */
cgltf_result cgltf_retime_animation(
		const cgltf_data* source,
		cgltf_size animation_index,
		const cgltf_float* source_times,
		const cgltf_float* output_times,
		cgltf_size count,
		cgltf_data** result);

#ifdef __cplusplus
}
#endif

#endif /* CGLTF_RETIME_H_INCLUDED__ */
