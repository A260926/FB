#ifndef TGL_GLSHADER_H
#define TGL_GLSHADER_H

/*
 * TinyGL fragment bytecode programs.
 *
 * A TGLShaderProgram is a small validated bytecode that runs on the CPU
 * during triangle rasterization, once per 2x2 pixel quad with four lanes
 * (top-left, top-right, bottom-left, bottom-right of an even-aligned 2x2
 * block of pixel centers).
 *
 * This header is self-contained; it does not require TGL/gl.h.
 *
 * Instruction set summary (see README.md for the full contract):
 *   registers r0..r15, four floats each, per lane.
 *   r0    input: perspective-correct (u, v, 0, 1) after the texture matrix
 *   r1    input: Gouraud color (r, g, b, a)
 *   r2    input: (pixel center x, pixel center y, window depth [0,1], 1/clip.w)
 *   r3..r15 temporaries (undefined at program start)
 */

#ifdef __cplusplus
extern "C" {
#endif

/* opcodes */
enum {
    TGL_SHADER_CONST = 0, /* dst = imm */
    TGL_SHADER_MOV,       /* dst = a */
    TGL_SHADER_ADD,       /* dst = a + b (componentwise) */
    TGL_SHADER_MUL,       /* dst = a * b (componentwise) */
    TGL_SHADER_MAD,       /* dst = a * b + c (separate multiply, then add) */
    TGL_SHADER_LT,        /* dst[k] = a[k] < b[k] ? 1 : 0 */
    TGL_SHADER_TEX,       /* dst = nearest repeat texture sample at a.xy */
    TGL_SHADER_IF,        /* enter block if a.x != 0 */
    TGL_SHADER_ELSE,      /* optional alternative of IF (at most one) */
    TGL_SHADER_ENDIF,     /* closes IF */
    TGL_SHADER_REPEAT,    /* run body count times, count in 1..16 */
    TGL_SHADER_BREAK_IF,  /* if a.x != 0, exit innermost REPEAT for this lane */
    TGL_SHADER_CONTINUE_IF, /* if a.x != 0, next iteration for this lane */
    TGL_SHADER_ENDREPEAT, /* closes REPEAT */
    TGL_SHADER_DISCARD_IF,  /* if a.x != 0, remove lane from write mask */
    TGL_SHADER_DDX,       /* dst = a(right lane) - a(left lane) per row */
    TGL_SHADER_DDY,       /* dst = a(bottom lane) - a(top lane) per column */
    TGL_SHADER_OUTPUT     /* output RGBA in a; exactly one, final instruction */
};

#define TGL_SHADER_MAX_INST 256
#define TGL_SHADER_MAX_DEPTH 8
#define TGL_SHADER_MAX_REPEAT 16
#define TGL_SHADER_EXPAND_BUDGET 4096
#define TGL_SHADER_NUM_REGS 16
#define TGL_SHADER_NUM_INPUTS 3
#define TGL_SHADER_FIRST_TEMP 3
#define TGL_SHADER_TEXTURE_DIM 256

/* public instruction record; only the operands used by an opcode are
 * validated, unused fields have no effect */
typedef struct TGLShaderInst {
    int op;
    int dst, a, b, c;
    float imm[4];
    unsigned count;
} TGLShaderInst;

/* validation error codes */
enum {
    TGL_SHADER_OK = 0,
    TGL_SHADER_ERR_SIZE,       /* instruction count outside 1..256 */
    TGL_SHADER_ERR_OPCODE,     /* unknown opcode */
    TGL_SHADER_ERR_OPERAND,    /* register operand out of range */
    TGL_SHADER_ERR_IMM,        /* non-finite CONST immediate */
    TGL_SHADER_ERR_COUNT,      /* REPEAT count outside 1..16 */
    TGL_SHADER_ERR_STRUCTURE,  /* mismatched/unclosed block, bad OUTPUT */
    TGL_SHADER_ERR_DEPTH,      /* structural nesting deeper than 8 */
    TGL_SHADER_ERR_BUDGET,     /* loop expansion budget exceeded */
    TGL_SHADER_ERR_LOOPCTL,    /* BREAK_IF/CONTINUE_IF outside any REPEAT */
    TGL_SHADER_ERR_DERIVATIVE, /* DDX/DDY inside a structured block */
    TGL_SHADER_ERR_UNDEF       /* read of a possibly undefined register */
};

/* validation failure report: index is the original instruction index the
 * error is attributed to (-1 when not applicable) */
typedef struct TGLShaderError {
    int code;
    int index;
} TGLShaderError;

typedef struct TGLShaderProgram TGLShaderProgram;

/* Create a validated program. The program owns a copy of the instructions.
 * On success returns a program with reference count 1 (owned by the caller)
 * and stores TGL_SHADER_OK in err when err is not NULL. On failure returns
 * NULL, stores the error in err when not NULL, and does not change any
 * binding. */
TGLShaderProgram *tglShaderCreate(const TGLShaderInst *code,
                                  unsigned ninst,
                                  TGLShaderError *err);
void tglShaderRetain(TGLShaderProgram *prog);
void tglShaderRelease(TGLShaderProgram *prog);

/* Bind a program to the current TinyGL context (NULL unbinds). Binding is
 * immediate state: it is never recorded into display lists, and triangles
 * replayed from a list use whatever program is bound at replay time. The
 * context keeps its own reference, so the caller may release its handle
 * after binding and keep drawing. Unbinding and glClose() release the
 * context reference. */
void tglBindShader(TGLShaderProgram *prog);
TGLShaderProgram *tglGetBoundShader(void);

/* runtime errors produced while drawing with a bound program */
enum {
    TGL_SHADER_RT_NONE = 0,
    TGL_SHADER_RT_STATE,      /* unsupported state: blending, GL_FLAT, or
                                 point/line polygon modes with a program */
    TGL_SHADER_RT_NO_TEXTURE, /* program uses TEX but no 2D texture is bound
                                 and enabled */
    TGL_SHADER_RT_NONFINITE,  /* non-finite value produced at runtime */
    TGL_SHADER_RT_ZERO_W      /* zero perspective denominator */
};

/* Returns the first unreported runtime error for the current context and
 * clears it (TGL_SHADER_RT_NONE when no error is pending). When a quad
 * fails at runtime none of its lanes are committed; quads completed before
 * the failure are kept. */
int tglShaderGetError(void);

/* texture sampler used by the independent quad executor: must write the
 * RGBA of texel (tx, ty) of a 256x256 texture, RGB normalized to [0,1] */
typedef void (*TGLShaderSampleFunc)(void *ctx, int tx, int ty, float rgba[4]);

/* Independent quad execution, without any GL context.
 *   in        : per-lane input registers r0..r2, in[lane][reg][component]
 *   out       : receives the per-lane OUTPUT rgba on success
 *   coverage  : 4-bit mask of real (non-helper) lanes; bit 0 is the
 *               top-left lane, bit 1 top-right, bit 2 bottom-left, bit 3
 *               bottom-right. All four lanes execute, including helpers.
 *   writemask : receives coverage minus every lane that discarded
 *   sample    : texture sampler, required iff the program contains TEX
 * Returns 0 on success. On failure (non-finite value produced in any lane,
 * or TEX without a sampler) returns a nonzero TGL_SHADER_RT_* code and
 * leaves out and *writemask unchanged. A zero coverage mask is valid and
 * produces a zero write mask, but still executes the program. */
int tglShaderExecQuad(const TGLShaderProgram *prog,
                      const float in[4][TGL_SHADER_NUM_INPUTS][4],
                      float out[4][4],
                      unsigned coverage,
                      unsigned *writemask,
                      TGLShaderSampleFunc sample,
                      void *sample_ctx);

/* nonzero when the program contains a TEX instruction */
int tglShaderUsesTexture(const TGLShaderProgram *prog);

#ifdef __cplusplus
}
#endif

#endif /* TGL_GLSHADER_H */
