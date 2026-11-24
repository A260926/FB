#ifndef TGL_SHADER_INT_H
#define TGL_SHADER_INT_H

/*
 * Internal shared declarations for the fragment bytecode implementation.
 * Not installed; only src/shader.c and src/shader_vm.c include this.
 */

#include <TGL/glshader.h>

struct TGLShaderProgram {
    GLuint refcount;      /* caller handles + context binding */
    GLuint ninstr;        /* instruction count (1..TGL_SHADER_MAX_INSTR) */
    GLint has_tex;        /* program contains at least one TEX */
    TGLShaderInstr *code; /* owned copy of the validated instructions */
    GLint *match;         /* per-instruction matching block instruction */
    GLint *else_idx;      /* per-IF index of its ELSE, or -1 */
};

/* Execute one quad with a validated program.  in supplies r0..r2 per lane;
 * all four lanes always execute.  On TGL_SHADER_EXEC_OK, out receives the
 * per-lane OUTPUT RGBA and *discard_mask the lanes that discarded.  On
 * failure nothing is written. */
GLint tgl_shader_vm_run(const TGLShaderProgram *prog,
                        const GLuint *texels,
                        const GLfloat in[4][TGL_SHADER_INPUT_REGS][4],
                        GLfloat out[4][4],
                        GLuint *discard_mask);

#endif /* TGL_SHADER_INT_H */
