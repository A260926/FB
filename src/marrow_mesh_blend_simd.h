/* marrow model-space mesh-blend SIMD kernel - across-instance SoA, vertical `wide` math.
 *
 * ONE source. Each ISA translation unit (marrow_mesh_blend_{sse2,avx2,avx2_fma}.c) defines its
 * MRW_ISA_* selector and MRW_MESH_BLEND_KERNEL, then includes this file.
 *
 * The kernel fuses the WHOLE model-space recurrence into the existing cross-instance wide
 * sampling + palette pipeline (marrow_batch_kernel.h): per joint, per tile, it samples both clips
 * wide (mrw_blend_sample), derives QA/QB (parent model-quat ⊗ local, normalized), nlerps them at the
 * per-lane effective weight, restores the local rotation against the OUTPUT parent's model quat, and
 * then runs the SAME TRS→affine → model compose → inverse-bind → palette scatter as the clip kernel.
 * The rotation recurrence is real vector math here (QA/QB/QO are `wide` registers held in the `rot`
 * SoA scratch between joints); there is no per-instance call into the scalar single-pose entry.
 *
 * The rotation scratch is joint_count × 12 × MRW_LANES floats: three model-space quats (QA, QB, QO)
 * per joint, lane innermost so a `wide` load grabs MRW_LANES instances of one component. The work
 * set is O(joint_count · MRW_LANES) - one tile at a time, not three poses per instance.
 *
 * Inputs are pre-validated by the dispatcher in marrow_mesh_blend.c; result is within a small
 * FMA/reassociation tolerance of the scalar reference. */
#include "marrow_batch_kernel.h"

#if !defined(MRW_MESH_BLEND_KERNEL)
#  error "define MRW_MESH_BLEND_KERNEL before including marrow_mesh_blend_simd.h"
#endif

MRW_DECL_MESH_BLEND_KERNEL(MRW_MESH_BLEND_KERNEL) {
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
        __m256i gA = mrw_samp_gidx(&cxA), gB = mrw_samp_gidx(&cxB);   /* per-tile, hoisted */
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
                /* sample both clips wide at joint j */
                mrw_w qAx,qAy,qAz,qAw, tA[3], sA[3];
                mrw_w qBx,qBy,qBz,qBw, tB[3], sB[3];
                mrw_blend_sample(&cxA MRW_GIDX_ARG(gA), j, L, &qAx,&qAy,&qAz,&qAw, tA, sA);
                mrw_blend_sample(&cxB MRW_GIDX_ARG(gB), j, L, &qBx,&qBy,&qBz,&qBw, tB, sB);

                /* QA[j] = normalize(QA[parent] ⊗ localA) (root: normalize(localA));
                 * QB[j] likewise. The parent model quats are loaded wide from the rot scratch. */
                mrw_w pax,pbx; mrw_w pay,pby, paz,pbz, paw,pbw;
                if (is_root) {
                    pax = qAx; pay = qAy; paz = qAz; paw = qAw;
                    pbx = qBx; pby = qBy; pbz = qBz; pbw = qBw;
                    /* root model quat = the local quat normalized */
                    mrw_quat_normalize_w(&pax,&pay,&paz,&paw);
                    mrw_quat_normalize_w(&pbx,&pby,&pbz,&pbw);
                } else {
                    pax = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 0u, 0u, L));
                    pay = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 0u, 1u, L));
                    paz = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 0u, 2u, L));
                    paw = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 0u, 3u, L));
                    pbx = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 1u, 0u, L));
                    pby = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 1u, 1u, L));
                    pbz = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 1u, 2u, L));
                    pbw = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 1u, 3u, L));
                    /* model quat = parent_model ⊗ local */
                    mrw_w ax,ay,az,aw, bx,by,bz,bw;
                    mrw_quat_mul_w(pax,pay,paz,paw, qAx,qAy,qAz,qAw, &ax,&ay,&az,&aw);
                    mrw_quat_mul_w(pbx,pby,pbz,pbw, qBx,qBy,qBz,qBw, &bx,&by,&bz,&bw);
                    pax=ax; pay=ay; paz=az; paw=aw;
                    pbx=bx; pby=by; pbz=bz; pbw=bw;
                    mrw_quat_normalize_w(&pax,&pay,&paz,&paw);
                    mrw_quat_normalize_w(&pbx,&pby,&pbz,&pbw);
                }
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 0u, 0u, L), pax);
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 0u, 1u, L), pay);
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 0u, 2u, L), paz);
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 0u, 3u, L), paw);
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 1u, 0u, L), pbx);
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 1u, 1u, L), pby);
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 1u, 2u, L), pbz);
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 1u, 3u, L), pbw);

                /* we = clamp(clamp(w,0,1)·clamp(mask,0,1), 0,1) */
                mrw_w we = mrw_w_clamp01(mrw_w_mul(mrw_w_clamp01(mrw_w_loadu(&wbuf[L])), maskj));
                /* QO[j] = hemisphere-corrected nlerp(QA,QB,we) */
                mrw_w qox,qoy,qoz,qow;
                mrw_quat_nlerp_w(pax,pay,paz,paw, pbx,pby,pbz,pbw, we, &qox,&qoy,&qoz,&qow);
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 2u, 0u, L), qox);
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 2u, 1u, L), qoy);
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 2u, 2u, L), qoz);
                mrw_w_store(&MRW_MESH_ROT_AT(rot, j, 2u, 3u, L), qow);

                /* localOut = root ? QO : normalize(conj(QO[parent]) ⊗ QO[j]) */
                mrw_w lrx,lry,lrz,lrw;
                if (is_root) {
                    lrx=qox; lry=qoy; lrz=qoz; lrw=qow;
                } else {
                    mrw_w opx = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 2u, 0u, L));
                    mrw_w opy = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 2u, 1u, L));
                    mrw_w opz = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 2u, 2u, L));
                    mrw_w opw = mrw_w_load(&MRW_MESH_ROT_AT(rot, parent, 2u, 3u, L));
                    /* conj(QO[parent]) = (-x,-y,-z,w); mul then renormalize */
                    mrw_w cx = mrw_w_xor(opx, mrw_w_set1(-0.0f));
                    mrw_w cy = mrw_w_xor(opy, mrw_w_set1(-0.0f));
                    mrw_w cz = mrw_w_xor(opz, mrw_w_set1(-0.0f));
                    mrw_quat_mul_w(cx,cy,cz,opw, qox,qoy,qoz,qow, &lrx,&lry,&lrz,&lrw);
                    mrw_quat_normalize_w(&lrx,&lry,&lrz,&lrw);
                }

                /* trans/scale stay LOCAL: plain lerp at we. */
                mrw_w tx = mrw_w_fma(we, mrw_w_sub(tB[0],tA[0]), tA[0]);
                mrw_w ty = mrw_w_fma(we, mrw_w_sub(tB[1],tA[1]), tA[1]);
                mrw_w tz = mrw_w_fma(we, mrw_w_sub(tB[2],tA[2]), tA[2]);
                mrw_w sx = mrw_w_fma(we, mrw_w_sub(sB[0],sA[0]), sA[0]);
                mrw_w sy = mrw_w_fma(we, mrw_w_sub(sB[1],sA[1]), sA[1]);
                mrw_w sz = mrw_w_fma(we, mrw_w_sub(sB[2],sA[2]), sA[2]);

                mrw_w la[12]; mrw_trs_to_affine_w(lrx,lry,lrz,lrw, tx,ty,tz, sx,sy,sz, la);
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
