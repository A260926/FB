/*
 * Fragment bytecode programs: creation-time validation, lifecycle
 * (create/bind/release), and the production quad rasterizer that runs a
 * bound program during GL_SMOOTH filled-triangle drawing.
 *
 * Validation happens once at creation and has three stages, in order:
 *   1. per-instruction operand and structured control-flow checks
 *      (first invalid instruction is reported),
 *   2. the REPEAT expansion budget (4096 expanded instructions),
 *   3. definite initialization analysis over the structured control flow:
 *      every register read must be defined on every feasible path.
 * The error report carries the original instruction index and a reason
 * code; a failed creation never changes the current binding.
 */

#include <math.h>
#include <string.h>

#include "zgl.h"
#include "shader_int.h"

#if TGL_FEATURE_RENDER_BITS != 32
#error "src/shader.c requires TGL_FEATURE_RENDER_BITS == 32"
#endif

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static GLint src_reg_ok(GLint r)
{
    return r >= 0 && r < TGL_SHADER_NUM_REGS;
}

static GLint dst_reg_ok(GLint r)
{
    return r >= TGL_SHADER_FIRST_TEMP && r < TGL_SHADER_NUM_REGS;
}

static GLint fail(TGLShaderError *err, GLint index, GLint code)
{
    err->index = index;
    err->code = code;
    return code;
}

/* Stage 1: operands and structure.  Also fills match[]/else_idx[]. */
static GLint shader_check_structure(const TGLShaderInstr *code, GLint n,
                                    GLint *match, GLint *else_idx,
                                    TGLShaderError *err)
{
    GLint depth = 0, i, d;
    GLint st_type[TGL_SHADER_MAX_DEPTH];
    GLint st_idx[TGL_SHADER_MAX_DEPTH];
    GLint st_else[TGL_SHADER_MAX_DEPTH];
    GLint output_seen = 0;

    for (i = 0; i < n; i++) {
        match[i] = -1;
        else_idx[i] = -1;
    }

    for (i = 0; i < n; i++) {
        const TGLShaderInstr *in = &code[i];
        switch (in->op) {
        case CONST:
            if (!dst_reg_ok(in->dst))
                return fail(err, i, TGL_SHADER_ERR_OPERAND);
            if (!isfinite(in->imm[0]) || !isfinite(in->imm[1]) ||
                !isfinite(in->imm[2]) || !isfinite(in->imm[3]))
                return fail(err, i, TGL_SHADER_ERR_IMMEDIATE);
            break;
        case MOV:
        case TEX:
            if (!dst_reg_ok(in->dst) || !src_reg_ok(in->a))
                return fail(err, i, TGL_SHADER_ERR_OPERAND);
            break;
        case ADD:
        case MUL:
        case LT:
            if (!dst_reg_ok(in->dst) || !src_reg_ok(in->a) ||
                !src_reg_ok(in->b))
                return fail(err, i, TGL_SHADER_ERR_OPERAND);
            break;
        case MAD:
            if (!dst_reg_ok(in->dst) || !src_reg_ok(in->a) ||
                !src_reg_ok(in->b) || !src_reg_ok(in->c))
                return fail(err, i, TGL_SHADER_ERR_OPERAND);
            break;
        case DDX:
        case DDY:
            if (!dst_reg_ok(in->dst) || !src_reg_ok(in->a))
                return fail(err, i, TGL_SHADER_ERR_OPERAND);
            if (depth != 0)
                return fail(err, i, TGL_SHADER_ERR_DERIVATIVE);
            break;
        case IF:
            if (!src_reg_ok(in->a))
                return fail(err, i, TGL_SHADER_ERR_OPERAND);
            if (depth >= TGL_SHADER_MAX_DEPTH)
                return fail(err, i, TGL_SHADER_ERR_STRUCTURE);
            st_type[depth] = IF;
            st_idx[depth] = i;
            st_else[depth] = 0;
            depth++;
            break;
        case ELSE:
            if (depth == 0 || st_type[depth - 1] != IF ||
                st_else[depth - 1])
                return fail(err, i, TGL_SHADER_ERR_STRUCTURE);
            st_else[depth - 1] = 1;
            else_idx[st_idx[depth - 1]] = i;
            match[i] = st_idx[depth - 1];
            break;
        case ENDIF:
            if (depth == 0 || st_type[depth - 1] != IF)
                return fail(err, i, TGL_SHADER_ERR_STRUCTURE);
            depth--;
            match[st_idx[depth]] = i;
            match[i] = st_idx[depth];
            break;
        case REPEAT:
            if (in->count < 1 || in->count > TGL_SHADER_MAX_REPEAT)
                return fail(err, i, TGL_SHADER_ERR_REPEAT_COUNT);
            if (depth >= TGL_SHADER_MAX_DEPTH)
                return fail(err, i, TGL_SHADER_ERR_STRUCTURE);
            st_type[depth] = REPEAT;
            st_idx[depth] = i;
            st_else[depth] = 0;
            depth++;
            break;
        case ENDREPEAT:
            if (depth == 0 || st_type[depth - 1] != REPEAT)
                return fail(err, i, TGL_SHADER_ERR_STRUCTURE);
            depth--;
            match[st_idx[depth]] = i;
            match[i] = st_idx[depth];
            break;
        case BREAK_IF:
        case CONTINUE_IF:
            if (!src_reg_ok(in->a))
                return fail(err, i, TGL_SHADER_ERR_OPERAND);
            for (d = depth - 1; d >= 0; d--)
                if (st_type[d] == REPEAT)
                    break;
            if (d < 0)
                return fail(err, i, TGL_SHADER_ERR_STRUCTURE);
            break;
        case DISCARD_IF:
            if (!src_reg_ok(in->a))
                return fail(err, i, TGL_SHADER_ERR_OPERAND);
            break;
        case OUTPUT:
            if (!src_reg_ok(in->a))
                return fail(err, i, TGL_SHADER_ERR_OPERAND);
            if (depth != 0 || i != n - 1)
                return fail(err, i, TGL_SHADER_ERR_OUTPUT);
            output_seen = 1;
            break;
        default:
            return fail(err, i, TGL_SHADER_ERR_OPCODE);
        }
    }
    if (depth != 0)
        return fail(err, st_idx[0], TGL_SHADER_ERR_UNCLOSED);
    if (!output_seen)
        return fail(err, n - 1, TGL_SHADER_ERR_OUTPUT);
    return TGL_SHADER_ERR_NONE;
}

/* Stage 2: expansion budget.  Every REPEAT is expanded syntactically; all
 * instructions count except the REPEAT/ENDREPEAT markers themselves. */
static GLint shader_check_budget(const TGLShaderInstr *code, GLint n,
                                 TGLShaderError *err)
{
    unsigned long long total = 0, mult = 1;
    unsigned long long mstack[TGL_SHADER_MAX_DEPTH];
    GLint rstack[TGL_SHADER_MAX_DEPTH];
    GLint depth = 0, i;

    for (i = 0; i < n; i++) {
        GLint op = code[i].op;
        if (op == REPEAT) {
            mstack[depth] = mult;
            rstack[depth] = i;
            depth++;
            mult *= code[i].count; /* <= 16^8, cannot overflow u64 */
        } else if (op == ENDREPEAT) {
            depth--;
            mult = mstack[depth];
        } else {
            total += mult;
            if (total > TGL_SHADER_BUDGET)
                return fail(err, depth ? rstack[depth - 1] : n - 1,
                            TGL_SHADER_ERR_BUDGET);
        }
    }
    return TGL_SHADER_ERR_NONE;
}

/* Stage 3: definite initialization.  mask is the bitmask of registers
 * defined on every feasible path; joins accumulate (via AND) the states
 * that can leave the innermost loop through BREAK_IF/CONTINUE_IF. */
typedef struct ShaderLoopJoin {
    GLuint break_join;
    GLuint cont_join;
} ShaderLoopJoin;

static GLint shader_di_range(const TGLShaderProgram *p, GLint pc, GLint end,
                             GLuint *mask, ShaderLoopJoin *loop,
                             TGLShaderError *err)
{
    while (pc < end) {
        const TGLShaderInstr *in = &p->code[pc];
        GLuint need = 0;
        switch (in->op) {
        case MOV:
        case TEX:
        case DDX:
        case DDY:
        case IF:
        case BREAK_IF:
        case CONTINUE_IF:
        case DISCARD_IF:
        case OUTPUT:
            need = 1u << in->a;
            break;
        case ADD:
        case MUL:
        case LT:
            need = (1u << in->a) | (1u << in->b);
            break;
        case MAD:
            need = (1u << in->a) | (1u << in->b) | (1u << in->c);
            break;
        default:
            break;
        }
        if (need & ~*mask)
            return fail(err, pc, TGL_SHADER_ERR_UNDEFINED);

        switch (in->op) {
        case CONST:
        case MOV:
        case ADD:
        case MUL:
        case MAD:
        case LT:
        case TEX:
        case DDX:
        case DDY:
            *mask |= 1u << in->dst;
            pc++;
            break;
        case IF: {
            GLint endif = p->match[pc];
            GLint elsep = p->else_idx[pc];
            GLuint then_m = *mask, else_m = *mask;
            if (shader_di_range(p, pc + 1, elsep >= 0 ? elsep : endif,
                                &then_m, loop, err))
                return err->code;
            if (elsep >= 0 &&
                shader_di_range(p, elsep + 1, endif, &else_m, loop, err))
                return err->code;
            *mask = then_m & else_m;
            pc = endif + 1;
        } break;
        case REPEAT: {
            /* The body runs at least once and may break/continue in its
             * first iteration, so the loop-exit state is the meet of the
             * body fallthrough and every break/continue point, all reached
             * from the loop-entry state. */
            GLint endrep = p->match[pc];
            ShaderLoopJoin inner;
            GLuint body = *mask;
            inner.break_join = 0xFFFFu;
            inner.cont_join = 0xFFFFu;
            if (shader_di_range(p, pc + 1, endrep, &body, &inner, err))
                return err->code;
            *mask = body & inner.break_join & inner.cont_join;
            pc = endrep + 1;
        } break;
        case BREAK_IF:
            if (loop)
                loop->break_join &= *mask;
            pc++;
            break;
        case CONTINUE_IF:
            if (loop)
                loop->cont_join &= *mask;
            pc++;
            break;
        default:
            pc++;
            break;
        }
    }
    return TGL_SHADER_ERR_NONE;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

TGLShaderProgram *tglShaderCreate(const TGLShaderInstr *code, GLuint ninstr,
                                  TGLShaderError *err)
{
    TGLShaderError local_err;
    TGLShaderProgram *p;
    GLubyte *mem;
    GLint rc;
    GLuint di_mask;
    size_t sz;
    GLuint i;

    if (!err)
        err = &local_err;
    err->index = -1;
    err->code = TGL_SHADER_ERR_NONE;

    if (!code || ninstr < 1 || ninstr > TGL_SHADER_MAX_INSTR) {
        fail(err, -1, TGL_SHADER_ERR_ARGUMENT);
        return NULL;
    }

    p = (TGLShaderProgram *) gl_zalloc(sizeof(TGLShaderProgram));
    if (!p) {
        fail(err, -1, TGL_SHADER_ERR_MEMORY);
        return NULL;
    }
    /* one block: instruction copy + match[] + else_idx[] */
    sz = (size_t) ninstr *
         (sizeof(TGLShaderInstr) + 2 * sizeof(GLint));
    mem = (GLubyte *) gl_malloc(sz);
    if (!mem) {
        gl_free(p);
        fail(err, -1, TGL_SHADER_ERR_MEMORY);
        return NULL;
    }
    p->code = (TGLShaderInstr *) mem;
    p->match = (GLint *) (mem + ninstr * sizeof(TGLShaderInstr));
    p->else_idx = p->match + ninstr;
    p->ninstr = ninstr;
    memcpy(p->code, code, ninstr * sizeof(TGLShaderInstr));

    rc = shader_check_structure(p->code, (GLint) ninstr, p->match,
                                p->else_idx, err);
    if (rc == TGL_SHADER_ERR_NONE)
        rc = shader_check_budget(p->code, (GLint) ninstr, err);
    if (rc == TGL_SHADER_ERR_NONE) {
        di_mask = 0x7u; /* r0..r2 begin defined */
        rc = shader_di_range(p, 0, (GLint) ninstr, &di_mask, NULL, err);
    }
    if (rc != TGL_SHADER_ERR_NONE) {
        gl_free(mem);
        gl_free(p);
        return NULL;
    }

    p->has_tex = 0;
    for (i = 0; i < ninstr; i++)
        if (p->code[i].op == TEX)
            p->has_tex = 1;
    p->refcount = 1;
    return p;
}

void tglShaderRelease(TGLShaderProgram *prog)
{
    if (!prog)
        return;
    if (--prog->refcount == 0) {
        gl_free(prog->code); /* single block: code/match/else_idx */
        gl_free(prog);
    }
}

void tglShaderBind(TGLShaderProgram *prog)
{
    GLContext *c = gl_get_context();
    if (c->shader_program == prog)
        return;
    if (prog)
        prog->refcount++;
    if (c->shader_program)
        tglShaderRelease(c->shader_program);
    c->shader_program = prog;
}

TGLShaderProgram *tglShaderGetBound(void)
{
    return gl_get_context()->shader_program;
}

GLint tglShaderGetError(void)
{
    GLContext *c = gl_get_context();
    GLint e = c->shader_error;
    c->shader_error = TGL_SHADER_EXEC_OK;
    return e;
}

/* ------------------------------------------------------------------ */
/* Production path                                                     */
/* ------------------------------------------------------------------ */

static void shader_set_error(GLContext *c, GLint e)
{
    if (c->shader_error == TGL_SHADER_EXEC_OK)
        c->shader_error = e;
}

void tgl_shader_reject(GLContext *c)
{
    shader_set_error(c, TGL_SHADER_EXEC_UNSUPPORTED);
}

void tgl_shader_context_shutdown(GLContext *c)
{
    if (c->shader_program)
        tglShaderRelease(c->shader_program);
    c->shader_program = NULL;
    c->shader_error = TGL_SHADER_EXEC_OK;
}

static GLuint shader_to_byte(GLfloat v)
{
    if (v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 255;
    return (GLuint) (v * 255.0f + 0.5f);
}

#define SHADER_GRADIENT(Q, gx, gy)                     \
    do {                                               \
        GLfloat d1_ = Q[1] - Q[0];                     \
        GLfloat d2_ = Q[2] - Q[0];                     \
        gx = (fdy2 * d1_ - fdy1 * d2_) * fz;           \
        gy = (fdx1 * d2_ - fdx2 * d1_) * fz;           \
    } while (0)

/*
 * Rasterize one clipped triangle with the bound program.  Coverage uses
 * pixel centers and half-open edges; quads are aligned to even screen
 * coordinates (lane order TL, TR, BL, BR).  Lanes not covered by this
 * triangle are helpers: their inputs are extrapolated from this triangle's
 * own attribute planes.  Only quads inside the bounding box with at least
 * one covered lane execute.  The depth test runs after the program with the
 * original comparison; helpers and discarded lanes never write.
 */
void tgl_shader_fill_triangle(GLContext *c, GLVertex *pv0, GLVertex *pv1,
                              GLVertex *pv2)
{
    TGLShaderProgram *prog = c->shader_program;
    ZBuffer *zb = c->zb;
    GLVertex *vs[3];
    const GLuint *texels = NULL;
    GLfloat x[3], y[3], zf[3], wi[3], nz[3], uo[3], vo[3], co[3][4];
    GLfloat fdx1, fdy1, fdx2, fdy2, area, fz;
    GLfloat d_wi_x, d_wi_y, d_nz_x, d_nz_y, d_zf_x, d_zf_y;
    GLfloat d_uo_x, d_uo_y, d_vo_x, d_vo_y;
    GLfloat d_co_x[4], d_co_y[4];
    GLfloat ex[3], ey[3], minx, maxx, miny, maxy, t;
    GLint topleft[3];
    GLint i, j, k, px0, px1, py0, py1, qx, qy;

    /* Program mode supports GL_SMOOTH filled triangles only; blending and
     * other configurations are explicitly rejected. */
    if (c->current_shade_model != GL_SMOOTH || zb->enable_blend) {
        shader_set_error(c, TGL_SHADER_EXEC_UNSUPPORTED);
        return;
    }
    if (prog->has_tex) {
        if (!c->texture_2d_enabled || !c->current_texture) {
            shader_set_error(c, TGL_SHADER_EXEC_NO_TEXTURE);
            return;
        }
        texels = (const GLuint *) c->current_texture->images[0].pixmap;
    }

    vs[0] = pv0;
    vs[1] = pv1;
    vs[2] = pv2;
    for (i = 0; i < 3; i++) {
        GLVertex *v = vs[i];
        GLfloat w = v->pc.W;
        x[i] = (GLfloat) v->zp.x;
        y[i] = (GLfloat) v->zp.y;
        zf[i] = (GLfloat) v->zp.z;
        wi[i] = 1.0f / w;
        nz[i] = v->pc.Z / w;
        uo[i] = v->tex_coord.X * wi[i];
        vo[i] = v->tex_coord.Y * wi[i];
        for (k = 0; k < 4; k++)
            co[i][k] = v->color.v[k] * wi[i];
    }

    /* orient so the signed area is positive */
    fdx1 = x[1] - x[0];
    fdy1 = y[1] - y[0];
    fdx2 = x[2] - x[0];
    fdy2 = y[2] - y[0];
    area = fdx1 * fdy2 - fdx2 * fdy1;
    if (area == 0.0f)
        return;
    if (area < 0.0f) {
        /* swap vertices 1 and 2 in every attribute array */
        t = x[1]; x[1] = x[2]; x[2] = t;
        t = y[1]; y[1] = y[2]; y[2] = t;
        t = zf[1]; zf[1] = zf[2]; zf[2] = t;
        t = wi[1]; wi[1] = wi[2]; wi[2] = t;
        t = nz[1]; nz[1] = nz[2]; nz[2] = t;
        t = uo[1]; uo[1] = uo[2]; uo[2] = t;
        t = vo[1]; vo[1] = vo[2]; vo[2] = t;
        for (k = 0; k < 4; k++) {
            t = co[1][k];
            co[1][k] = co[2][k];
            co[2][k] = t;
        }
        fdx1 = x[1] - x[0];
        fdy1 = y[1] - y[0];
        fdx2 = x[2] - x[0];
        fdy2 = y[2] - y[0];
        area = -area;
    }
    fz = 1.0f / area;

    SHADER_GRADIENT(wi, d_wi_x, d_wi_y);
    SHADER_GRADIENT(nz, d_nz_x, d_nz_y);
    SHADER_GRADIENT(zf, d_zf_x, d_zf_y);
    SHADER_GRADIENT(uo, d_uo_x, d_uo_y);
    SHADER_GRADIENT(vo, d_vo_x, d_vo_y);
    for (k = 0; k < 4; k++) {
        GLfloat d1_ = co[1][k] - co[0][k];
        GLfloat d2_ = co[2][k] - co[0][k];
        d_co_x[k] = (fdy2 * d1_ - fdy1 * d2_) * fz;
        d_co_y[k] = (fdx1 * d2_ - fdx2 * d1_) * fz;
    }

    /* bounding box in pixel space (pixel centers inside the box);
     * clamp to a sane range first so the float->int rounding is safe */
    minx = maxx = x[0];
    miny = maxy = y[0];
    for (i = 1; i < 3; i++) {
        if (x[i] < minx) minx = x[i];
        if (x[i] > maxx) maxx = x[i];
        if (y[i] < miny) miny = y[i];
        if (y[i] > maxy) maxy = y[i];
    }
    {
        GLfloat xlim = (GLfloat) zb->xsize + 1.0f;
        GLfloat ylim = (GLfloat) zb->ysize + 1.0f;
        minx = clampf(minx, -xlim, xlim);
        maxx = clampf(maxx, -xlim, xlim);
        miny = clampf(miny, -ylim, ylim);
        maxy = clampf(maxy, -ylim, ylim);
    }
    px0 = (GLint) ceilf(minx - 0.5f);
    px1 = (GLint) floorf(maxx - 0.5f);
    py0 = (GLint) ceilf(miny - 0.5f);
    py1 = (GLint) floorf(maxy - 0.5f);
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > zb->xsize - 1) px1 = zb->xsize - 1;
    if (py1 > zb->ysize - 1) py1 = zb->ysize - 1;
    if (px0 > px1 || py0 > py1)
        return;

    /* edge vectors and the half-open (top-left) tie rule */
    for (i = 0; i < 3; i++) {
        j = (i + 1) % 3;
        ex[i] = x[j] - x[i];
        ey[i] = y[j] - y[i];
        topleft[i] = (ey[i] > 0.0f) || (ey[i] == 0.0f && ex[i] > 0.0f);
    }

    for (qy = py0 & ~1; qy <= py1; qy += 2) {
        for (qx = px0 & ~1; qx <= px1; qx += 2) {
            GLuint coverage = 0;
            GLfloat cx[4], cy[4], zbufv[4];
            GLfloat in[4][TGL_SHADER_INPUT_REGS][4];
            GLfloat out[4][4];
            GLuint discard = 0;
            GLint err = TGL_SHADER_EXEC_OK;
            GLint lane, e;

            for (lane = 0; lane < 4; lane++) {
                GLint lx = qx + (lane & 1);
                GLint ly = qy + (lane >> 1);
                cx[lane] = (GLfloat) lx + 0.5f;
                cy[lane] = (GLfloat) ly + 0.5f;
                if (lx < 0 || lx >= zb->xsize || ly < 0 || ly >= zb->ysize)
                    continue;
                for (e = 0; e < 3; e++) {
                    GLfloat ef = ex[e] * (cy[lane] - y[e]) -
                                 ey[e] * (cx[lane] - x[e]);
                    if (ef < 0.0f || (ef == 0.0f && !topleft[e]))
                        break;
                }
                if (e == 3)
                    coverage |= 1u << lane;
            }
            if (!coverage)
                continue;

            /* build r0..r2 for every lane, helpers extrapolated */
            for (lane = 0; lane < 4; lane++) {
                GLfloat dx = cx[lane] - x[0];
                GLfloat dy = cy[lane] - y[0];
                GLfloat wq = wi[0] + d_wi_x * dx + d_wi_y * dy;
                GLfloat uq, vq, zq;
                if (wq == 0.0f) {
                    err = TGL_SHADER_EXEC_ZERO_DIVIDE;
                    break;
                }
                if (!isfinite(wq)) {
                    err = TGL_SHADER_EXEC_NONFINITE;
                    break;
                }
                uq = (uo[0] + d_uo_x * dx + d_uo_y * dy) / wq;
                vq = (vo[0] + d_vo_x * dx + d_vo_y * dy) / wq;
                zq = nz[0] + d_nz_x * dx + d_nz_y * dy;
                zbufv[lane] = zf[0] + d_zf_x * dx + d_zf_y * dy;
                if (!isfinite(zbufv[lane])) {
                    /* never convert a bad depth to an integer */
                    err = TGL_SHADER_EXEC_NONFINITE;
                    break;
                }
                in[lane][0][0] = uq;
                in[lane][0][1] = vq;
                in[lane][0][2] = 0.0f;
                in[lane][0][3] = 1.0f;
                for (k = 0; k < 4; k++)
                    in[lane][1][k] =
                        (co[0][k] + d_co_x[k] * dx + d_co_y[k] * dy) / wq;
                in[lane][2][0] = cx[lane];
                in[lane][2][1] = cy[lane];
                in[lane][2][2] = (zq + 1.0f) * 0.5f;
                in[lane][2][3] = wq;
            }
            if (err != TGL_SHADER_EXEC_OK) {
                /* a bad quad commits nothing; earlier quads stand */
                shader_set_error(c, err);
                return;
            }

            err = tgl_shader_vm_run(prog, texels, in, out, &discard);
            if (err != TGL_SHADER_EXEC_OK) {
                shader_set_error(c, err);
                return;
            }

            /* depth test after the shader; commit surviving lanes */
            {
                GLuint wmask = coverage & ~discard;
                for (lane = 0; lane < 4; lane++) {
                    GLint lx, ly;
                    GLint zi;
                    GLuint zz;
                    GLushort *pz;
                    PIXEL *pp;
                    if (!(wmask & (1u << lane)))
                        continue;
                    lx = qx + (lane & 1);
                    ly = qy + (lane >> 1);
                    zi = (GLint) zbufv[lane];
                    zz = ((GLuint) zi) >> ZB_POINT_Z_FRAC_BITS;
                    pz = zb->zbuf + ly * zb->xsize + lx;
                    if (zb->depth_test && !(zz >= (GLuint) *pz))
                        continue;
                    pp = zb->pbuf + ly * zb->xsize + lx;
                    *pp = (PIXEL) ((shader_to_byte(out[lane][0]) << 16) |
                                   (shader_to_byte(out[lane][1]) << 8) |
                                   shader_to_byte(out[lane][2]));
                    if (zb->depth_write)
                        *pz = (GLushort) zz;
                }
            }
#if TGL_HAS(DIRTY_RECTANGLE)
            ZB_markDirty(zb, qx, qy, qx + 1, qy + 1);
#endif
        }
    }
}
