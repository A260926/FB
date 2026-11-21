// SPDX-License-Identifier: Zlib
// SPDX-FileCopyrightText: 2024 dairin0d https://github.com/dairin0d

// Bounded per-pixel layer storage and compositing for the deep
// (multi-layer translucent) capture mode. Only C99 + stdlib here.

#include <stdlib.h>
#include <string.h>

#include "deep_internal.h"

// Strict ordering of layers within a pixel: near to far, with
// ties broken by submission key and the node box coordinates.
// Since the (submission_key, node_box) identity is unique per
// pixel, this is a total order over the stored layers, which
// makes the K-nearest truncation independent of insertion order.
static int dmir_deep_layer_less(const DMirLayer* a, const DMirLayer* b) {
    if (a->depth != b->depth) return a->depth < b->depth;
    if (a->submission_key != b->submission_key)
        return a->submission_key < b->submission_key;
    if (a->node_box.level != b->node_box.level)
        return a->node_box.level < b->node_box.level;
    if (a->node_box.x != b->node_box.x) return a->node_box.x < b->node_box.x;
    if (a->node_box.y != b->node_box.y) return a->node_box.y < b->node_box.y;
    return a->node_box.z < b->node_box.z;
}

static int dmir_deep_same_sample(const DMirLayer* layer,
    uint64_t submission_key, const DMirNodeBox* node_box)
{
    if (layer->submission_key != submission_key) return 0;
    if (layer->node_box.level != node_box->level) return 0;
    if (layer->node_box.x != node_box->x) return 0;
    if (layer->node_box.y != node_box->y) return 0;
    if (layer->node_box.z != node_box->z) return 0;
    return 1;
}

DMirDeepBuffer* dmir_deep_buffer_make(uint32_t size_x, uint32_t size_y,
    uint32_t row_size, uint32_t capacity, const DMirDepth* depth_src)
{
    if (capacity < 1) capacity = 1;
    if (capacity > (uint32_t)DMIR_DEEP_CAPACITY_MAX) {
        capacity = DMIR_DEEP_CAPACITY_MAX;
    }

    DMirDeepBuffer* deep = calloc(1, sizeof(DMirDeepBuffer));
    if (!deep) return NULL;

    deep->size_x = size_x;
    deep->size_y = size_y;
    deep->row_size = row_size;
    deep->capacity = capacity;

    size_t pixel_count = (size_t)row_size * (size_t)size_y;
    if (pixel_count == 0) pixel_count = 1; // safeguard, same as framebuffer

    deep->count = calloc(pixel_count, sizeof(uint32_t));
    deep->layers = malloc(pixel_count * (size_t)capacity * sizeof(DMirLayer));
    deep->old_depth = malloc(pixel_count * sizeof(DMirDepth));

    if (!deep->count || !deep->layers || !deep->old_depth) {
        dmir_deep_buffer_free(deep);
        return NULL;
    }

    if (depth_src) {
        memcpy(deep->old_depth, depth_src, pixel_count * sizeof(DMirDepth));
    } else {
        for (size_t i = 0; i < pixel_count; i++) {
            deep->old_depth[i] = DMIR_MAX_DEPTH;
        }
    }

    return deep;
}

void dmir_deep_buffer_free(DMirDeepBuffer* deep) {
    if (!deep) return;
    if (deep->count) free(deep->count);
    if (deep->layers) free(deep->layers);
    if (deep->old_depth) free(deep->old_depth);
    free(deep);
}

void dmir_deep_buffer_insert(DMirDeepBuffer* deep, uint32_t pixel_index,
    uint64_t submission_key, const DMirNodeBox* node_box, DMirAddress data_ref,
    DMirDepth depth, DMirMaterialFunc material, void* material_user)
{
    // Only samples strictly nearer than the depth snapshot at the
    // start of the capture are recorded; equal depth is rejected.
    if (!(depth < deep->old_depth[pixel_index])) return;

    DMirLayer* slots = deep->layers + (size_t)pixel_index * deep->capacity;
    uint32_t count = deep->count[pixel_index];

    // The same node repeatedly covering this pixel (e.g. due to
    // internal splitting) counts once, keeping its minimum depth.
    for (uint32_t i = 0; i < count; i++) {
        if (!dmir_deep_same_sample(slots + i, submission_key, node_box)) continue;
        if (depth < slots[i].depth) {
            slots[i].depth = depth;
            // A decreased depth can only move the layer nearer.
            while ((i > 0) && dmir_deep_layer_less(slots + i, slots + i - 1)) {
                DMirLayer tmp = slots[i];
                slots[i] = slots[i - 1];
                slots[i - 1] = tmp;
                i--;
            }
        }
        return;
    }

    float rgba[4];
    material(material_user, data_ref, rgba);

    // Fully transparent samples don't occupy a layer.
    if (!(rgba[3] > 0.0f)) return;

    DMirLayer sample;
    sample.submission_key = submission_key;
    sample.node_box = *node_box;
    sample.data_ref = data_ref;
    sample.depth = depth;
    sample.r = rgba[0];
    sample.g = rgba[1];
    sample.b = rgba[2];
    sample.a = rgba[3];

    uint32_t pos = 0;
    while ((pos < count) && dmir_deep_layer_less(slots + pos, &sample)) pos++;

    // The sample is farther than all the K kept layers: dropped.
    if (pos >= deep->capacity) return;

    uint32_t new_count = (count < deep->capacity) ? count + 1 : count;
    for (uint32_t i = new_count - 1; i > pos; i--) {
        slots[i] = slots[i - 1];
    }
    slots[pos] = sample;
    deep->count[pixel_index] = new_count;
}

float dmir_deep_buffer_composite(DMirDeepBuffer* deep, uint32_t pixel_index,
    const float* background_rgb, float* out_rgb)
{
    const DMirLayer* slots = deep->layers + (size_t)pixel_index * deep->capacity;
    uint32_t count = deep->count[pixel_index];

    float c_r = 0.0f, c_g = 0.0f, c_b = 0.0f;
    float t = 1.0f;
    for (uint32_t i = 0; i < count; i++) {
        float a = slots[i].a;
        c_r += t * a * slots[i].r;
        c_g += t * a * slots[i].g;
        c_b += t * a * slots[i].b;
        t *= 1.0f - a;
    }

    out_rgb[0] = c_r + t * background_rgb[0];
    out_rgb[1] = c_g + t * background_rgb[1];
    out_rgb[2] = c_b + t * background_rgb[2];
    return t;
}
