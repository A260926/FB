;
; predication_ps.asm
;
; Human-readable view of the legal ps_3_0 token words built by
; checks/check_predication.c (documentation only; no external assembler is
; used). CMP and TEXLDD are pixel-shader-only; TEXLDL is valid here too.
; All predicated texture work is restricted to 2D samplers.
;
ps_3_0
    dcl_texcoord v0
    dcl_sampler2d s0

    ; Establish four independent predicate lanes.
    setp_ge p0, v0, v0

    ; CMP is pixel-only: dst.c = src0.c >= 0 ? src1.c : src2.c. The candidate
    ;  is built once, then committed through the per-channel predicate.
    (p0)     cmp r0, v0, v0, v0

    ; Explicit-gradient fetch with two gradient groups, one sample total.
    (!p0.x)  texldd r1, v0, s0, v0, v0

    ; LOD fetch is legal in the pixel shader as well.
    mov      r2, v0
    (p0)     texldl r3, r2, s0

    ; Partial SETP only rewrites the masked predicate lanes; the others keep
    ;  their previous boolean values.
    setp_ge p0.xz, v0, v0

    mov oC0, r1
