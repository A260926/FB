; Readable Shader Model 3 pixel-shader assembly corresponding to the
; predicated PS3 fixtures built in checks/predication.c. Documentation only;
; the legal machine words are constructed directly in C, no assembler needed.

ps_3_sw

; --- case_cmp_pixel ---------------------------------------------------------
def c0, 1, -1, 0, -5
def c1, 100, 100, 100, 100
def c2, 200, 200, 200, 200
(p0) cmp r4, c0, c1, c2         ; src>=0 picks src1, otherwise src2

; --- case_texldd (2D sampler, explicit gradients) --------------------------
dcl_sampler s0, 2d
(p0) texldd r7.xyz, c0, s0, c1, c2   ; c0=uv, c1=ddx, c2=ddy

; --- case_scalar_destination (scalar depth output, no component suffix) ----
(p0.x) mov oDepth, c0.x

; Stage restriction (kept, not loosened):
;   texldd is pixel-shader only. A predicated texldd in a VS is an error.
