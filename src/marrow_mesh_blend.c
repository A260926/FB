/* marrow model-space mesh blend - explicit MODEL-SPACE rotation blend of two same-skeleton local
 * poses, for near-camera characters and same-skeleton crowds.
 *
 * The rest of marrow blends local TRS component-wise (mrw_pose_blend and the batch blend kernels).
 * That is cheap and exactly what a blend tree wants, but it has a geometric blind spot: masking the
 * torso and blending only the arms still lets the PARENT bones' rotations move each arm's direction
 * in model space by different amounts in the two inputs, so the blended arm direction is not the
 * blend of the two model-space directions. This entry fixes that for the cases that care:
 *
 *   QA[j] = model-space unit quat of input A: qA[j] = normalize(qA[parent] ⊗ localA[j].rot)
 *   QB[j] = likewise for input B                     (root: q = normalize(local.rot))
 *   we    = clamp(weight,0,1) · clamp(mask[j],0,1)   (mask NULL ⇒ 1)
 *   QO[j] = hemisphere-corrected nlerp(QA[j], QB[j], we)
 *   out[j].rot = root ? QO[j] : normalize(conj(QO[parent]) ⊗ QO[j])
 *   out[j].trans/scale = lerp(localA, localB, we)    (still LOCAL - not model-space blended)
 *
 * The model-space sweep is independent of the mask; only the nlerp weight sees it. So a mask=0
 * child of a blended parent keeps input A's model-space direction, and its output LOCAL rotation
 * compensates for the changed parent - the whole point of the entry. Rotations are composed as
 * quaternions only: NO rotation is extracted from a scale/shear-bearing affine, and the full local
 * affine (non-uniform scale ⇒ shear under composition) is still what builds the palette.
 *
 * The single-pose entry and the batch kernel's SCALAR reference share the step helpers in
 * marrow_mesh_rot.h (bit-for-bit equal); the SSE2/AVX2(+FMA) kernels in
 * marrow_mesh_blend_{sse2,avx2,avx2_fma}.c vectorize the SAME recurrence across instances.
 * The pre-existing local-space entry points, additive layer and file format are unchanged. */
#include "marrow_internal.h"
#include "marrow_quat.h"        /* mrw_quat_mul / _conj / _normalize / _nlerp */
#include "marrow_mesh_rot.h"    /* the shared per-joint recurrence steps */

/* ------------------------------------------------------------------ overflow-safe sizing */

static int u64_mul(uint64_t a, uint64_t b, uint64_t *p) {
    if (b != 0 && a > UINT64_MAX / b) return 0;
    *p = a * b;
    return 1;
}
static int u64_round_up_64(uint64_t v, uint64_t *r) {
    if (v > UINT64_MAX - 63u) return 0;
    *r = (v + 63u) & ~(uint64_t)63u;
    return 1;
}
static int u64_to_size(uint64_t v, size_t *out) {
    if (v > (uint64_t)SIZE_MAX) return 0;
    *out = (size_t)v;
    return 1;
}

/* Rotation state for one pose / one lane set: joint_count × 12 floats (QA, QB, QO per joint),
 * cache-line rounded. */
static mrw_result mesh_rot_bytes(uint32_t joint_count, size_t *out) {
    uint64_t v;
    if (!u64_mul(joint_count, MRW_MESH_ROT_FLOATS, &v)) return MRW_E_OVERFLOW;
    if (!u64_mul(v, sizeof(float), &v))                return MRW_E_OVERFLOW;
    if (!u64_round_up_64(v, &v))                       return MRW_E_OVERFLOW;
    if (!u64_to_size(v, out))                          return MRW_E_OVERFLOW;
    return MRW_OK;
}

/* Batch scratch: the across-instance SoA model scratch (joint_count × 12 × MRW_LANES floats) plus
 * the across-instance rotation state (joint_count × 12 × MRW_LANES floats). The mock model block is
 * a multiple of 64 bytes, so the rotation block that follows is 64-aligned too. */
static mrw_result mesh_batch_scratch_bytes(uint32_t joint_count, size_t *out) {
    uint64_t one;
    if (!u64_mul(joint_count, MRW_PALETTE_FLOATS, &one)) return MRW_E_OVERFLOW;
    if (!u64_mul(one, MRW_LANES, &one))                  return MRW_E_OVERFLOW;
    if (!u64_mul(one, sizeof(float), &one))              return MRW_E_OVERFLOW; /* model block */
    uint64_t rot;
    if (!u64_mul(joint_count, MRW_MESH_ROT_FLOATS, &rot)) return MRW_E_OVERFLOW;
    if (!u64_mul(rot, MRW_LANES, &rot))                    return MRW_E_OVERFLOW;
    if (!u64_mul(rot, sizeof(float), &rot))                return MRW_E_OVERFLOW;
    if (one > UINT64_MAX - rot) return MRW_E_OVERFLOW;
    uint64_t total = one + rot;
    if (!u64_round_up_64(total, &total)) return MRW_E_OVERFLOW;
    if (!u64_to_size(total, out))        return MRW_E_OVERFLOW;
    return MRW_OK;
}

/* palette output size: instance_count × joint_count × 12 × elem; 0 elem ⇒ range error. */
static mrw_result mesh_out_bytes(uint32_t joint_count, uint32_t instance_count, uint32_t elem,
                                 size_t *out) {
    uint64_t v;
    if (!u64_mul(instance_count, joint_count, &v)) return MRW_E_OVERFLOW;
    if (!u64_mul(v, MRW_PALETTE_FLOATS, &v))       return MRW_E_OVERFLOW;
    if (!u64_mul(v, elem, &v))                     return MRW_E_OVERFLOW;
    if (!u64_to_size(v, out))                      return MRW_E_OVERFLOW;
    return MRW_OK;
}

static uint32_t mesh_elem(mrw_palette_format f) {
    return f == MRW_PALETTE_F32 ? 4u : f == MRW_PALETTE_F16 ? 2u : 0u;
}

/* ------------------------------------------------------------------ finite checks */

static int trs_finite(const mrw_trs *t) {
    for (int k = 0; k < 4; ++k) if (!mrw_f32_finite(t->rot[k]))   return 0;
    for (int k = 0; k < 3; ++k) if (!mrw_f32_finite(t->trans[k])) return 0;
    for (int k = 0; k < 3; ++k) if (!mrw_f32_finite(t->scale[k])) return 0;
    return 1;
}
static int trs_array_finite(const mrw_trs *a, uint32_t n) {
    for (uint32_t j = 0; j < n; ++j) if (!trs_finite(&a[j])) return 0;
    return 1;
}
static int floats_finite(const float *a, uint32_t n) {
    for (uint32_t j = 0; j < n; ++j) if (!mrw_f32_finite(a[j])) return 0;
    return 1;
}

/* Effective per-joint weight: clamp weight and mask[j] individually, then their product, exactly as
 * the local-pose blend layer does. */
static inline float eff_weight(float wc, const float *mask, uint32_t j) {
    float mj = mask ? mrw_clampf(mask[j], 0.0f, 1.0f) : 1.0f;
    return mrw_clampf(wc * mj, 0.0f, 1.0f);
}

/* ------------------------------------------------------------------ single pose */

mrw_result mrw_pose_mesh_blend_requirements(uint32_t joint_count, mrw_mem_req *out_scratch) {
    if (!out_scratch) return MRW_E_RANGE;
    size_t bytes = 0;
    mrw_result rc = mesh_rot_bytes(joint_count, &bytes);
    if (rc) return rc;
    out_scratch->size  = bytes;
    out_scratch->align = MRW_ALIGN_SECTION;
    return MRW_OK;
}

mrw_result mrw_pose_mesh_blend(const mrw_skeleton_view *skel,
                               const mrw_trs *a, const mrw_trs *b, float weight,
                               const float *mask /*nullable, joint_count*/,
                               mrw_trs *out, uint32_t joint_count, uint32_t joint_capacity,
                               void *scratch, size_t scratch_capacity) {
    if (joint_count == 0) return MRW_OK;                   /* no-op (data pointers may be NULL) */
    if (!skel || !a || !b || !out) return MRW_E_RANGE;
    if (joint_count != skel->joint_count) return MRW_E_INCOMPATIBLE;
    if (joint_count > joint_capacity) return MRW_E_CAPACITY;
    if (!mrw_f32_finite(weight)) return MRW_E_RANGE;
    /* every finite check happens before any output is touched */
    if (!trs_array_finite(a, joint_count)) return MRW_E_RANGE;
    if (!trs_array_finite(b, joint_count)) return MRW_E_RANGE;
    if (mask && !floats_finite(mask, joint_count)) return MRW_E_RANGE;

    size_t need = 0;
    mrw_result rc = mesh_rot_bytes(joint_count, &need);
    if (rc) return rc;
    if (!scratch) return MRW_E_RANGE;
    if (((uintptr_t)scratch & (MRW_ALIGN_SECTION - 1u)) != 0) return MRW_E_ALIGN;
    if (scratch_capacity < need) return MRW_E_CAPACITY;

    float *rot = (float *)scratch;   /* joint_count × 12: QA, QB, QO per joint, locals only */
    float wc = mrw_clampf(weight, 0.0f, 1.0f);
    for (uint32_t j = 0; j < joint_count; ++j) {
        int is_root = (j == 0);
        uint16_t parent = 0;
        if (!is_root) mrw_skeleton_parent(skel, j, &parent);   /* parent < j (validated) */

        const float *qa_parent = is_root ? a[j].rot
            : rot + MRW_MESH_ROT_POS(parent, 0u, 0u);
        const float *qb_parent = is_root ? b[j].rot
            : rot + MRW_MESH_ROT_POS(parent, 1u, 0u);
        float qa[4], qb[4], qo[4];
        mrw_mesh_model_rot_step(qa_parent, a[j].rot, is_root, qa);
        mrw_mesh_model_rot_step(qb_parent, b[j].rot, is_root, qb);
        memcpy(rot + MRW_MESH_ROT_POS(j, 0u, 0u), qa, sizeof qa);
        memcpy(rot + MRW_MESH_ROT_POS(j, 1u, 0u), qb, sizeof qb);

        float we = eff_weight(wc, mask, j);
        mrw_mesh_blend_rot_step(qa, qb, we, qo);
        memcpy(rot + MRW_MESH_ROT_POS(j, 2u, 0u), qo, sizeof qo);

        float lr[4];
        if (is_root) {
            memcpy(lr, qo, sizeof lr);
        } else {
            mrw_mesh_local_rot_step(rot + MRW_MESH_ROT_POS(parent, 2u, 0u), qo, 0, lr);
        }
        /* trans/scale stay LOCAL: plain lerp at the same effective weight. All a[j]/b[j] components
         * are read here, before out[j] is written, so out MAY exactly alias either input. */
        float ot[3], os[3];
        for (int k = 0; k < 3; ++k) ot[k] = a[j].trans[k] + we * (b[j].trans[k] - a[j].trans[k]);
        for (int k = 0; k < 3; ++k) os[k] = a[j].scale[k] + we * (b[j].scale[k] - a[j].scale[k]);
        for (int k = 0; k < 4; ++k) out[j].rot[k]   = lr[k];
        for (int k = 0; k < 3; ++k) out[j].trans[k] = ot[k];
        for (int k = 0; k < 3; ++k) out[j].scale[k] = os[k];
    }
    return MRW_OK;
}

/* ------------------------------------------------------------------ batch requirements */

mrw_result mrw_batch_mesh_blend_clips_to_palette_requirements(
    uint32_t joint_count, uint32_t instance_count, mrw_palette_format out_format,
    mrw_mem_req *out_scratch, mrw_mem_req *out_palettes) {
    if (!out_scratch && !out_palettes) return MRW_E_RANGE;
    uint32_t elem = mesh_elem(out_format);
    if (elem == 0) return MRW_E_RANGE;
    /* compute BOTH sizes first so a failure writes NEITHER out-param */
    size_t sbytes = 0, obytes = 0;
    mrw_result rc = mesh_batch_scratch_bytes(joint_count, &sbytes);
    if (rc) return rc;
    rc = mesh_out_bytes(joint_count, instance_count, elem, &obytes);
    if (rc) return rc;
    if (out_scratch)  { out_scratch->size  = sbytes; out_scratch->align  = MRW_ALIGN_SECTION; }
    if (out_palettes) { out_palettes->size = obytes; out_palettes->align = MRW_ALIGN_ARRAY;   }
    return MRW_OK;
}

/* ------------------------------------------------------------------ scalar batch reference */

/* Sample clip joint j at one lane's frame state (matches mrw_clip_sample_local[j]). */
static void sample_joint(const mrw_clip_view *clip, uint32_t j, int st, uint32_t i0, float u,
                         mrw_trs *lj) {
    if (st) {
        mrw_clip_sample(clip, j, 0, lj);
    } else {
        mrw_trs a, b;
        mrw_clip_sample(clip, j, i0, &a);
        mrw_clip_sample(clip, j, i0 + 1, &b);
        mrw_interp_trs(&a, &b, u, lj);
    }
}

/* Fold model(j) for one lane into the SoA scratch and (for live lanes) scatter the palette entry.
 * `local12` is the combined local affine; identical compose/scatter to the clip batch kernel. */
static void compose_scatter_scalar(float *model, const float ib[12],
                                   const float local12[12], int is_root, uint16_t parent,
                                   uint32_t j, uint32_t jc, uint32_t row, uint32_t l, int live_lane,
                                   void *out_palettes, mrw_palette_format out_format) {
    float mjm[12];
    if (is_root) {
        memcpy(mjm, local12, sizeof mjm);
    } else {
        float pm[12];
        for (uint32_t c = 0; c < MRW_PALETTE_FLOATS; ++c) pm[c] = MRW_MODEL_AT(model, parent, c, l);
        mrw_affine_mul(pm, local12, mjm);
    }
    for (uint32_t c = 0; c < MRW_PALETTE_FLOATS; ++c) MRW_MODEL_AT(model, j, c, l) = mjm[c];
    if (!live_lane) return;
    float M[12]; mrw_affine_mul(mjm, ib, M);
    size_t off = ((size_t)row * jc + j) * MRW_PALETTE_FLOATS;
    if (out_format == MRW_PALETTE_F16) {
        uint16_t *dst = (uint16_t *)out_palettes + off;
        for (uint32_t c = 0; c < MRW_PALETTE_FLOATS; ++c) dst[c] = mrw_f32_to_f16(M[c]);
    } else {
        memcpy((float *)out_palettes + off, M, sizeof M);
    }
}

/* Scalar reference: same per-joint recurrence as mrw_pose_mesh_blend, then the clip batch's
 * compose → inverse-bind → palette. Bit-for-bit equal to looping the single-pose entry and then
 * mrw_local_to_model/mrw_model_to_palette. `model`/`rot` are the validated SoA scratch blocks. */
static mrw_result mesh_blend_scalar(const mrw_skeleton_view *skel,
                                    const mrw_clip_view *clipA, const mrw_clip_view *clipB,
                                    const float *timesA, const float *timesB, const float *weights,
                                    const float *mask, uint32_t instance_count,
                                    void *out_palettes, mrw_palette_format out_format,
                                    float *model, float *rot) {
    uint32_t jc = skel->joint_count;
    for (uint32_t base = 0; base < instance_count; ) {
        uint32_t live = instance_count - base; if (live > MRW_LANES) live = MRW_LANES;
        uint32_t i0A[MRW_LANES], i0B[MRW_LANES]; float uA[MRW_LANES], uB[MRW_LANES];
        int stA[MRW_LANES], stB[MRW_LANES];
        for (uint32_t l = 0; l < MRW_LANES; ++l) {
            uint32_t idx = base + (l < live ? l : 0);
            stA[l] = mrw_clip_frame(clipA, timesA[idx], &i0A[l], &uA[l]);
            stB[l] = mrw_clip_frame(clipB, timesB[idx], &i0B[l], &uB[l]);
        }
        for (uint32_t j = 0; j < jc; ++j) {
            int is_root = (j == 0);
            uint16_t parent = 0;
            if (!is_root) mrw_skeleton_parent(skel, j, &parent);
            float ib[12]; mrw_skeleton_inverse_bind(skel, j, ib);
            for (uint32_t l = 0; l < MRW_LANES; ++l) {
                uint32_t idx = base + (l < live ? l : 0);
                mrw_trs la, lb;
                sample_joint(clipA, j, stA[l], i0A[l], uA[l], &la);
                sample_joint(clipB, j, stB[l], i0B[l], uB[l], &lb);
                float wc = mrw_clampf(weights[idx], 0.0f, 1.0f);
                float mj = mask ? mrw_clampf(mask[j], 0.0f, 1.0f) : 1.0f;
                float we = mrw_clampf(wc * mj, 0.0f, 1.0f);

                /* the SoA rotation state is lane-innermost: address through MRW_MESH_ROT_AT */
                float qa[4], qb[4], qo[4];
                float pa[4], pb[4];
                if (is_root) {
                    memcpy(pa, la.rot, sizeof pa);
                    memcpy(pb, lb.rot, sizeof pb);
                } else {
                    for (int c = 0; c < 4; ++c) {
                        pa[c] = MRW_MESH_ROT_AT(rot, parent, 0u, (uint32_t)c, l);
                        pb[c] = MRW_MESH_ROT_AT(rot, parent, 1u, (uint32_t)c, l);
                    }
                }                mrw_mesh_model_rot_step(pa, la.rot, is_root, qa);
                mrw_mesh_model_rot_step(pb, lb.rot, is_root, qb);
                for (int c = 0; c < 4; ++c) {
                    MRW_MESH_ROT_AT(rot, j, 0u, (uint32_t)c, l) = qa[c];
                    MRW_MESH_ROT_AT(rot, j, 1u, (uint32_t)c, l) = qb[c];
                }
                mrw_mesh_blend_rot_step(qa, qb, we, qo);
                for (int c = 0; c < 4; ++c) MRW_MESH_ROT_AT(rot, j, 2u, (uint32_t)c, l) = qo[c];

                float lr[4];
                if (is_root) {
                    memcpy(lr, qo, sizeof lr);
                } else {
                    float qop[4];
                    for (int c = 0; c < 4; ++c) qop[c] = MRW_MESH_ROT_AT(rot, parent, 2u, (uint32_t)c, l);
                    mrw_mesh_local_rot_step(qop, qo, 0, lr);
                }
                mrw_trs blended;
                memcpy(blended.rot, lr, sizeof lr);
                for (int k = 0; k < 3; ++k)
                    blended.trans[k] = la.trans[k] + we * (lb.trans[k] - la.trans[k]);
                for (int k = 0; k < 3; ++k)
                    blended.scale[k] = la.scale[k] + we * (lb.scale[k] - la.scale[k]);
                float local12[12]; mrw_trs_to_affine(&blended, local12);
                compose_scatter_scalar(model, ib, local12, is_root, parent,
                                       j, jc, base + l, l, l < live, out_palettes, out_format);
            }
        }
        base += live;
    }
    return MRW_OK;
}

/* ------------------------------------------------------------------ validate + dispatch */

static mrw_result mesh_prep(const mrw_dispatch *disp, const mrw_skeleton_view *skel,
                            uint32_t instance_count, mrw_palette_format out_format,
                            const void *out_palettes, size_t out_palettes_capacity,
                            void *scratch, size_t scratch_capacity,
                            float **model, float **rot, int *use_f16c, int *proceed) {
    *proceed = 0;
    mrw_result drc = mrw_batch_validate_dispatch(disp);
    if (drc) return drc;
    uint32_t elem = mesh_elem(out_format);
    if (elem == 0) return MRW_E_RANGE;
    if (instance_count == 0) return MRW_OK;                       /* clean no-op */
    if (!out_palettes || !scratch) return MRW_E_RANGE;

    uint32_t jc = skel->joint_count;
    mrw_mem_req sreq, oreq;
    mrw_result rc = mrw_batch_mesh_blend_clips_to_palette_requirements(jc, instance_count,
                                                                      out_format, &sreq, &oreq);
    if (rc) return rc;
    size_t out_align = (out_format == MRW_PALETTE_F16) ? elem : MRW_ALIGN_ARRAY;
    if (((uintptr_t)scratch      & (MRW_ALIGN_SECTION - 1u)) != 0) return MRW_E_ALIGN;
    if (((uintptr_t)out_palettes & (out_align         - 1u)) != 0) return MRW_E_ALIGN;
    if (scratch_capacity      < sreq.size) return MRW_E_CAPACITY;
    if (out_palettes_capacity < oreq.size) return MRW_E_CAPACITY;

    /* model block first, then the rotation block; the model block is a multiple of 64 bytes. */
    float *m = (float *)scratch;
    size_t model_floats = (size_t)jc * MRW_PALETTE_FLOATS * MRW_LANES;
    *model = m;
    *rot   = m + model_floats;
    *use_f16c = (disp->features & MRW_FEAT_F16C) != 0;
    *proceed = 1;
    return MRW_OK;
}

static mrw_result mesh_blend_impl(const mrw_dispatch *disp, const mrw_skeleton_view *skel,
                                  const mrw_clip_view *clipA, const mrw_clip_view *clipB,
                                  const float *timesA, const float *timesB, const float *weights,
                                  const float *mask, uint32_t instance_count,
                                  mrw_palette_format out_format,
                                  void *out_palettes, size_t out_palettes_capacity,
                                  void *scratch, size_t scratch_capacity) {
    if (!disp || !skel || !clipA || !clipB) return MRW_E_RANGE;
    uint32_t jc = skel->joint_count;
    if (jc != clipA->joint_count || jc != clipB->joint_count) return MRW_E_INCOMPATIBLE;
    if (!mrw_id_equal(&clipA->skeleton_id, &skel->id) ||
        !mrw_id_equal(&clipB->skeleton_id, &skel->id)) return MRW_E_INCOMPATIBLE;

    float *model = NULL, *rot = NULL; int use_f16c = 0, proceed = 0;
    mrw_result rc = mesh_prep(disp, skel, instance_count, out_format,
                              out_palettes, out_palettes_capacity, scratch, scratch_capacity,
                              &model, &rot, &use_f16c, &proceed);
    if (rc || !proceed) return rc;
    if (!timesA || !timesB || !weights) return MRW_E_RANGE;

    for (uint32_t i = 0; i < instance_count; ++i)
        if (!mrw_f32_finite(timesA[i]) || !mrw_f32_finite(timesB[i]) || !mrw_f32_finite(weights[i]))
            return MRW_E_RANGE;
    if (mask) for (uint32_t j = 0; j < jc; ++j) if (!mrw_f32_finite(mask[j])) return MRW_E_RANGE;

    switch (disp->backend) {
        case MRW_BACKEND_SSE2:
            return mrw_mesh_blend_kernel_sse2(skel, clipA, clipB, timesA, timesB, weights, mask,
                                              instance_count, out_palettes, out_format, use_f16c,
                                              model, rot);
        case MRW_BACKEND_AVX2:
            return (disp->features & MRW_FEAT_FMA)
                 ? mrw_mesh_blend_kernel_avx2_fma(skel, clipA, clipB, timesA, timesB, weights, mask,
                                                  instance_count, out_palettes, out_format, use_f16c,
                                                  model, rot)
                 : mrw_mesh_blend_kernel_avx2    (skel, clipA, clipB, timesA, timesB, weights, mask,
                                                  instance_count, out_palettes, out_format, use_f16c,
                                                  model, rot);
        default:
            return mesh_blend_scalar(skel, clipA, clipB, timesA, timesB, weights, mask,
                                     instance_count, out_palettes, out_format, model, rot);
    }
}

/* ------------------------------------------------------------------ public entry points */

mrw_result mrw_batch_mesh_blend_clips_to_palette(
    const mrw_dispatch *disp, const mrw_skeleton_view *skel,
    const mrw_clip_view *clipA, const mrw_clip_view *clipB,
    const float *timesA, const float *timesB, const float *weights,
    const float *mask /*joint_count, or NULL*/, uint32_t instance_count,
    float *out_palettes, size_t out_palettes_capacity,
    void  *scratch,      size_t scratch_capacity) {
    return mesh_blend_impl(disp, skel, clipA, clipB, timesA, timesB, weights, mask, instance_count,
                           MRW_PALETTE_F32, out_palettes, out_palettes_capacity,
                           scratch, scratch_capacity);
}

mrw_result mrw_batch_mesh_blend_clips_to_palette_f16(
    const mrw_dispatch *disp, const mrw_skeleton_view *skel,
    const mrw_clip_view *clipA, const mrw_clip_view *clipB,
    const float *timesA, const float *timesB, const float *weights,
    const float *mask /*joint_count, or NULL*/, uint32_t instance_count,
    uint16_t *out_palettes, size_t out_palettes_capacity,
    void     *scratch,      size_t scratch_capacity) {
    return mesh_blend_impl(disp, skel, clipA, clipB, timesA, timesB, weights, mask, instance_count,
                           MRW_PALETTE_F16, out_palettes, out_palettes_capacity,
                           scratch, scratch_capacity);
}
