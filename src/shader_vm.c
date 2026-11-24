/*
 * Fragment bytecode virtual machine: executes one 2x2 quad (four lanes) of
 * a validated shader program.  Used both by the production rasterizer
 * (src/shader.c) and by the public independent execution interface
 * (tglShaderExecuteQuad).
 *
 * Lane order is fixed: 0 = top-left, 1 = top-right, 2 = bottom-left,
 * 3 = bottom-right.  Top-level instructions run in lockstep across the
 * quad; IF/REPEAT blocks run per lane and reconverge at the end of the
 * block, so DDX/DDY (legal only at top level) always see the real
 * neighbouring lanes' registers at the same program point.
 */

#include <math.h>
#include <string.h>

#include "shader_int.h"

#define FLOW_NORMAL 0
#define FLOW_BREAK 1
#define FLOW_CONTINUE 2

typedef struct VMState {
    const TGLShaderProgram *prog;
    const GLuint *texels;
    GLfloat regs[4][TGL_SHADER_NUM_REGS][4];
    GLubyte discard[4];
    GLint err;
} VMState;

static GLint vec4_finite(const GLfloat v[4])
{
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) &&
           isfinite(v[3]);
}

/* Nearest repeat sample of the 256x256 texture, RGB normalized to [0,1]. */
static void vm_tex_sample(const GLuint *texels, GLfloat u, GLfloat v,
                          GLfloat out[4])
{
    GLfloat fu = u - floorf(u);
    GLfloat fv = v - floorf(v);
    GLint iu = (GLint) floorf(fu * (GLfloat) TGL_SHADER_TEXTURE_DIM);
    GLint iv = (GLint) floorf(fv * (GLfloat) TGL_SHADER_TEXTURE_DIM);
    GLuint p = texels[(iv & 255) * TGL_SHADER_TEXTURE_DIM + (iu & 255)];
    out[0] = (GLfloat) ((p >> 16) & 0xff) / 255.0f;
    out[1] = (GLfloat) ((p >> 8) & 0xff) / 255.0f;
    out[2] = (GLfloat) (p & 0xff) / 255.0f;
    out[3] = 1.0f;
}

/* Execute one scalar (non-control) instruction for one lane.  All source
 * registers are read into temporaries before the destination is written, so
 * destination/source aliasing is safe. */
static void vm_scalar(VMState *vm, const TGLShaderInstr *in, GLint lane)
{
    GLfloat(*R)[4] = vm->regs[lane];
    GLfloat a4[4], b4[4], c4[4], r[4];
    GLint k;

    switch (in->op) {
    case CONST:
        r[0] = in->imm[0];
        r[1] = in->imm[1];
        r[2] = in->imm[2];
        r[3] = in->imm[3];
        break;
    case MOV:
        memcpy(r, R[in->a], sizeof r);
        break;
    case ADD:
        memcpy(a4, R[in->a], sizeof a4);
        memcpy(b4, R[in->b], sizeof b4);
        for (k = 0; k < 4; k++)
            r[k] = a4[k] + b4[k];
        break;
    case MUL:
        memcpy(a4, R[in->a], sizeof a4);
        memcpy(b4, R[in->b], sizeof b4);
        for (k = 0; k < 4; k++)
            r[k] = a4[k] * b4[k];
        break;
    case MAD:
        memcpy(a4, R[in->a], sizeof a4);
        memcpy(b4, R[in->b], sizeof b4);
        memcpy(c4, R[in->c], sizeof c4);
        for (k = 0; k < 4; k++) {
            /* separate multiply then add; no FMA contraction */
            volatile GLfloat t = a4[k] * b4[k];
            r[k] = t + c4[k];
        }
        break;
    case LT:
        memcpy(a4, R[in->a], sizeof a4);
        memcpy(b4, R[in->b], sizeof b4);
        for (k = 0; k < 4; k++)
            r[k] = a4[k] < b4[k] ? 1.0f : 0.0f;
        break;
    case TEX:
        vm_tex_sample(vm->texels, R[in->a][0], R[in->a][1], r);
        break;
    case DISCARD_IF:
        if (R[in->a][0] != 0.0f)
            vm->discard[lane] = 1;
        return;
    default:
        /* unreachable for a validated program */
        vm->err = TGL_SHADER_EXEC_ARGUMENT;
        return;
    }
    memcpy(R[in->dst], r, sizeof r);
    if (!vec4_finite(r))
        vm->err = TGL_SHADER_EXEC_NONFINITE;
}

/* Execute instructions [pc, end) for one lane, recursing into IF/REPEAT
 * blocks.  *flow reports BREAK/CONTINUE to the innermost enclosing REPEAT. */
static void vm_exec_range(VMState *vm, GLint lane, GLint pc, GLint end,
                          GLint *flow)
{
    const TGLShaderProgram *prog = vm->prog;
    while (pc < end) {
        const TGLShaderInstr *in;
        if (vm->err)
            return;
        in = &prog->code[pc];
        switch (in->op) {
        case IF: {
            GLint endif = prog->match[pc];
            GLint elsep = prog->else_idx[pc];
            if (vm->regs[lane][in->a][0] != 0.0f)
                vm_exec_range(vm, lane, pc + 1, elsep >= 0 ? elsep : endif,
                              flow);
            else if (elsep >= 0)
                vm_exec_range(vm, lane, elsep + 1, endif, flow);
            if (vm->err || *flow != FLOW_NORMAL)
                return;
            pc = endif + 1;
        } break;
        case REPEAT: {
            GLint endrep = prog->match[pc];
            GLuint it;
            for (it = 0; it < in->count; it++) {
                *flow = FLOW_NORMAL;
                vm_exec_range(vm, lane, pc + 1, endrep, flow);
                if (vm->err)
                    return;
                if (*flow == FLOW_BREAK)
                    break;
                /* FLOW_CONTINUE and FLOW_NORMAL both start the next pass */
            }
            *flow = FLOW_NORMAL;
            pc = endrep + 1;
        } break;
        case BREAK_IF:
            if (vm->regs[lane][in->a][0] != 0.0f) {
                *flow = FLOW_BREAK;
                return;
            }
            pc++;
            break;
        case CONTINUE_IF:
            if (vm->regs[lane][in->a][0] != 0.0f) {
                *flow = FLOW_CONTINUE;
                return;
            }
            pc++;
            break;
        default:
            vm_scalar(vm, in, lane);
            pc++;
            break;
        }
    }
}

GLint tgl_shader_vm_run(const TGLShaderProgram *prog,
                        const GLuint *texels,
                        const GLfloat in[4][TGL_SHADER_INPUT_REGS][4],
                        GLfloat out[4][4],
                        GLuint *discard_mask)
{
    VMState vm;
    GLint lane, pc, k, kk;

    if (!prog || !in || !out || !discard_mask)
        return TGL_SHADER_EXEC_ARGUMENT;
    if (prog->has_tex && !texels)
        return TGL_SHADER_EXEC_NO_TEXTURE;

    vm.prog = prog;
    vm.texels = texels;
    vm.err = TGL_SHADER_EXEC_OK;

    /* Load inputs and clear temporaries.  Inputs must be finite in every
     * lane, helpers included. */
    for (lane = 0; lane < 4; lane++) {
        vm.discard[lane] = 0;
        for (k = 0; k < TGL_SHADER_INPUT_REGS; k++) {
            for (kk = 0; kk < 4; kk++) {
                GLfloat v = in[lane][k][kk];
                if (!isfinite(v))
                    return TGL_SHADER_EXEC_NONFINITE;
                vm.regs[lane][k][kk] = v;
            }
        }
        for (k = TGL_SHADER_INPUT_REGS; k < TGL_SHADER_NUM_REGS; k++)
            for (kk = 0; kk < 4; kk++)
                vm.regs[lane][k][kk] = 0.0f;
    }

    pc = 0;
    while (pc < (GLint) prog->ninstr && !vm.err) {
        const TGLShaderInstr *in = &prog->code[pc];
        switch (in->op) {
        case IF:
        case REPEAT: {
            /* run the block per lane; lanes reconverge afterwards */
            GLint next = prog->match[pc] + 1;
            for (lane = 0; lane < 4 && !vm.err; lane++) {
                GLint flow = FLOW_NORMAL;
                vm_exec_range(&vm, lane, pc, pc + 1, &flow);
            }
            pc = next;
        } break;
        case DDX:
        case DDY: {
            /* snapshot the source for all lanes before writing */
            GLfloat src[4][4], d[4];
            for (lane = 0; lane < 4; lane++)
                memcpy(src[lane], vm.regs[lane][in->a], sizeof src[0]);
            for (k = 0; k < 4; k++) {
                if (in->op == DDX) {
                    d[0] = d[1] = src[1][k] - src[0][k];
                    d[2] = d[3] = src[3][k] - src[2][k];
                } else {
                    d[0] = d[2] = src[2][k] - src[0][k];
                    d[1] = d[3] = src[3][k] - src[1][k];
                }
                for (lane = 0; lane < 4; lane++)
                    vm.regs[lane][in->dst][k] = d[lane];
            }
            for (lane = 0; lane < 4; lane++)
                if (!vec4_finite(vm.regs[lane][in->dst]))
                    vm.err = TGL_SHADER_EXEC_NONFINITE;
            pc++;
        } break;
        case OUTPUT:
            for (lane = 0; lane < 4; lane++)
                memcpy(out[lane], vm.regs[lane][in->a], 4 * sizeof(GLfloat));
            pc++;
            break;
        default:
            for (lane = 0; lane < 4 && !vm.err; lane++)
                vm_scalar(&vm, in, lane);
            pc++;
            break;
        }
    }
    if (vm.err)
        return vm.err;
    {
        GLuint dm = 0;
        for (lane = 0; lane < 4; lane++)
            if (vm.discard[lane])
                dm |= 1u << lane;
        *discard_mask = dm;
    }
    return TGL_SHADER_EXEC_OK;
}

GLint tglShaderExecuteQuad(const TGLShaderProgram *prog,
                           const GLfloat in[4][TGL_SHADER_INPUT_REGS][4],
                           GLfloat out[4][4],
                           GLuint coverage,
                           GLuint *write_mask,
                           const GLuint *texels)
{
    GLuint discard = 0;
    GLint err;
    if (!prog || !in || !out || !write_mask)
        return TGL_SHADER_EXEC_ARGUMENT;
    err = tgl_shader_vm_run(prog, texels, in, out, &discard);
    if (err != TGL_SHADER_EXEC_OK)
        return err;
    *write_mask = (coverage & 0xFu) & ~discard;
    return TGL_SHADER_EXEC_OK;
}
