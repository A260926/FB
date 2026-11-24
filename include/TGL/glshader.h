#ifndef TGL_GLSHADER_H
#define TGL_GLSHADER_H

/*
 * TinyGL fragment bytecode ("material preview") interface.
 *
 * A shader program is a short sequence of scalar instructions executed by a
 * small virtual machine once per 2x2 pixel quad (four lanes).  Programs are
 * created with tglShaderCreate(), which validates the whole instruction
 * stream (structure, expansion budget and definite initialization) and
 * stores a private copy of the instructions.  A program is bound to the
 * TinyGL context with tglShaderBind(); the binding holds its own reference,
 * so the caller may release its handle immediately and keep drawing.
 * Binding is immediate state: it is NOT recorded into display lists, and
 * triangles replayed from a list use whatever program is bound at replay
 * time.  Unbinding (tglShaderBind(NULL)) and glClose() release the
 * context's reference.  With no program bound, the classic fixed pipeline
 * runs unchanged.
 *
 * Only C99 and the standard C library are required.  There is no shader
 * compiler, window system or GPU dependency.
 */

#include <TGL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Limits (see task_support/bytecode.md). */
#define TGL_SHADER_NUM_REGS 16   /* registers per lane, r0..r15 */
#define TGL_SHADER_INPUT_REGS 3  /* r0..r2 are read-only inputs */
#define TGL_SHADER_FIRST_TEMP 3  /* r3..r15 are temporaries */
#define TGL_SHADER_MAX_INSTR 256 /* instructions per program */
#define TGL_SHADER_MAX_DEPTH 8   /* structural nesting depth */
#define TGL_SHADER_MAX_REPEAT 16 /* REPEAT trip count */
#define TGL_SHADER_BUDGET 4096   /* expanded instruction budget */
#define TGL_SHADER_TEXTURE_DIM 256

/* Public instruction record.  Only the operands used by an instruction are
 * validated; unused fields have no effect. */
typedef struct TGLShaderInstr {
    GLint op;           /* opcode, one of the enum values below */
    GLint dst, a, b, c; /* integer register operands */
    GLfloat imm[4];     /* CONST immediate (must be finite) */
    GLuint count;       /* REPEAT trip count (1..16) */
} TGLShaderInstr;

/* Opcodes.  Names follow task_support/bytecode.md exactly. */
enum {
    CONST = 0,   /* dst = imm */
    MOV,         /* dst = a */
    ADD,         /* dst = a + b (componentwise) */
    MUL,         /* dst = a * b (componentwise) */
    MAD,         /* dst = a * b + c (separate multiply then add, no FMA) */
    LT,          /* dst[k] = a[k] < b[k] ? 1 : 0 */
    TEX,         /* dst = nearest repeat sample of the 256x256 texture at a.xy */
    IF,          /* enter block if a.x != 0 */
    ELSE,        /* optional alternative of IF, at most one */
    ENDIF,       /* closes IF */
    REPEAT,      /* repeat body count times, count in 1..16 */
    BREAK_IF,    /* if a.x != 0, exit innermost REPEAT for this lane */
    CONTINUE_IF, /* if a.x != 0, skip to next iteration of innermost REPEAT */
    ENDREPEAT,   /* closes REPEAT */
    DISCARD_IF,  /* if a.x != 0, drop lane from the write mask (keeps executing) */
    DDX,         /* dst = right - left of a per row (top level only) */
    DDY,         /* dst = bottom - top of a per column (top level only) */
    OUTPUT       /* output RGBA in a; exactly one, final instruction, top level */
};

/* Creation error reasons. */
enum {
    TGL_SHADER_ERR_NONE = 0,   /* no error */
    TGL_SHADER_ERR_ARGUMENT,   /* NULL code or instruction count not 1..256 */
    TGL_SHADER_ERR_OPCODE,     /* unknown opcode */
    TGL_SHADER_ERR_OPERAND,    /* register operand out of range */
    TGL_SHADER_ERR_IMMEDIATE,  /* non-finite CONST immediate */
    TGL_SHADER_ERR_REPEAT_COUNT, /* REPEAT count not in 1..16 */
    TGL_SHADER_ERR_STRUCTURE,  /* misplaced/mismatched block instruction,
                                  nesting deeper than 8, or BREAK_IF/CONTINUE_IF
                                  outside any REPEAT */
    TGL_SHADER_ERR_UNCLOSED,   /* unclosed block; index is the opening instruction */
    TGL_SHADER_ERR_OUTPUT,     /* OUTPUT missing, not final, or inside a block */
    TGL_SHADER_ERR_BUDGET,     /* expansion budget exceeded; index is the REPEAT
                                  that first makes the expansion exceed 4096
                                  (or the final instruction when no loop caused it) */
    TGL_SHADER_ERR_UNDEFINED,  /* read of a register that is not defined on
                                  every feasible structural path */
    TGL_SHADER_ERR_DERIVATIVE, /* DDX/DDY inside an IF/REPEAT block */
    TGL_SHADER_ERR_MEMORY      /* allocation failure */
};

/* Diagnostic produced by tglShaderCreate().  index is the original
 * instruction index the error refers to, or -1 when it does not apply. */
typedef struct TGLShaderError {
    GLint index; /* original instruction index, -1 if not applicable */
    GLint code;  /* TGL_SHADER_ERR_* */
} TGLShaderError;

/* Execution / production error codes. */
enum {
    TGL_SHADER_EXEC_OK = 0,    /* success */
    TGL_SHADER_EXEC_ARGUMENT,  /* bad argument (NULL program/out/mask/inputs) */
    TGL_SHADER_EXEC_NO_TEXTURE, /* TEX program without a usable texture */
    TGL_SHADER_EXEC_NONFINITE, /* a non-finite value was produced in some lane */
    TGL_SHADER_EXEC_ZERO_DIVIDE, /* zero perspective denominator while building
                                    per-lane inputs in the production path */
    TGL_SHADER_EXEC_UNSUPPORTED /* production configuration rejected (blending,
                                   GL_FLAT, or point/line polygon modes) */
};

typedef struct TGLShaderProgram TGLShaderProgram;

/* Create a validated program owning a copy of code[0..ninstr-1].
 * On success returns a program with one reference (the caller's) and sets
 * err->code to TGL_SHADER_ERR_NONE (err may be NULL).  On failure returns
 * NULL, fills *err with the original instruction index and reason, and does
 * not change the current binding. */
TGLShaderProgram *tglShaderCreate(const TGLShaderInstr *code,
                                  GLuint ninstr,
                                  TGLShaderError *err);

/* Drop one reference.  The program is freed when the count reaches zero.
 * NULL is accepted and does nothing. */
void tglShaderRelease(TGLShaderProgram *prog);

/* Bind a program to the current TinyGL context (NULL unbinds).  The binding
 * holds its own reference: releasing the caller's handle does not stop
 * drawing.  Immediate state; not recorded into display lists. */
void tglShaderBind(TGLShaderProgram *prog);

/* Currently bound program, or NULL.  Does not transfer a reference. */
TGLShaderProgram *tglShaderGetBound(void);

/* Query and clear the last production-path shader error
 * (TGL_SHADER_EXEC_*).  This reports draw-time failures: rejected
 * configurations, missing textures, non-finite values or zero perspective
 * denominators encountered while rasterizing. */
GLint tglShaderGetError(void);

/* Independent four-lane execution of a validated program.
 *
 * in[lane][reg][comp] supplies the input registers r0..r2 of each of the
 * four lanes (lane order: top-left, top-right, bottom-left, bottom-right).
 * coverage is a 4-bit mask of real (non-helper) lanes; a zero mask is valid
 * and still executes the program.  All four lanes execute, helpers included.
 *
 * texels points to 256*256 texels in the TinyGL 32-bit PIXEL layout
 * (0xAARRGGBB; R in bits 16..23, G in 8..15, B in 0..7), or is NULL.  A
 * program containing TEX requires non-NULL texels; other programs ignore
 * the pointer.
 *
 * On success returns TGL_SHADER_EXEC_OK, writes the per-lane OUTPUT RGBA to
 * out and sets *write_mask to coverage minus every lane that executed
 * DISCARD_IF.  On failure returns a TGL_SHADER_EXEC_* code and leaves out
 * and *write_mask untouched. */
GLint tglShaderExecuteQuad(const TGLShaderProgram *prog,
                           const GLfloat in[4][TGL_SHADER_INPUT_REGS][4],
                           GLfloat out[4][4],
                           GLuint coverage,
                           GLuint *write_mask,
                           const GLuint *texels);

#ifdef __cplusplus
}
#endif

#endif /* TGL_GLSHADER_H */
