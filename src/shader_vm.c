/*
 * Fragment bytecode program validation and quad execution.
 *
 * This file is independent of the GL context: it implements program
 * creation (structural/operand validation, loop expansion budget, and
 * definite-initialization analysis over the real structured control flow)
 * plus the independent four-lane execution engine.
 */

#include <TGL/glshader.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct TGLShaderProgram {
    int refcount;
    unsigned ninst;
    int uses_tex;
    TGLShaderInst *code; /* owned copy of ninst instructions */
    /* structure, filled at creation (all indices refer to code[]) */
    int match[TGL_SHADER_MAX_INST];    /* IF/ELSE->ENDIF, ENDIF->IF,
                                          REPEAT<->ENDREPEAT, else -1 */
    int else_idx[TGL_SHADER_MAX_INST]; /* IF -> ELSE index or -1 */
    int loop_rep[TGL_SHADER_MAX_INST]; /* innermost enclosing REPEAT or -1 */
};

/* ------------------------------------------------------------------ */
/* operand classes                                                      */
/* ------------------------------------------------------------------ */

static int op_writes_dst(int op)
{
    switch (op) {
    case TGL_SHADER_CONST:
    case TGL_SHADER_MOV:
    case TGL_SHADER_ADD:
    case TGL_SHADER_MUL:
    case TGL_SHADER_MAD:
    case TGL_SHADER_LT:
    case TGL_SHADER_TEX:
    case TGL_SHADER_DDX:
    case TGL_SHADER_DDY:
        return 1;
    default:
        return 0;
    }
}

/* number of register source operands a..c used by the opcode */
static int op_num_sources(int op)
{
    switch (op) {
    case TGL_SHADER_MOV:
    case TGL_SHADER_TEX:
    case TGL_SHADER_IF:
    case TGL_SHADER_BREAK_IF:
    case TGL_SHADER_CONTINUE_IF:
    case TGL_SHADER_DISCARD_IF:
    case TGL_SHADER_DDX:
    case TGL_SHADER_DDY:
    case TGL_SHADER_OUTPUT:
        return 1;
    case TGL_SHADER_ADD:
    case TGL_SHADER_MUL:
    case TGL_SHADER_LT:
        return 2;
    case TGL_SHADER_MAD:
        return 3;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* creation: structural, operand and budget validation                  */
/* ------------------------------------------------------------------ */

enum { BLK_IF = 1, BLK_REPEAT = 2 };

static void set_err(TGLShaderError *err, int code, int index)
{
    if (err) {
        err->code = code;
        err->index = index;
    }
}

static int validate_structure(TGLShaderProgram *p,
                              const TGLShaderInst *code,
                              unsigned n,
                              TGLShaderError *err)
{
    int blk_kind[TGL_SHADER_MAX_DEPTH];
    int blk_idx[TGL_SHADER_MAX_DEPTH];
    int blk_else[TGL_SHADER_MAX_DEPTH];
    unsigned blk_rcount[TGL_SHADER_MAX_DEPTH];
    int depth = 0;
    unsigned long long mult = 1, budget = 0;
    unsigned i;

    for (i = 0; i < n; i++) {
        p->match[i] = -1;
        p->else_idx[i] = -1;
        p->loop_rep[i] = -1;
    }

    for (i = 0; i < n; i++) {
        const TGLShaderInst *in = &code[i];
        int op = in->op;
        int counts = 1; /* instruction counts towards the expansion budget */
        int j, inner_rep;

        if (op < 0 || op > TGL_SHADER_OUTPUT) {
            set_err(err, TGL_SHADER_ERR_OPCODE, (int) i);
            return 0;
        }

        /* register operands */
        if (op_writes_dst(op) &&
            (in->dst < TGL_SHADER_FIRST_TEMP || in->dst >= TGL_SHADER_NUM_REGS)) {
            set_err(err, TGL_SHADER_ERR_OPERAND, (int) i);
            return 0;
        }
        {
            int src[3];
            int ns = op_num_sources(op);
            src[0] = in->a;
            src[1] = in->b;
            src[2] = in->c;
            for (j = 0; j < ns; j++) {
                if (src[j] < 0 || src[j] >= TGL_SHADER_NUM_REGS) {
                    set_err(err, TGL_SHADER_ERR_OPERAND, (int) i);
                    return 0;
                }
            }
        }

        /* innermost enclosing REPEAT (for BREAK_IF/CONTINUE_IF) */
        inner_rep = -1;
        for (j = depth - 1; j >= 0; j--) {
            if (blk_kind[j] == BLK_REPEAT) {
                inner_rep = blk_idx[j];
                break;
            }
        }
        p->loop_rep[i] = inner_rep;

        switch (op) {
        case TGL_SHADER_CONST:
            for (j = 0; j < 4; j++) {
                if (!isfinite(in->imm[j])) {
                    set_err(err, TGL_SHADER_ERR_IMM, (int) i);
                    return 0;
                }
            }
            break;
        case TGL_SHADER_IF:
            if (depth >= TGL_SHADER_MAX_DEPTH) {
                set_err(err, TGL_SHADER_ERR_DEPTH, (int) i);
                return 0;
            }
            blk_kind[depth] = BLK_IF;
            blk_idx[depth] = (int) i;
            blk_else[depth] = 0;
            blk_rcount[depth] = 1;
            depth++;
            break;
        case TGL_SHADER_ELSE:
            if (depth == 0 || blk_kind[depth - 1] != BLK_IF ||
                blk_else[depth - 1]) {
                set_err(err, TGL_SHADER_ERR_STRUCTURE, (int) i);
                return 0;
            }
            blk_else[depth - 1] = 1;
            p->else_idx[blk_idx[depth - 1]] = (int) i;
            break;
        case TGL_SHADER_ENDIF:
            if (depth == 0 || blk_kind[depth - 1] != BLK_IF) {
                set_err(err, TGL_SHADER_ERR_STRUCTURE, (int) i);
                return 0;
            }
            depth--;
            p->match[blk_idx[depth]] = (int) i;
            p->match[i] = blk_idx[depth];
            if (p->else_idx[blk_idx[depth]] >= 0)
                p->match[p->else_idx[blk_idx[depth]]] = (int) i;
            break;
        case TGL_SHADER_REPEAT:
            if (in->count < 1 || in->count > TGL_SHADER_MAX_REPEAT) {
                set_err(err, TGL_SHADER_ERR_COUNT, (int) i);
                return 0;
            }
            if (depth >= TGL_SHADER_MAX_DEPTH) {
                set_err(err, TGL_SHADER_ERR_DEPTH, (int) i);
                return 0;
            }
            blk_kind[depth] = BLK_REPEAT;
            blk_idx[depth] = (int) i;
            blk_else[depth] = 0;
            blk_rcount[depth] = in->count;
            depth++;
            mult *= in->count;
            counts = 0; /* REPEAT marker itself is not counted */
            break;
        case TGL_SHADER_ENDREPEAT:
            if (depth == 0 || blk_kind[depth - 1] != BLK_REPEAT) {
                set_err(err, TGL_SHADER_ERR_STRUCTURE, (int) i);
                return 0;
            }
            depth--;
            mult /= blk_rcount[depth];
            p->match[blk_idx[depth]] = (int) i;
            p->match[i] = blk_idx[depth];
            counts = 0; /* ENDREPEAT marker itself is not counted */
            break;
        case TGL_SHADER_BREAK_IF:
        case TGL_SHADER_CONTINUE_IF:
            if (inner_rep < 0) {
                set_err(err, TGL_SHADER_ERR_LOOPCTL, (int) i);
                return 0;
            }
            break;
        case TGL_SHADER_DDX:
        case TGL_SHADER_DDY:
            if (depth != 0) {
                set_err(err, TGL_SHADER_ERR_DERIVATIVE, (int) i);
                return 0;
            }
            break;
        case TGL_SHADER_OUTPUT:
            if (depth != 0 || i != n - 1) {
                set_err(err, TGL_SHADER_ERR_STRUCTURE, (int) i);
                return 0;
            }
            break;
        default:
            break;
        }

        /* loop expansion budget, with checked (64-bit) arithmetic */
        if (counts) {
            budget += mult;
            if (budget > (unsigned long long) TGL_SHADER_EXPAND_BUDGET) {
                set_err(err, TGL_SHADER_ERR_BUDGET,
                        inner_rep >= 0 ? inner_rep : (int) i);
                return 0;
            }
        }
    }

    if (depth != 0) {
        /* an unclosed block reports its opening instruction */
        set_err(err, TGL_SHADER_ERR_STRUCTURE, blk_idx[0]);
        return 0;
    }
    if (code[n - 1].op != TGL_SHADER_OUTPUT) {
        set_err(err, TGL_SHADER_ERR_STRUCTURE, (int) (n - 1));
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* creation: definite initialization over the structured control flow   */
/* ------------------------------------------------------------------ */

static unsigned inst_read_mask(const TGLShaderInst *in)
{
    unsigned m = 0;
    switch (in->op) {
    case TGL_SHADER_MOV:
    case TGL_SHADER_TEX:
    case TGL_SHADER_IF:
    case TGL_SHADER_BREAK_IF:
    case TGL_SHADER_CONTINUE_IF:
    case TGL_SHADER_DISCARD_IF:
    case TGL_SHADER_DDX:
    case TGL_SHADER_DDY:
    case TGL_SHADER_OUTPUT:
        m = 1u << in->a;
        break;
    case TGL_SHADER_ADD:
    case TGL_SHADER_MUL:
    case TGL_SHADER_LT:
        m = (1u << in->a) | (1u << in->b);
        break;
    case TGL_SHADER_MAD:
        m = (1u << in->a) | (1u << in->b) | (1u << in->c);
        break;
    default:
        break;
    }
    return m;
}

static unsigned inst_write_mask(const TGLShaderInst *in)
{
    if (op_writes_dst(in->op))
        return 1u << in->dst;
    return 0;
}

static int validate_definite_init(const TGLShaderProgram *p,
                                  TGLShaderError *err)
{
    unsigned n = p->ninst;
    unsigned def_in[TGL_SHADER_MAX_INST];
    unsigned def_out[TGL_SHADER_MAX_INST];
    unsigned reads[TGL_SHADER_MAX_INST];
    unsigned char succ[TGL_SHADER_MAX_INST][2];
    unsigned char nsucc[TGL_SHADER_MAX_INST];
    unsigned i;
    int changed, k;

    for (i = 0; i < n; i++) {
        const TGLShaderInst *in = &p->code[i];
        int ns = 0;
        reads[i] = inst_read_mask(in);
        def_out[i] = inst_write_mask(in);
        switch (in->op) {
        case TGL_SHADER_IF: {
            int endif = p->match[i];
            int els = p->else_idx[i];
            succ[i][ns++] = (unsigned char) (i + 1); /* then */
            /* false edge: into the else block or past the endif */
            succ[i][ns++] =
                (unsigned char) (els >= 0 ? els + 1 : endif + 1);
            break;
        }
        case TGL_SHADER_ELSE:
            /* falling through the then-block skips the else block */
            succ[i][ns++] = (unsigned char) (p->match[i] + 1);
            break;
        case TGL_SHADER_REPEAT:
            succ[i][ns++] = (unsigned char) (i + 1); /* body runs >= 1 */
            break;
        case TGL_SHADER_ENDREPEAT:
            succ[i][ns++] = (unsigned char) (p->match[i] + 1); /* iterate */
            succ[i][ns++] = (unsigned char) (i + 1);           /* exit */
            break;
        case TGL_SHADER_BREAK_IF:
            succ[i][ns++] = (unsigned char) (i + 1);
            /* exit past the ENDREPEAT of the innermost loop */
            succ[i][ns++] =
                (unsigned char) (p->match[p->loop_rep[i]] + 1);
            break;
        case TGL_SHADER_CONTINUE_IF:
            succ[i][ns++] = (unsigned char) (i + 1);
            /* jump to the ENDREPEAT of the innermost loop */
            succ[i][ns++] = (unsigned char) (p->match[p->loop_rep[i]]);
            break;
        case TGL_SHADER_OUTPUT:
            break; /* final instruction, no successors */
        default:
            succ[i][ns++] = (unsigned char) (i + 1);
            break;
        }
        nsucc[i] = (unsigned char) ns;
    }

    /* must-analysis: a register is defined at a point only when it is
     * defined on every feasible path reaching it (meet = intersection).
     * r0..r2 begin defined, temporaries do not. */
    def_in[0] = 0x7u;
    for (i = 1; i < n; i++)
        def_in[i] = 0xFFFFu;
    do {
        changed = 0;
        for (i = 0; i < n; i++) {
            unsigned out = def_in[i] | def_out[i];
            for (k = 0; k < nsucc[i]; k++) {
                unsigned s = succ[i][k];
                unsigned ni = def_in[s] & out;
                if (ni != def_in[s]) {
                    def_in[s] = ni;
                    changed = 1;
                }
            }
        }
    } while (changed);

    /* report the lowest original instruction index with an undefined read */
    for (i = 0; i < n; i++) {
        if (reads[i] & ~def_in[i]) {
            set_err(err, TGL_SHADER_ERR_UNDEF, (int) i);
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* public program lifecycle                                             */
/* ------------------------------------------------------------------ */

TGLShaderProgram *tglShaderCreate(const TGLShaderInst *code,
                                  unsigned ninst,
                                  TGLShaderError *err)
{
    TGLShaderProgram *p;
    unsigned i;

    if (!code || ninst < 1 || ninst > TGL_SHADER_MAX_INST) {
        set_err(err, TGL_SHADER_ERR_SIZE, ninst ? (int) (ninst - 1) : -1);
        return NULL;
    }

    p = (TGLShaderProgram *) malloc(sizeof(TGLShaderProgram));
    if (!p) {
        set_err(err, TGL_SHADER_ERR_SIZE, -1);
        return NULL;
    }
    p->code =
        (TGLShaderInst *) malloc(sizeof(TGLShaderInst) * (size_t) ninst);
    if (!p->code) {
        free(p);
        set_err(err, TGL_SHADER_ERR_SIZE, -1);
        return NULL;
    }
    memcpy(p->code, code, sizeof(TGLShaderInst) * (size_t) ninst);
    p->ninst = ninst;
    p->refcount = 1;
    p->uses_tex = 0;

    if (!validate_structure(p, p->code, ninst, err) ||
        !validate_definite_init(p, err)) {
        free(p->code);
        free(p);
        return NULL;
    }

    for (i = 0; i < ninst; i++)
        if (p->code[i].op == TGL_SHADER_TEX)
            p->uses_tex = 1;

    set_err(err, TGL_SHADER_OK, -1);
    return p;
}

void tglShaderRetain(TGLShaderProgram *prog)
{
    if (prog)
        prog->refcount++;
}

void tglShaderRelease(TGLShaderProgram *prog)
{
    if (!prog)
        return;
    if (--prog->refcount <= 0) {
        free(prog->code);
        free(prog);
    }
}

int tglShaderUsesTexture(const TGLShaderProgram *prog)
{
    return prog ? prog->uses_tex : 0;
}

/* ------------------------------------------------------------------ */
/* quad execution                                                       */
/* ------------------------------------------------------------------ */

typedef struct VMExec {
    const TGLShaderProgram *prog;
    float reg[4][TGL_SHADER_NUM_REGS][4]; /* [lane][reg][component] */
    float out[4][4];
    unsigned discarded;
    TGLShaderSampleFunc sample;
    void *sample_ctx;
    int error;
} VMExec;

typedef struct LoopSig {
    unsigned brk;  /* lanes that hit BREAK_IF for this loop */
    unsigned cont; /* lanes that hit CONTINUE_IF for this loop */
} LoopSig;

static void exec_range(VMExec *vm, int start, int end, unsigned active,
                       LoopSig *sig);

static int lane_finite4(const float v[4])
{
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) &&
           isfinite(v[3]);
}

/* execute one non-structural instruction for the lanes in active */
static void exec_inst(VMExec *vm, const TGLShaderInst *in, unsigned active)
{
    float res[4][4];
    int l, k;

    switch (in->op) {
    case TGL_SHADER_CONST:
        for (l = 0; l < 4; l++)
            if (active & (1u << l))
                for (k = 0; k < 4; k++)
                    res[l][k] = in->imm[k];
        break;
    case TGL_SHADER_MOV:
        for (l = 0; l < 4; l++)
            if (active & (1u << l))
                for (k = 0; k < 4; k++)
                    res[l][k] = vm->reg[l][in->a][k];
        break;
    case TGL_SHADER_ADD:
        for (l = 0; l < 4; l++)
            if (active & (1u << l))
                for (k = 0; k < 4; k++)
                    res[l][k] = vm->reg[l][in->a][k] + vm->reg[l][in->b][k];
        break;
    case TGL_SHADER_MUL:
        for (l = 0; l < 4; l++)
            if (active & (1u << l))
                for (k = 0; k < 4; k++)
                    res[l][k] = vm->reg[l][in->a][k] * vm->reg[l][in->b][k];
        break;
    case TGL_SHADER_MAD:
        for (l = 0; l < 4; l++)
            if (active & (1u << l))
                for (k = 0; k < 4; k++)
                    res[l][k] = vm->reg[l][in->a][k] * vm->reg[l][in->b][k] +
                                vm->reg[l][in->c][k];
        break;
    case TGL_SHADER_LT:
        for (l = 0; l < 4; l++)
            if (active & (1u << l))
                for (k = 0; k < 4; k++)
                    res[l][k] =
                        vm->reg[l][in->a][k] < vm->reg[l][in->b][k] ? 1.0f
                                                                    : 0.0f;
        break;
    case TGL_SHADER_TEX:
        for (l = 0; l < 4; l++) {
            if (!(active & (1u << l)))
                continue;
            {
                float u = vm->reg[l][in->a][0];
                float v = vm->reg[l][in->a][1];
                float fu, fv, rgba[4];
                int tx, ty;
                if (!isfinite(u) || !isfinite(v)) {
                    vm->error = TGL_SHADER_RT_NONFINITE;
                    return;
                }
                fu = u - floorf(u);
                fv = v - floorf(v);
                tx = (int) floorf(fu * (float) TGL_SHADER_TEXTURE_DIM);
                ty = (int) floorf(fv * (float) TGL_SHADER_TEXTURE_DIM);
                if (tx < 0)
                    tx = 0;
                if (tx > TGL_SHADER_TEXTURE_DIM - 1)
                    tx = TGL_SHADER_TEXTURE_DIM - 1;
                if (ty < 0)
                    ty = 0;
                if (ty > TGL_SHADER_TEXTURE_DIM - 1)
                    ty = TGL_SHADER_TEXTURE_DIM - 1;
                vm->sample(vm->sample_ctx, tx, ty, rgba);
                for (k = 0; k < 4; k++)
                    res[l][k] = rgba[k];
            }
        }
        break;
    case TGL_SHADER_DDX:
        /* right minus left in each row, same for both lanes of the row */
        for (k = 0; k < 4; k++) {
            float top = vm->reg[1][in->a][k] - vm->reg[0][in->a][k];
            float bot = vm->reg[3][in->a][k] - vm->reg[2][in->a][k];
            res[0][k] = top;
            res[1][k] = top;
            res[2][k] = bot;
            res[3][k] = bot;
        }
        break;
    case TGL_SHADER_DDY:
        /* bottom minus top in each column, same for both lanes of the
         * column */
        for (k = 0; k < 4; k++) {
            float left = vm->reg[2][in->a][k] - vm->reg[0][in->a][k];
            float right = vm->reg[3][in->a][k] - vm->reg[1][in->a][k];
            res[0][k] = left;
            res[2][k] = left;
            res[1][k] = right;
            res[3][k] = right;
        }
        break;
    case TGL_SHADER_DISCARD_IF:
        for (l = 0; l < 4; l++)
            if ((active & (1u << l)) && vm->reg[l][in->a][0] != 0.0f)
                vm->discarded |= (1u << l);
        return;
    case TGL_SHADER_OUTPUT:
        for (l = 0; l < 4; l++)
            if (active & (1u << l))
                for (k = 0; k < 4; k++)
                    vm->out[l][k] = vm->reg[l][in->a][k];
        return;
    default:
        return;
    }

    /* reject any non-finite produced value before committing */
    for (l = 0; l < 4; l++)
        if ((active & (1u << l)) && !lane_finite4(res[l])) {
            vm->error = TGL_SHADER_RT_NONFINITE;
            return;
        }
    for (l = 0; l < 4; l++)
        if (active & (1u << l))
            for (k = 0; k < 4; k++)
                vm->reg[l][in->dst][k] = res[l][k];
}

static void exec_range(VMExec *vm, int start, int end, unsigned active,
                       LoopSig *sig)
{
    const TGLShaderProgram *p = vm->prog;
    int i = start;

    while (i < end && active) {
        const TGLShaderInst *in = &p->code[i];
        switch (in->op) {
        case TGL_SHADER_IF: {
            int endif = p->match[i];
            int els = p->else_idx[i];
            int then_end = els >= 0 ? els : endif;
            unsigned cond = 0, tmask, fmask;
            int l;
            for (l = 0; l < 4; l++)
                if ((active & (1u << l)) && vm->reg[l][in->a][0] != 0.0f)
                    cond |= (1u << l);
            tmask = active & cond;
            fmask = active & ~cond;
            if (tmask)
                exec_range(vm, i + 1, then_end, tmask, sig);
            if (vm->error)
                return;
            if (els >= 0 && fmask)
                exec_range(vm, els + 1, endif, fmask, sig);
            if (vm->error)
                return;
            if (sig)
                active &= ~(sig->brk | sig->cont);
            i = endif + 1;
            break;
        }
        case TGL_SHADER_REPEAT: {
            int body_end = p->match[i];
            unsigned broken = 0;
            unsigned it;
            for (it = 0; it < in->count; it++) {
                unsigned run = active & ~broken;
                LoopSig ls;
                if (!run)
                    break;
                ls.brk = 0;
                ls.cont = 0;
                exec_range(vm, i + 1, body_end, run, &ls);
                if (vm->error)
                    return;
                broken |= ls.brk;
                /* continued lanes simply re-enter the next iteration */
            }
            if (sig)
                active &= ~(sig->brk | sig->cont);
            i = body_end + 1;
            break;
        }
        case TGL_SHADER_BREAK_IF:
        case TGL_SHADER_CONTINUE_IF: {
            unsigned cond = 0;
            int l;
            for (l = 0; l < 4; l++)
                if ((active & (1u << l)) && vm->reg[l][in->a][0] != 0.0f)
                    cond |= (1u << l);
            if (sig) {
                if (in->op == TGL_SHADER_BREAK_IF)
                    sig->brk |= cond;
                else
                    sig->cont |= cond;
                active &= ~cond;
            }
            i++;
            break;
        }
        default:
            exec_inst(vm, in, active);
            if (vm->error)
                return;
            i++;
            break;
        }
    }
}

int tglShaderExecQuad(const TGLShaderProgram *prog,
                      const float in[4][TGL_SHADER_NUM_INPUTS][4],
                      float out[4][4],
                      unsigned coverage,
                      unsigned *writemask,
                      TGLShaderSampleFunc sample,
                      void *sample_ctx)
{
    VMExec vm;
    int l, r, k;

    if (!prog || !in || !out || !writemask)
        return TGL_SHADER_RT_STATE;
    if (prog->uses_tex && !sample)
        return TGL_SHADER_RT_NO_TEXTURE;

    memset(&vm, 0, sizeof(vm));
    vm.prog = prog;
    vm.sample = sample;
    vm.sample_ctx = sample_ctx;
    vm.discarded = 0;
    vm.error = TGL_SHADER_RT_NONE;
    for (l = 0; l < 4; l++)
        for (r = 0; r < TGL_SHADER_NUM_INPUTS; r++)
            for (k = 0; k < 4; k++)
                vm.reg[l][r][k] = in[l][r][k];

    exec_range(&vm, 0, (int) prog->ninst, 0xF, NULL);
    if (vm.error)
        return vm.error;

    for (l = 0; l < 4; l++)
        for (k = 0; k < 4; k++)
            out[l][k] = vm.out[l][k];
    *writemask = (coverage & 0xFu) & ~vm.discarded;
    return TGL_SHADER_RT_NONE;
}
