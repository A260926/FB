// SPDX-License-Identifier: Zlib
// SPDX-FileCopyrightText: 2024 dairin0d https://github.com/dairin0d

// Internal interface between the renderer core (discrete_mirage.c)
// and the bounded deep-layer storage (deep_buffer.c). This header
// is not a part of the public API.

#ifndef DMIR_DEEP_INTERNAL_H
#define DMIR_DEEP_INTERNAL_H

#include "discrete_mirage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bounded per-pixel layer storage. All arrays are indexed by the
// same pixel index as the framebuffer's depth/voxel buffers.
// Memory usage is O(pixel_count * capacity).
typedef struct DMirDeepBuffer {
    uint32_t size_x;
    uint32_t size_y;
    uint32_t row_size;
    uint32_t capacity; // max layers kept per pixel (1..32)
    uint32_t* count; // layers currently stored per pixel
    DMirLayer* layers; // pixel-major: pixel * capacity + slot
    DMirDepth* old_depth; // depth buffer snapshot taken at begin
} DMirDeepBuffer;

// Allocates the storage and snapshots the current depth buffer
// (depth_src must point to row_size * size_y depth values).
// The capacity is clamped to [1, DMIR_DEEP_CAPACITY_MAX].
// Returns NULL on allocation failure.
DMirDeepBuffer* dmir_deep_buffer_make(uint32_t size_x, uint32_t size_y,
    uint32_t row_size, uint32_t capacity, const DMirDepth* depth_src);

void dmir_deep_buffer_free(DMirDeepBuffer* deep);

// Records one sample at the given pixel. Samples not strictly
// nearer than the depth snapshot are ignored. A sample with the
// same identity (submission_key + node_box) as an existing layer
// only updates that layer's depth to the minimum. Otherwise the
// material callback is queried, samples with alpha == 0 are
// skipped, and the rest are inserted in near-to-far order
// (ties broken by submission_key, level, x, y, z), dropping the
// farthest layers beyond the capacity.
void dmir_deep_buffer_insert(DMirDeepBuffer* deep, uint32_t pixel_index,
    uint64_t submission_key, const DMirNodeBox* node_box, DMirAddress data_ref,
    DMirDepth depth, DMirMaterialFunc material, void* material_user);

// Read-only compositing, see dmir_deep_composite.
float dmir_deep_buffer_composite(DMirDeepBuffer* deep, uint32_t pixel_index,
    const float* background_rgb, float* out_rgb);

#ifdef __cplusplus
}
#endif

#endif
