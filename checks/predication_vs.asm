;
; predication_vs.asm
;
; Human-readable view of the legal vs_3_0 token words built by
; checks/check_predication.c (the words themselves are constructed in C; this
; project no longer ships an assembler, so this file is documentation only).
; It covers every vertex-legal predicated operation and the predicate rules:
; per-component p0, replicated p0.x, and the "!" negate, combined with sparse
; write masks. Components closed by the mask or the predicate keep their old
; value; every source is read before any destination channel is written.
;
vs_3_0
    dcl_position v0
    dcl_position v1
    dcl_sampler2d s0

    ; Four independent boolean lanes; without a swizzle each destination
    ;  channel is gated by the matching predicate lane.
    setp_ge p0, v1, v1
    (p0)      mov  r0, v1                 ; r0.x/r0.z update, r0.y/r0.w kept

    ; A replicate swizzle broadcasts one lane; "!" negates it lane by lane.
    (!p0.x)   add  r0.xz, v1, v1           ; same gate on x and z only

    ; Sparse mask leaves the other channels alone even when predicated open.
    (p0)      mul_sat r0, v1, v1           ; saturate only on written lanes

    ; Arithmetic family, all under predication.
    (p0)      sub  r0, v1, v1
    (p0)      mad  r0, v1, v1, v1
    (p0)      min  r0, v1, v1
    (p0)      max  r0, v1, v1
    (p0)      dp3  r0.xyz, v1, v1
    (p0)      dp4  r0, v1, v1

    ; Vertex texture fetch: one candidate sample, LOD from the .w channel,
    ;  then per-channel submission (the texture is never re-sampled).
    mov       r1, v1
    (p0)      texldl r2, r1, s0

    ; Predication reused inside a loop and a called helper.
    defi i0, 0, 3, 1, 0
    loop aL, i0
        (p0)  mov r0, v1
    endloop

    call l0
    ret

label l0
    (p0)  mov r0, v1
    ret
