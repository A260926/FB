/* marrow pose-combine SIMD kernels - across-instance blend / accumulate to palette.
 *
 * ONE source. Each ISA translation unit (marrow_blend_{sse2,avx2,avx2_fma}.c) defines its MRW_ISA_*
 * selector and the kernel names MRW_BLEND_KERNEL / MRW_ACCUM_KERNEL, then includes this file. The
 * per-joint pipeline reuses the shared wide helpers (marrow_batch_kernel.h): the SAME keyframe
 * gather, nlerp/lerp interp, TRS→affine, affine compose, and SoA→AoS scatter as the clip kernel -
 * only the per-instance COMBINE (a 2-clip blend, or a shared-delta accumulate) is new, and it is
 * nlerp/lerp/quat-mul (no trig, no pow), so it vectorizes in the same `wide` vocabulary.
 *
 * Inputs are pre-validated by the dispatcher in marrow_blend_batch.c; `model` is the validated SoA
 * scratch. Result is bit-for-bit-close to the scalar reference (FMA fusion / reassociation only). */
#include "marrow_batch_kernel.h"

#if !defined(MRW_BLEND_KERNEL) || !defined(MRW_ACCUM_KERNEL)
#  error "define MRW_BLEND_KERNEL and MRW_ACCUM_KERNEL before including marrow_blend_simd.h"
#endif

/* The per-tile sampling context (mrw_samp / mrw_fill_frames / mrw_blend_sample /
 * mrw_samp_init_const) and the compose→palette seam (mrw_compose_scatter / mrw_drain_soa) live in
 * marrow_batch_kernel.h - ONE source shared with the model-space mesh-blend kernel. */

/* ------------------------------------------------------------------ blend kernel */

MRW_DECL_BLEND_KERNEL(MRW_BLEND_KERNEL) {
    const uint32_t jc  = skel->joint_count;
    const uint32_t SUB = MRW_LANES / MRW_W;
    mrw_samp cxA, cxB; mrw_samp_init_const(&cxA, clipA); mrw_samp_init_const(&cxB, clipB);
    uint32_t i0A[MRW_LANES], i0B[MRW_LANES]; float uuA[MRW_LANES], uuB[MRW_LANES], wbuf[MRW_LANES];
    cxA.i0 = i0A; cxA.uu = uuA; cxB.i0 = i0B; cxB.uu = uuB;

    for (uint32_t base = 0; base < instance_count; ) {
        uint32_t live = instance_count - base; if (live > MRW_LANES) live = MRW_LANES;
        mrw_fill_frames(clipA, timesA, base, live, i0A, uuA, &cxA.st);
        mrw_fill_frames(clipB, timesB, base, live, i0B, uuB, &cxB.st);
        for (uint32_t l = 0; l < MRW_LANES; ++l) wbuf[l] = weights[base + (l < live ? l : 0)];
#if defined(MRW_ISA_AVX2) && !defined(MRW_AVX2_GATHER_SCALAR)
        __m256i gA = mrw_samp_gidx(&cxA), gB = mrw_samp_gidx(&cxB);   /* per-tile, hoisted out of j */
#endif
        for (uint32_t j = 0; j < jc; ++j) {
            int is_root = (j == 0);
            uint16_t parent = is_root ? 0
                : mrw_rd_u16(skel->base + skel->parent_off + (size_t)j * MRW_PARENT_STRIDE);
            float ib[12];
            memcpy(ib, skel->base + skel->inverse_bind_off + (size_t)j * MRW_INVERSE_BIND_STRIDE,
                   MRW_INVERSE_BIND_STRIDE);
            mrw_w ibw[12]; for (int c = 0; c < 12; ++c) ibw[c] = mrw_w_set1(ib[c]);
            mrw_w maskj = mask ? mrw_w_set1(mrw_clampf(mask[j], 0.0f, 1.0f)) : mrw_w_set1(1.0f);
#if !defined(MRW_ISA_AVX2)
            _Alignas(MRW_W_ALIGN) float pal_soa[MRW_PALETTE_FLOATS * MRW_LANES];
#endif
            for (uint32_t g = 0; g < SUB; ++g) {
                uint32_t L = g * MRW_W;
                mrw_w qAx,qAy,qAz,qAw, tA[3], sA[3];
                mrw_w qBx,qBy,qBz,qBw, tB[3], sB[3];
                mrw_blend_sample(&cxA MRW_GIDX_ARG(gA), j, L, &qAx,&qAy,&qAz,&qAw, tA, sA);
                mrw_blend_sample(&cxB MRW_GIDX_ARG(gB), j, L, &qBx,&qBy,&qBz,&qBw, tB, sB);
                /* we = clamp(clamp(w,0,1)·clamp(mask,0,1), 0,1) */
                mrw_w we = mrw_w_clamp01(mrw_w_mul(mrw_w_clamp01(mrw_w_loadu(&wbuf[L])), maskj));
                mrw_w qx,qy,qz,qw;
                mrw_quat_nlerp_w(qAx,qAy,qAz,qAw, qBx,qBy,qBz,qBw, we, &qx,&qy,&qz,&qw);
                mrw_w tx = mrw_w_fma(we, mrw_w_sub(tB[0],tA[0]), tA[0]);
                mrw_w ty = mrw_w_fma(we, mrw_w_sub(tB[1],tA[1]), tA[1]);
                mrw_w tz = mrw_w_fma(we, mrw_w_sub(tB[2],tA[2]), tA[2]);
                mrw_w sx = mrw_w_fma(we, mrw_w_sub(sB[0],sA[0]), sA[0]);
                mrw_w sy = mrw_w_fma(we, mrw_w_sub(sB[1],sA[1]), sA[1]);
                mrw_w sz = mrw_w_fma(we, mrw_w_sub(sB[2],sA[2]), sA[2]);
                mrw_w la[12]; mrw_trs_to_affine_w(qx,qy,qz,qw, tx,ty,tz, sx,sy,sz, la);
                mrw_compose_scatter(la, model, ibw, is_root, parent, L, j, jc, base, live,
                                    out_palettes, out_format, use_f16c
#if !defined(MRW_ISA_AVX2)
                                    , pal_soa
#endif
                                    );
            }
            mrw_drain_soa(
#if !defined(MRW_ISA_AVX2)
                pal_soa,
#else
                (const float *)0,
#endif
                out_palettes, out_format, j, jc, base, live);
        }
        base += live;
    }
    return MRW_OK;
}

/* ------------------------------------------------------------------ accumulate kernel */

MRW_DECL_ACCUM_KERNEL(MRW_ACCUM_KERNEL) {
    const uint32_t jc  = skel->joint_count;
    const uint32_t SUB = MRW_LANES / MRW_W;
    mrw_samp cx; mrw_samp_init_const(&cx, base_clip);
    uint32_t i0[MRW_LANES]; float uu[MRW_LANES], wbuf[MRW_LANES];
    cx.i0 = i0; cx.uu = uu;
    const mrw_w qiw = mrw_w_set1(1.0f), z = mrw_w_zero();

    for (uint32_t base = 0; base < instance_count; ) {
        uint32_t live = instance_count - base; if (live > MRW_LANES) live = MRW_LANES;
        mrw_fill_frames(base_clip, times, base, live, i0, uu, &cx.st);
        for (uint32_t l = 0; l < MRW_LANES; ++l) wbuf[l] = weights[base + (l < live ? l : 0)];
#if defined(MRW_ISA_AVX2) && !defined(MRW_AVX2_GATHER_SCALAR)
        __m256i gidx = mrw_samp_gidx(&cx);   /* per-tile, hoisted out of j */
#endif
        for (uint32_t j = 0; j < jc; ++j) {
            int is_root = (j == 0);
            uint16_t parent = is_root ? 0
                : mrw_rd_u16(skel->base + skel->parent_off + (size_t)j * MRW_PARENT_STRIDE);
            float ib[12];
            memcpy(ib, skel->base + skel->inverse_bind_off + (size_t)j * MRW_INVERSE_BIND_STRIDE,
                   MRW_INVERSE_BIND_STRIDE);
            mrw_w ibw[12]; for (int c = 0; c < 12; ++c) ibw[c] = mrw_w_set1(ib[c]);
            mrw_w maskj = mask ? mrw_w_set1(mrw_clampf(mask[j], 0.0f, 1.0f)) : mrw_w_set1(1.0f);
            /* shared delta (same for every lane) broadcast once per joint */
            mrw_w dqx=mrw_w_set1(delta[j].rot[0]), dqy=mrw_w_set1(delta[j].rot[1]),
                  dqz=mrw_w_set1(delta[j].rot[2]), dqw=mrw_w_set1(delta[j].rot[3]);
            mrw_w dtx=mrw_w_set1(delta[j].trans[0]), dty=mrw_w_set1(delta[j].trans[1]), dtz=mrw_w_set1(delta[j].trans[2]);
            mrw_w dsx=mrw_w_set1(delta[j].scale[0]), dsy=mrw_w_set1(delta[j].scale[1]), dsz=mrw_w_set1(delta[j].scale[2]);
#if !defined(MRW_ISA_AVX2)
            _Alignas(MRW_W_ALIGN) float pal_soa[MRW_PALETTE_FLOATS * MRW_LANES];
#endif
            for (uint32_t g = 0; g < SUB; ++g) {
                uint32_t L = g * MRW_W;
                mrw_w qbx,qby,qbz,qbw, tb[3], sb[3];
                mrw_blend_sample(&cx MRW_GIDX_ARG(gidx), j, L, &qbx,&qby,&qbz,&qbw, tb, sb);
                mrw_w we = mrw_w_clamp01(mrw_w_mul(mrw_w_clamp01(mrw_w_loadu(&wbuf[L])), maskj));
                /* nlerp_id(delta.rot, we) = nlerp(identity, delta.rot, we); rot = normalize(base ⊗ nid) */
                mrw_w nx,ny,nz,nw;
                mrw_quat_nlerp_w(z,z,z,qiw, dqx,dqy,dqz,dqw, we, &nx,&ny,&nz,&nw);
                mrw_w rx,ry,rz,rw;
                mrw_quat_mul_w(qbx,qby,qbz,qbw, nx,ny,nz,nw, &rx,&ry,&rz,&rw);
                mrw_quat_normalize_w(&rx,&ry,&rz,&rw);
                /* trans = base + we·delta.trans; scale = base + we·(base·delta.scale − base) */
                mrw_w tx = mrw_w_fma(we, dtx, tb[0]);
                mrw_w ty = mrw_w_fma(we, dty, tb[1]);
                mrw_w tz = mrw_w_fma(we, dtz, tb[2]);
                mrw_w sx = mrw_w_fma(we, mrw_w_sub(mrw_w_mul(sb[0],dsx), sb[0]), sb[0]);
                mrw_w sy = mrw_w_fma(we, mrw_w_sub(mrw_w_mul(sb[1],dsy), sb[1]), sb[1]);
                mrw_w sz = mrw_w_fma(we, mrw_w_sub(mrw_w_mul(sb[2],dsz), sb[2]), sb[2]);
                mrw_w la[12]; mrw_trs_to_affine_w(rx,ry,rz,rw, tx,ty,tz, sx,sy,sz, la);
                mrw_compose_scatter(la, model, ibw, is_root, parent, L, j, jc, base, live,
                                    out_palettes, out_format, use_f16c
#if !defined(MRW_ISA_AVX2)
                                    , pal_soa
#endif
                                    );
            }
            mrw_drain_soa(
#if !defined(MRW_ISA_AVX2)
                pal_soa,
#else
                (const float *)0,
#endif
                out_palettes, out_format, j, jc, base, live);
        }
        base += live;
    }
    return MRW_OK;
}
