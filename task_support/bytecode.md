# Fragment bytecode contract

Public instruction record: opcode `op`; integer operands `dst,a,b,c`; four float immediates `imm[4]`; unsigned `count`. Declare enum values with the names below in the new public header. Only operands used by an instruction are validated. Each instruction reads all four lanes of every source before writing any destination; destination/source aliasing is allowed. Source registers are 0..15, destination registers 3..15; r0..r2 are read-only inputs. All registers are four floats per lane. Immediates must be finite. Unused fields have no effect.

| Opcode | Meaning |
|---|---|
| CONST | dst = imm |
| MOV | dst = a |
| ADD | dst = a+b, componentwise |
| MUL | dst = a*b, componentwise |
| MAD | dst = a*b+c, separate multiply then add, no required FMA |
| LT | dst[k] = a[k] < b[k] ? 1 : 0 |
| TEX | dst = nearest repeat sample at a.xy; RGB normalized to [0,1], alpha=1 |
| IF | enter if a.x != 0 |
| ELSE / ENDIF | usual structured alternatives; ELSE optional, at most once |
| REPEAT | repeat its body count times, count in 1..16 |
| BREAK_IF | if a.x !=0, exit innermost loop for this lane |
| CONTINUE_IF | if a.x !=0, skip to the next iteration of innermost loop for this lane |
| ENDREPEAT | closes REPEAT |
| DISCARD_IF | if a.x !=0, permanently remove lane from write mask; execution continues |
| DDX | dst from a, right minus left in each row, same result for both lanes in that row |
| DDY | dst from a, bottom minus top in each column, same result for both lanes in that column |
| OUTPUT | output the RGBA in a; exactly one OUTPUT, final instruction, outside every block |

Instruction count 1..256, nested structural depth at most 8. No arbitrary jumps or recursive calls. BREAK_IF/CONTINUE_IF require an enclosing REPEAT. DDX/DDY are legal only outside all IF/REPEAT blocks. For the expansion budget, expand every REPEAT syntactically count times and include both sides of each IF; at most 4096 executable instructions, counting IF, ELSE, ENDIF and OUTPUT, but not REPEAT/ENDREPEAT markers. Checked arithmetic must prevent size overflow.

Validation treats every condition as potentially true or false, even when computed from CONST. A register read must be defined on every feasible structural path reaching it, accounting for zero iterations remaining after BREAK_IF and skipped statements after CONTINUE_IF. REPEAT executes at least once but can break/continue in its first iteration. Input registers begin defined; temporary registers do not. DISCARD_IF is not a control-flow exit. An unclosed block reports its opening instruction. Validate structure/operands before definite initialization. Reject the first invalid operand/structure instruction; if multiple undefined reads exist, report the lowest original instruction index. Budget failure reports the REPEAT that first makes the expansion exceed the limit (or final instruction when no loop caused it). Do not read invalid program indices while producing diagnostics.

Independent quad execution accepts four lanes' input registers and a 4-bit coverage mask. All four lanes execute, including helpers. Returned write mask is coverage minus every lane that ever discarded. On execution failure caller outputs and mask remain unchanged. Register computations use float; reject any nonfinite produced value in any lane, including helpers, before committing the quad. A zero coverage mask is valid and produces a zero write mask, but still executes the validated program. Programs without TEX can execute without a texture; a TEX program requires GL_TEXTURE_2D enabled for production drawing and a valid texture in the independent execution context.

Texture coordinates repeat by fract(x)=x-floor(x), texel indices floor(fract(u)*256), floor(fract(v)*256). Sampling uses the existing texture object's texels, not a separate synthetic sampler in production. The new program's OUTPUT RGB is clamped to [0,1] only at final byte conversion; alpha is returned by independent execution but blending is outside this mode. Program arithmetic and derivatives remain unclamped. Texture, program and uniforms (the immutable CONST data) must come from the actual current draw state; helper evaluation has no framebuffer writes.
