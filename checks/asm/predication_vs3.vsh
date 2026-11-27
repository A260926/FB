; Readable Shader Model 3 vertex-shader assembly corresponding to the
; predicated fixtures built in checks/predication.c. This file is
; documentation only: the legal machine words are constructed directly in
; C, so no external assembler is required.
;
; Predicate semantics used below:
;   (p0)   mov r1, v0      ; each channel is gated by the matching p0 component
;   (!p0.x) add r1, r0, r1 ; the same negated component gates every channel
; A static write mask and the predicate together decide the final write;
; every channel that is not written keeps its previous value.

vs_3_sw

; --- case_mixed_vector_predicate -----------------------------------------
dcl_texcoord v0
setp_ge p0, v0, v0
(p0) mov r1, v0                 ; r1 = {v0.x, old r1.y, v0.z, old r1.w}

; --- case_negated_replicate -----------------------------------------------
(!p0.x) add r1, r0, r1          ; !p0.x false everywhere: r1 is untouched

; --- case_sparse_mask ------------------------------------------------------
(p0) mul r2.xz, r0, r0          ; only x/z are eligible, then predicate-gated

; --- case_setp_partial -----------------------------------------------------
dcl_texcoord1 v1
setp_lt p0.yz, v1, v0           ; p0.x and p0.w are preserved

; --- case_aliasing ----------------------------------------------------------
(p0) mov r0, r0.wzyx            ; all sources read before any channel commits

; --- case_saturate ----------------------------------------------------------
(p0) mov_sat r3, r0             ; saturation applies only to written channels

; --- case_all_arithmetic ----------------------------------------------------
(p0) add r5, r1, r2
(p0) sub r5, r1, r2
(p0) mul r5, r1, r2
(p0) mad r5, r1, r2, r3
(p0) min r5, r1, r2
(p0) max r5, r1, r2
(p0) dp3 r5, r1, r2
(p0) dp4 r5, r1, r2

; --- case_texldl (2D sampler, explicit LOD in the .w component) ------------
dcl_sampler s0, 2d
(p0) texldl r6, c0, s0          ; c0.xy = uv, c0.w = level of detail

; --- case_loop_reuse --------------------------------------------------------
defi i0, 0, 2, 1, 0
loop aL, i0
    setp_gt p0, r0, c0
    (p0) add r1, r1, c1         ; the same predicate temp is reused per iter
endloop

; --- case_helper_reuse ------------------------------------------------------
call l0
ret
label l0
    setp_ge p0, r0, r0
    (p0) mov r2, r1
ret
