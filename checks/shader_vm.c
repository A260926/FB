/*
 * checks/shader_vm.c — verify / control / render checks for the TinyGL
 * fragment bytecode shader (include/TGL/glshader.h).
 *
 * Exports task_cases/task_case_count for task_support/driver.c; no main().
 */

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <TGL/gl.h>
#include <TGL/glshader.h>
#include "zbuffer.h"
#include "../task_support/check.h"

/* ================================================================== */
/* shared helpers                                                      */
/* ================================================================== */

static TGLShaderInstr instr(GLint op, GLint dst, GLint a, GLint b, GLint c)
{
    TGLShaderInstr in;
    in.op = op;
    in.dst = dst;
    in.a = a;
    in.b = b;
    in.c = c;
    in.imm[0] = in.imm[1] = in.imm[2] = in.imm[3] = 0.0f;
    in.count = 0;
    return in;
}

static TGLShaderInstr iconst(GLint dst, GLfloat x, GLfloat y, GLfloat z,
                             GLfloat w)
{
    TGLShaderInstr in = instr(CONST, dst, 0, 0, 0);
    in.imm[0] = x;
    in.imm[1] = y;
    in.imm[2] = z;
    in.imm[3] = w;
    return in;
}

static TGLShaderInstr irepeat(GLuint count)
{
    TGLShaderInstr in = instr(REPEAT, 0, 0, 0, 0);
    in.count = count;
    return in;
}

static GLint create_fails(const TGLShaderInstr *code, GLuint n,
                          GLint want_code, GLint want_index)
{
    TGLShaderError err;
    TGLShaderProgram *p = tglShaderCreate(code, n, &err);
    if (p) {
        tglShaderRelease(p);
        return 0;
    }
    return err.code == want_code && err.index == want_index;
}

static TGLShaderProgram *create_ok(const TGLShaderInstr *code, GLuint n)
{
    TGLShaderError err;
    return tglShaderCreate(code, n, &err);
}

/* ================================================================== */
/* verify group: creation-time validation                              */
/* ================================================================== */

static int verify_lifecycle(void)
{
    TGLShaderInstr code[3];
    TGLShaderError err;
    TGLShaderProgram *p;
    ZBuffer *zb;

    code[0] = iconst(3, 0.25f, 0.5f, 0.75f, 1.0f);
    code[1] = instr(MOV, 4, 3, 0, 0);
    code[2] = instr(OUTPUT, 0, 4, 0, 0);

    err.index = -2;
    err.code = -2;
    p = tglShaderCreate(code, 3, &err);
    TASK_CHECK(p != NULL);
    TASK_CHECK(err.code == TGL_SHADER_ERR_NONE);

    /* bad arguments */
    TASK_CHECK(tglShaderCreate(NULL, 3, &err) == NULL);
    TASK_CHECK(err.code == TGL_SHADER_ERR_ARGUMENT);
    TASK_CHECK(tglShaderCreate(code, 0, &err) == NULL);
    TASK_CHECK(err.code == TGL_SHADER_ERR_ARGUMENT);
    TASK_CHECK(tglShaderCreate(code, 257, &err) == NULL);
    TASK_CHECK(err.code == TGL_SHADER_ERR_ARGUMENT);

    /* binding holds a reference; the caller may drop its handle */
    zb = ZB_open(8, 8, ZB_MODE_RGBA, NULL);
    TASK_CHECK(zb != NULL);
    glInit(zb);
    TASK_CHECK(tglShaderGetBound() == NULL);
    tglShaderBind(p);
    TASK_CHECK(tglShaderGetBound() == p);
    tglShaderRelease(p); /* caller handle gone, binding keeps it alive */
    TASK_CHECK(tglShaderGetBound() == p);
    tglShaderBind(NULL);
    TASK_CHECK(tglShaderGetBound() == NULL);
    /* p was freed by the unbind; bind/release of NULL are no-ops */
    tglShaderBind(NULL);
    tglShaderRelease(NULL);
    glClose();
    ZB_close(zb);
    return 0;
}

static int verify_both_sides_read(void)
{
    /* r3 assigned only on the then side: read after ENDIF is undefined */
    TGLShaderInstr bad[6];
    TGLShaderInstr good[7];
    TGLShaderProgram *p;

    bad[0] = instr(IF, 0, 0, 0, 0);
    bad[1] = iconst(3, 1, 1, 1, 1);
    bad[2] = instr(ELSE, 0, 0, 0, 0);
    bad[3] = instr(ENDIF, 0, 0, 0, 0);
    bad[4] = instr(MOV, 4, 3, 0, 0);
    bad[5] = instr(OUTPUT, 0, 4, 0, 0);
    TASK_CHECK(create_fails(bad, 6, TGL_SHADER_ERR_UNDEFINED, 4));

    good[0] = instr(IF, 0, 0, 0, 0);
    good[1] = iconst(3, 1, 1, 1, 1);
    good[2] = instr(ELSE, 0, 0, 0, 0);
    good[3] = iconst(3, 2, 2, 2, 2);
    good[4] = instr(ENDIF, 0, 0, 0, 0);
    good[5] = instr(MOV, 4, 3, 0, 0);
    good[6] = instr(OUTPUT, 0, 4, 0, 0);
    p = create_ok(good, 7);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);
    return 0;
}

static int verify_missing_else(void)
{
    /* without ELSE the not-taken path leaves r3 undefined */
    TGLShaderInstr code[5];
    code[0] = instr(IF, 0, 0, 0, 0);
    code[1] = iconst(3, 1, 1, 1, 1);
    code[2] = instr(ENDIF, 0, 0, 0, 0);
    code[3] = instr(MOV, 4, 3, 0, 0);
    code[4] = instr(OUTPUT, 0, 4, 0, 0);
    TASK_CHECK(create_fails(code, 5, TGL_SHADER_ERR_UNDEFINED, 3));
    return 0;
}

static int verify_break_continue_skip(void)
{
    /* BREAK_IF before the definition: the loop may exit first */
    TGLShaderInstr brk[6];
    TGLShaderInstr cont[6];
    TGLShaderInstr ok[6];
    TGLShaderInstr nested[7];
    TGLShaderProgram *p;

    brk[0] = irepeat(2);
    brk[1] = instr(BREAK_IF, 0, 0, 0, 0);
    brk[2] = iconst(3, 1, 1, 1, 1);
    brk[3] = instr(ENDREPEAT, 0, 0, 0, 0);
    brk[4] = instr(MOV, 4, 3, 0, 0);
    brk[5] = instr(OUTPUT, 0, 4, 0, 0);
    TASK_CHECK(create_fails(brk, 6, TGL_SHADER_ERR_UNDEFINED, 4));

    cont[0] = irepeat(2);
    cont[1] = instr(CONTINUE_IF, 0, 0, 0, 0);
    cont[2] = iconst(3, 1, 1, 1, 1);
    cont[3] = instr(ENDREPEAT, 0, 0, 0, 0);
    cont[4] = instr(MOV, 4, 3, 0, 0);
    cont[5] = instr(OUTPUT, 0, 4, 0, 0);
    TASK_CHECK(create_fails(cont, 6, TGL_SHADER_ERR_UNDEFINED, 4));

    /* definition dominates the break: valid */
    ok[0] = irepeat(2);
    ok[1] = iconst(3, 1, 1, 1, 1);
    ok[2] = instr(BREAK_IF, 0, 0, 0, 0);
    ok[3] = instr(ENDREPEAT, 0, 0, 0, 0);
    ok[4] = instr(MOV, 4, 3, 0, 0);
    ok[5] = instr(OUTPUT, 0, 4, 0, 0);
    p = create_ok(ok, 6);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);

    /* a break inside a conditional still bypasses the later assignment:
     * r4 is defined after the BREAK_IF, so it is undefined at loop exit */
    nested[0] = irepeat(3);
    nested[1] = iconst(3, 1, 1, 1, 1);
    nested[2] = instr(IF, 0, 0, 0, 0);
    nested[3] = instr(BREAK_IF, 0, 1, 0, 0);
    nested[4] = instr(ENDIF, 0, 0, 0, 0);
    nested[5] = iconst(4, 2, 2, 2, 2);
    nested[6] = instr(ENDREPEAT, 0, 0, 0, 0);
    /* reading r4 after the loop must fail; append MOV+OUTPUT */
    {
        TGLShaderInstr prog2[9];
        memcpy(prog2, nested, sizeof nested);
        prog2[7] = instr(MOV, 5, 4, 0, 0);
        prog2[8] = instr(OUTPUT, 0, 5, 0, 0);
        TASK_CHECK(create_fails(prog2, 9, TGL_SHADER_ERR_UNDEFINED, 7));
    }
    /* but reading r3 (defined before the break) is fine */
    {
        TGLShaderInstr prog3[9];
        memcpy(prog3, nested, sizeof nested);
        prog3[7] = instr(MOV, 5, 3, 0, 0);
        prog3[8] = instr(OUTPUT, 0, 5, 0, 0);
        p = create_ok(prog3, 9);
        TASK_CHECK(p != NULL);
        tglShaderRelease(p);
    }
    return 0;
}

static int verify_operands(void)
{
    TGLShaderInstr bad_dst[2];
    TGLShaderInstr bad_dst2[2];
    TGLShaderInstr bad_src[2];
    TGLShaderInstr bad_src2[2];
    TGLShaderInstr bad_late[4];
    TGLShaderInstr bad_op[2];
    TGLShaderInstr bad_imm[2];
    TGLShaderInstr bad_imm2[2];
    TGLShaderProgram *p;

    bad_dst[0] = iconst(2, 1, 1, 1, 1); /* r2 is read-only */
    bad_dst[1] = instr(OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(bad_dst, 2, TGL_SHADER_ERR_OPERAND, 0));

    bad_dst2[0] = iconst(16, 1, 1, 1, 1); /* out of range */
    bad_dst2[1] = instr(OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(bad_dst2, 2, TGL_SHADER_ERR_OPERAND, 0));

    bad_src[0] = instr(MOV, 3, 16, 0, 0);
    bad_src[1] = instr(OUTPUT, 0, 3, 0, 0);
    TASK_CHECK(create_fails(bad_src, 2, TGL_SHADER_ERR_OPERAND, 0));

    bad_src2[0] = instr(ADD, 3, 0, -1, 0);
    bad_src2[1] = instr(OUTPUT, 0, 3, 0, 0);
    TASK_CHECK(create_fails(bad_src2, 2, TGL_SHADER_ERR_OPERAND, 0));

    /* the first invalid instruction is reported, not a later one */
    bad_late[0] = iconst(3, 1, 1, 1, 1);
    bad_late[1] = instr(MOV, 4, 3, 0, 0);
    bad_late[2] = instr(ADD, 5, 4, 99, 0);
    bad_late[3] = instr(OUTPUT, 0, 5, 0, 0);
    TASK_CHECK(create_fails(bad_late, 4, TGL_SHADER_ERR_OPERAND, 2));

    bad_op[0] = instr(999, 3, 0, 0, 0);
    bad_op[1] = instr(OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(bad_op, 2, TGL_SHADER_ERR_OPCODE, 0));

    bad_imm[0] = iconst(3, 1, 1, 1, 1);
    bad_imm[0].imm[2] = (GLfloat) NAN;
    bad_imm[1] = instr(OUTPUT, 0, 3, 0, 0);
    TASK_CHECK(create_fails(bad_imm, 2, TGL_SHADER_ERR_IMMEDIATE, 0));

    bad_imm2[0] = iconst(3, 1, 1, 1, 1);
    bad_imm2[0].imm[0] = (GLfloat) INFINITY;
    bad_imm2[1] = instr(OUTPUT, 0, 3, 0, 0);
    TASK_CHECK(create_fails(bad_imm2, 2, TGL_SHADER_ERR_IMMEDIATE, 0));

    /* unused operands are not validated */
    {
        TGLShaderInstr ok2[2];
        ok2[0] = iconst(3, 1, 1, 1, 1);
        ok2[0].a = 99; /* unused by CONST */
        ok2[1] = instr(OUTPUT, 0, 3, 0, 0);
        p = create_ok(ok2, 2);
        TASK_CHECK(p != NULL);
        tglShaderRelease(p);
    }
    {
        TGLShaderInstr ok3[3];
        ok3[0] = instr(MOV, 3, 0, 0, 0);
        ok3[0].imm[0] = (GLfloat) NAN; /* MOV has no immediate */
        ok3[0].count = 999;            /* and no trip count */
        ok3[1] = instr(OUTPUT, 0, 3, 0, 0);
        p = create_ok(ok3, 2);
        TASK_CHECK(p != NULL);
        tglShaderRelease(p);
    }
    {
        TGLShaderInstr ok4[4];
        ok4[0] = irepeat(2);
        ok4[0].dst = 99; /* REPEAT only uses count */
        ok4[0].a = 99;
        ok4[1] = iconst(3, 1, 1, 1, 1);
        ok4[2] = instr(ENDREPEAT, 0, 0, 0, 0);
        ok4[3] = instr(OUTPUT, 0, 3, 0, 0);
        p = create_ok(ok4, 4);
        TASK_CHECK(p != NULL);
        tglShaderRelease(p);
    }
    return 0;
}

static int verify_repeat_count(void)
{
    TGLShaderInstr z[4];
    TGLShaderInstr big[4];
    TGLShaderInstr one[4];
    TGLShaderInstr max[4];
    TGLShaderProgram *p;

    z[0] = irepeat(0);
    z[1] = iconst(3, 1, 1, 1, 1);
    z[2] = instr(ENDREPEAT, 0, 0, 0, 0);
    z[3] = instr(OUTPUT, 0, 3, 0, 0);
    TASK_CHECK(create_fails(z, 4, TGL_SHADER_ERR_REPEAT_COUNT, 0));

    big[0] = irepeat(17);
    big[1] = iconst(3, 1, 1, 1, 1);
    big[2] = instr(ENDREPEAT, 0, 0, 0, 0);
    big[3] = instr(OUTPUT, 0, 3, 0, 0);
    TASK_CHECK(create_fails(big, 4, TGL_SHADER_ERR_REPEAT_COUNT, 0));

    one[0] = irepeat(1);
    one[1] = iconst(3, 1, 1, 1, 1);
    one[2] = instr(ENDREPEAT, 0, 0, 0, 0);
    one[3] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(one, 4);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);

    max[0] = irepeat(16);
    max[1] = iconst(3, 1, 1, 1, 1);
    max[2] = instr(ENDREPEAT, 0, 0, 0, 0);
    max[3] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(max, 4);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);
    return 0;
}

static int verify_structure(void)
{
    TGLShaderInstr code[20];
    TGLShaderProgram *p;
    GLint i;

    code[0] = instr(ELSE, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 1, TGL_SHADER_ERR_STRUCTURE, 0));
    code[0] = instr(ENDIF, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 1, TGL_SHADER_ERR_STRUCTURE, 0));
    code[0] = instr(ENDREPEAT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 1, TGL_SHADER_ERR_STRUCTURE, 0));

    /* break/continue with no enclosing REPEAT */
    code[0] = instr(BREAK_IF, 0, 0, 0, 0);
    code[1] = instr(OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 2, TGL_SHADER_ERR_STRUCTURE, 0));
    code[0] = instr(CONTINUE_IF, 0, 0, 0, 0);
    code[1] = instr(OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 2, TGL_SHADER_ERR_STRUCTURE, 0));

    /* mismatched closers */
    code[0] = irepeat(2);
    code[1] = instr(ENDIF, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 2, TGL_SHADER_ERR_STRUCTURE, 1));
    code[0] = instr(IF, 0, 0, 0, 0);
    code[1] = instr(ENDREPEAT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 2, TGL_SHADER_ERR_STRUCTURE, 1));
    code[0] = irepeat(2);
    code[1] = instr(ELSE, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 2, TGL_SHADER_ERR_STRUCTURE, 1));

    /* double ELSE */
    code[0] = instr(IF, 0, 0, 0, 0);
    code[1] = instr(ELSE, 0, 0, 0, 0);
    code[2] = instr(ELSE, 0, 0, 0, 0);
    code[3] = instr(ENDIF, 0, 0, 0, 0);
    code[4] = instr(OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 5, TGL_SHADER_ERR_STRUCTURE, 2));

    /* unclosed blocks report the opening instruction (outermost) */
    code[0] = instr(IF, 0, 0, 0, 0);
    code[1] = iconst(3, 1, 1, 1, 1);
    code[2] = iconst(4, 2, 2, 2, 2);
    TASK_CHECK(create_fails(code, 3, TGL_SHADER_ERR_UNCLOSED, 0));

    code[0] = instr(IF, 0, 0, 0, 0);
    code[1] = irepeat(2);
    code[2] = iconst(3, 1, 1, 1, 1);
    TASK_CHECK(create_fails(code, 3, TGL_SHADER_ERR_UNCLOSED, 0));

    /* nesting deeper than 8: the 9th IF fails */
    for (i = 0; i < 9; i++)
        code[i] = instr(IF, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 9, TGL_SHADER_ERR_STRUCTURE, 8));

    /* exactly 8 deep is fine */
    for (i = 0; i < 8; i++)
        code[i] = instr(IF, 0, 0, 0, 0);
    for (i = 0; i < 8; i++)
        code[8 + i] = instr(ENDIF, 0, 0, 0, 0);
    code[16] = instr(OUTPUT, 0, 0, 0, 0);
    p = create_ok(code, 17);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);
    return 0;
}

static int verify_output_rule(void)
{
    TGLShaderInstr code[3];
    TGLShaderProgram *p;

    /* OUTPUT must be the final instruction */
    code[0] = instr(OUTPUT, 0, 0, 0, 0);
    code[1] = iconst(3, 1, 1, 1, 1);
    TASK_CHECK(create_fails(code, 2, TGL_SHADER_ERR_OUTPUT, 0));

    /* missing OUTPUT reports the final instruction index */
    code[0] = iconst(3, 1, 1, 1, 1);
    code[1] = instr(MOV, 4, 3, 0, 0);
    TASK_CHECK(create_fails(code, 2, TGL_SHADER_ERR_OUTPUT, 1));

    /* OUTPUT inside a block */
    code[0] = instr(IF, 0, 0, 0, 0);
    code[1] = instr(OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 2, TGL_SHADER_ERR_OUTPUT, 1));

    /* two OUTPUTs: the first is not final */
    code[0] = instr(OUTPUT, 0, 0, 0, 0);
    code[1] = instr(OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 2, TGL_SHADER_ERR_OUTPUT, 0));

    /* single OUTPUT program is valid */
    code[0] = instr(OUTPUT, 0, 0, 0, 0);
    p = create_ok(code, 1);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);
    return 0;
}

static int verify_budget(void)
{
    TGLShaderInstr code[260];
    TGLShaderProgram *p;
    GLint i, n;

    /* 16*16*15 = 3840 counted, plus OUTPUT: well under budget */
    n = 0;
    code[n++] = irepeat(16);
    code[n++] = irepeat(16);
    for (i = 0; i < 15; i++)
        code[n++] = iconst(3, 1, 1, 1, 1);
    code[n++] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[n++] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[n++] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, (GLuint) n);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);

    /* exactly 4096 expanded instructions is allowed */
    n = 0;
    code[n++] = irepeat(16);
    code[n++] = irepeat(16);
    for (i = 0; i < 15; i++)
        code[n++] = iconst(3, 1, 1, 1, 1);
    code[n++] = instr(ENDREPEAT, 0, 0, 0, 0);
    for (i = 0; i < 15; i++)
        code[n++] = iconst(3, 1, 1, 1, 1);
    code[n++] = instr(ENDREPEAT, 0, 0, 0, 0);
    for (i = 0; i < 15; i++)
        code[n++] = iconst(3, 1, 1, 1, 1);
    code[n++] = instr(OUTPUT, 0, 3, 0, 0);
    /* expansion: 16*(16*15 + 15) + 15 + 1(OUTPUT) = 4096 */
    p = create_ok(code, (GLuint) n);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);

    /* one more top-level instruction exceeds the budget with no loop
     * responsible: the final instruction index is reported */
    n = 0;
    code[n++] = irepeat(16);
    code[n++] = irepeat(16);
    for (i = 0; i < 15; i++)
        code[n++] = iconst(3, 1, 1, 1, 1);
    code[n++] = instr(ENDREPEAT, 0, 0, 0, 0);
    for (i = 0; i < 15; i++)
        code[n++] = iconst(3, 1, 1, 1, 1);
    code[n++] = instr(ENDREPEAT, 0, 0, 0, 0);
    for (i = 0; i < 16; i++)
        code[n++] = iconst(3, 1, 1, 1, 1);
    code[n++] = instr(OUTPUT, 0, 3, 0, 0);
    TASK_CHECK(create_fails(code, (GLuint) n, TGL_SHADER_ERR_BUDGET, n - 1));

    /* 16*16*17 = 4352: the inner REPEAT first makes it overflow */
    n = 0;
    code[n++] = irepeat(16);
    code[n++] = irepeat(16);
    for (i = 0; i < 17; i++)
        code[n++] = iconst(3, 1, 1, 1, 1);
    code[n++] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[n++] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[n++] = instr(OUTPUT, 0, 3, 0, 0);
    TASK_CHECK(create_fails(code, (GLuint) n, TGL_SHADER_ERR_BUDGET, 1));

    /* overflow at an instruction sitting directly in the outer body:
     * the outer REPEAT is reported */
    n = 0;
    code[n++] = irepeat(16); /* 0 */
    code[n++] = irepeat(2);  /* 1 */
    for (i = 0; i < 16; i++)
        code[n++] = iconst(3, 1, 1, 1, 1);
    code[n++] = instr(ENDREPEAT, 0, 0, 0, 0);
    for (i = 0; i < 225; i++)
        code[n++] = iconst(3, 1, 1, 1, 1);
    code[n++] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[n++] = instr(OUTPUT, 0, 3, 0, 0);
    /* expansion: 16*(2*16 + 225) + 1 = 4113 > 4096, exceeded in outer body */
    TASK_CHECK(create_fails(code, (GLuint) n, TGL_SHADER_ERR_BUDGET, 0));
    return 0;
}

static int verify_derivative(void)
{
    TGLShaderInstr code[5];
    TGLShaderProgram *p;

    code[0] = instr(IF, 0, 0, 0, 0);
    code[1] = instr(DDX, 3, 0, 0, 0);
    code[2] = instr(ENDIF, 0, 0, 0, 0);
    code[3] = instr(OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 4, TGL_SHADER_ERR_DERIVATIVE, 1));

    code[0] = irepeat(2);
    code[1] = instr(DDY, 3, 0, 0, 0);
    code[2] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[3] = instr(OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(create_fails(code, 4, TGL_SHADER_ERR_DERIVATIVE, 1));

    /* top-level derivatives are legal */
    code[0] = instr(DDX, 3, 0, 0, 0);
    code[1] = instr(DDY, 4, 0, 0, 0);
    code[2] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 3);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);
    return 0;
}

static int verify_loop_first_iteration(void)
{
    /* a definition later in the loop body does not reach a read in the
     * first iteration: the loop header state is the loop-entry state */
    TGLShaderInstr code[5];
    TGLShaderProgram *p;

    code[0] = irepeat(3);
    code[1] = instr(MOV, 4, 3, 0, 0); /* r3 not defined yet */
    code[2] = iconst(3, 1, 1, 1, 1);
    code[3] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[4] = instr(OUTPUT, 0, 4, 0, 0);
    TASK_CHECK(create_fails(code, 5, TGL_SHADER_ERR_UNDEFINED, 1));

    /* the same shape with the read guarded by the loop-exit is fine */
    code[0] = irepeat(3);
    code[1] = iconst(3, 1, 1, 1, 1);
    code[2] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[3] = instr(MOV, 4, 3, 0, 0);
    code[4] = instr(OUTPUT, 0, 4, 0, 0);
    p = create_ok(code, 5);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);
    return 0;
}

static int verify_create_keeps_binding(void)
{
    TGLShaderInstr good[2];
    TGLShaderInstr bad[2];
    TGLShaderProgram *p;
    ZBuffer *zb;

    good[0] = iconst(3, 1, 0, 0, 1);
    good[1] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(good, 2);
    TASK_CHECK(p != NULL);

    bad[0] = instr(MOV, 4, 9, 0, 0); /* reads undefined r9 */
    bad[1] = instr(OUTPUT, 0, 4, 0, 0);

    zb = ZB_open(8, 8, ZB_MODE_RGBA, NULL);
    TASK_CHECK(zb != NULL);
    glInit(zb);
    tglShaderBind(p);
    /* a failed creation must not change the current binding */
    TASK_CHECK(tglShaderCreate(bad, 2, NULL) == NULL);
    TASK_CHECK(tglShaderGetBound() == p);
    tglShaderBind(NULL);
    tglShaderRelease(p);
    glClose();
    ZB_close(zb);
    return 0;
}

/* ================================================================== */
/* control group: independent quad execution semantics                 */
/* ================================================================== */

static void zero_inputs(GLfloat in[4][TGL_SHADER_INPUT_REGS][4])
{
    memset(in, 0, 4 * TGL_SHADER_INPUT_REGS * 4 * sizeof(GLfloat));
}

static int control_nested_branch(void)
{
    TGLShaderInstr code[12];
    TGLShaderProgram *p;
    GLfloat in[4][TGL_SHADER_INPUT_REGS][4];
    GLfloat out[4][4];
    GLuint mask = 0;
    static const GLfloat want[4][4] = {
        {11, 12, 13, 14}, /* r0=1, r1=1: (1,2,3,4) + 10 */
        {10, 20, 30, 40}, /* r0=1, r1=0: (1,2,3,4) * 10 */
        {7, 7, 7, 7},     /* r0=0: else branch */
        {7, 7, 7, 7},
    };
    GLint lane, k;

    code[0] = iconst(3, 1, 2, 3, 4);
    code[1] = instr(IF, 0, 0, 0, 0);
    code[2] = iconst(4, 10, 10, 10, 10);
    code[3] = instr(IF, 0, 1, 0, 0);
    code[4] = instr(ADD, 3, 3, 4, 0);
    code[5] = instr(ELSE, 0, 0, 0, 0);
    code[6] = instr(MUL, 3, 3, 4, 0);
    code[7] = instr(ENDIF, 0, 0, 0, 0);
    code[8] = instr(ELSE, 0, 0, 0, 0);
    code[9] = iconst(3, 7, 7, 7, 7);
    code[10] = instr(ENDIF, 0, 0, 0, 0);
    code[11] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 12);
    TASK_CHECK(p != NULL);

    zero_inputs(in);
    in[0][0][0] = 1;
    in[0][1][0] = 1;
    in[1][0][0] = 1;
    in[1][1][0] = 0;
    in[2][0][0] = 0;
    in[3][0][0] = 0;
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, NULL) ==
               TGL_SHADER_EXEC_OK);
    TASK_CHECK(mask == 0xF);
    for (lane = 0; lane < 4; lane++)
        for (k = 0; k < 4; k++)
            TASK_CHECK(out[lane][k] == want[lane][k]);
    tglShaderRelease(p);
    return 0;
}

static int control_loop_exit(void)
{
    /* each lane breaks out of the loop at its own iteration */
    TGLShaderInstr code[8];
    TGLShaderProgram *p;
    GLfloat in[4][TGL_SHADER_INPUT_REGS][4];
    GLfloat out[4][4];
    GLuint mask = 0;
    static const GLfloat target[4] = {0.5f, 2.5f, 3.5f, 5.5f};
    static const GLfloat want[4] = {1, 3, 4, 5};
    GLint lane, k;

    code[0] = iconst(3, 0, 0, 0, 0);
    code[1] = iconst(4, 1, 1, 1, 1);
    code[2] = irepeat(5);
    code[3] = instr(ADD, 3, 3, 4, 0);  /* r3 += 1 */
    code[4] = instr(LT, 5, 0, 3, 0);   /* r5 = r0 < r3 */
    code[5] = instr(BREAK_IF, 0, 5, 0, 0);
    code[6] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[7] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 8);
    TASK_CHECK(p != NULL);

    zero_inputs(in);
    for (lane = 0; lane < 4; lane++)
        in[lane][0][0] = target[lane];
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, NULL) ==
               TGL_SHADER_EXEC_OK);
    TASK_CHECK(mask == 0xF);
    for (lane = 0; lane < 4; lane++)
        for (k = 0; k < 4; k++)
            TASK_CHECK(out[lane][k] == want[lane]);
    tglShaderRelease(p);
    return 0;
}

static int control_inner_continue_break(void)
{
    /* nested loops; the inner loop uses both CONTINUE_IF and BREAK_IF */
    TGLShaderInstr code[14];
    TGLShaderProgram *p;
    GLfloat in[4][TGL_SHADER_INPUT_REGS][4];
    GLfloat out[4][4];
    GLuint mask = 0;
    static const GLfloat T[4] = {2.5f, 0.5f, 5.5f, 100.5f};
    static const GLfloat B[4] = {50.5f, 100.5f, 20.5f, 0.5f};
    static const GLfloat want[4] = {57, 110, 27, 12};
    GLint lane, k;

    code[0] = iconst(3, 0, 0, 0, 0);   /* acc */
    code[1] = iconst(4, 1, 1, 1, 1);   /* one */
    code[2] = iconst(6, 10, 10, 10, 10); /* ten */
    code[3] = irepeat(2);              /* outer */
    code[4] = irepeat(6);              /* inner */
    code[5] = instr(ADD, 3, 3, 4, 0);  /* acc += 1 */
    code[6] = instr(LT, 5, 3, 0, 0);   /* acc < T? */
    code[7] = instr(CONTINUE_IF, 0, 5, 0, 0);
    code[8] = instr(ADD, 3, 3, 6, 0);  /* acc += 10 */
    code[9] = instr(LT, 5, 1, 3, 0);   /* B < acc? */
    code[10] = instr(BREAK_IF, 0, 5, 0, 0);
    code[11] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[12] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[13] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 14);
    TASK_CHECK(p != NULL);

    zero_inputs(in);
    for (lane = 0; lane < 4; lane++) {
        in[lane][0][0] = T[lane];
        in[lane][1][0] = B[lane];
    }
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, NULL) ==
               TGL_SHADER_EXEC_OK);
    for (lane = 0; lane < 4; lane++)
        for (k = 0; k < 4; k++)
            TASK_CHECK(out[lane][k] == want[lane]);
    tglShaderRelease(p);
    return 0;
}

static int control_discard_derivative(void)
{
    /* a discarded lane keeps computing registers and feeds DDX/DDY */
    TGLShaderInstr code[8];
    TGLShaderProgram *p;
    GLfloat in[4][TGL_SHADER_INPUT_REGS][4];
    GLfloat out[4][4];
    GLuint mask = 0;
    static const GLfloat r0x[4] = {1, 6, 10, 20};
    static const GLfloat want_x[4] = {10, 10, 20, 20}; /* DDX of 2*r0.x */
    GLint lane;

    code[0] = iconst(3, 5, 5, 5, 5);
    code[1] = instr(LT, 4, 0, 3, 0); /* r4 = r0 < 5 */
    code[2] = instr(DISCARD_IF, 0, 4, 0, 0);
    code[3] = iconst(5, 2, 2, 2, 2);
    code[4] = instr(MUL, 6, 0, 5, 0); /* r6 = 2 * r0 */
    code[5] = instr(DDX, 7, 6, 0, 0);
    code[6] = instr(DDY, 8, 6, 0, 0);
    code[7] = instr(OUTPUT, 0, 7, 0, 0);
    p = create_ok(code, 8);
    TASK_CHECK(p != NULL);

    zero_inputs(in);
    for (lane = 0; lane < 4; lane++)
        in[lane][0][0] = r0x[lane];
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, NULL) ==
               TGL_SHADER_EXEC_OK);
    /* lane 0 discarded itself but still produced r6 for lane 1's DDX */
    TASK_CHECK(mask == 0xE);
    for (lane = 0; lane < 4; lane++)
        TASK_CHECK(out[lane][0] == want_x[lane]);

    /* same program outputting DDY instead */
    code[7] = instr(OUTPUT, 0, 8, 0, 0);
    {
        TGLShaderProgram *p2 = create_ok(code, 8);
        static const GLfloat want_y[4] = {18, 28, 18, 28};
        TASK_CHECK(p2 != NULL);
        mask = 0;
        TASK_CHECK(tglShaderExecuteQuad(p2, in, out, 0xF, &mask, NULL) ==
                   TGL_SHADER_EXEC_OK);
        TASK_CHECK(mask == 0xE);
        for (lane = 0; lane < 4; lane++)
            TASK_CHECK(out[lane][0] == want_y[lane]);
        tglShaderRelease(p2);
    }
    tglShaderRelease(p);
    return 0;
}

static int control_tex(void)
{
    static GLuint tex[TGL_SHADER_TEXTURE_DIM * TGL_SHADER_TEXTURE_DIM];
    TGLShaderInstr code[2];
    TGLShaderProgram *p;
    GLfloat in[4][TGL_SHADER_INPUT_REGS][4];
    GLfloat out[4][4];
    GLuint mask = 0;

    memset(tex, 0, sizeof tex);
    tex[64 * TGL_SHADER_TEXTURE_DIM + 32] = 0x0000FF00u; /* green */
    tex[128 * TGL_SHADER_TEXTURE_DIM + 128] = 0x00FF8080u;

    code[0] = instr(TEX, 3, 0, 0, 0);
    code[1] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 2);
    TASK_CHECK(p != NULL);

    zero_inputs(in);
    in[0][0][0] = 32.0f / 256.0f; /* u -> texel 32 */
    in[0][0][1] = 64.0f / 256.0f; /* v -> texel 64 */
    in[1][0][0] = 1.0f + 32.0f / 256.0f;  /* repeats */
    in[1][0][1] = 64.0f / 256.0f;
    in[2][0][0] = -1.0f + 32.0f / 256.0f; /* negative wraps */
    in[2][0][1] = 64.0f / 256.0f;
    in[3][0][0] = 128.0f / 256.0f;
    in[3][0][1] = 128.0f / 256.0f;
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, tex) ==
               TGL_SHADER_EXEC_OK);
    TASK_CHECK(mask == 0xF);
    /* lanes 0..2 sample the green texel through the repeat wrap */
    TASK_CHECK(out[0][0] == 0.0f && out[0][1] == 1.0f && out[0][2] == 0.0f &&
               out[0][3] == 1.0f);
    TASK_CHECK(out[1][1] == 1.0f && out[1][0] == 0.0f);
    TASK_CHECK(out[2][1] == 1.0f && out[2][0] == 0.0f);
    /* lane 3: RGB normalized to [0,1], alpha forced to 1 */
    TASK_CHECK(out[3][0] == 1.0f);
    TASK_CHECK(out[3][1] == 128.0f / 255.0f && out[3][2] == 128.0f / 255.0f);
    TASK_CHECK(out[3][3] == 1.0f);

    /* a TEX program requires texels */
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, NULL) ==
               TGL_SHADER_EXEC_NO_TEXTURE);
    tglShaderRelease(p);

    /* a program without TEX works with no texture */
    code[0] = instr(MOV, 3, 0, 0, 0);
    code[1] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 2);
    TASK_CHECK(p != NULL);
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, NULL) ==
               TGL_SHADER_EXEC_OK);
    tglShaderRelease(p);
    return 0;
}

static int control_nonfinite(void)
{
    TGLShaderInstr code[3];
    TGLShaderProgram *p;
    GLfloat in[4][TGL_SHADER_INPUT_REGS][4];
    GLfloat out[4][4];
    GLuint mask;
    GLint lane, k;

    /* 1e30 * 1e30 overflows to inf */
    code[0] = iconst(3, 1e30f, 1e30f, 1e30f, 1e30f);
    code[1] = instr(MUL, 3, 3, 3, 0);
    code[2] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 3);
    TASK_CHECK(p != NULL);
    zero_inputs(in);
    for (lane = 0; lane < 4; lane++)
        for (k = 0; k < 4; k++)
            out[lane][k] = 99.0f;
    mask = 0xAA;
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, NULL) ==
               TGL_SHADER_EXEC_NONFINITE);
    /* caller outputs and mask are unchanged on failure */
    TASK_CHECK(mask == 0xAA);
    for (lane = 0; lane < 4; lane++)
        for (k = 0; k < 4; k++)
            TASK_CHECK(out[lane][k] == 99.0f);
    tglShaderRelease(p);

    /* non-finite input register */
    code[0] = instr(MOV, 3, 0, 0, 0);
    code[1] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 2);
    TASK_CHECK(p != NULL);
    zero_inputs(in);
    in[2][0][0] = (GLfloat) INFINITY;
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, NULL) ==
               TGL_SHADER_EXEC_NONFINITE);
    in[2][0][0] = (GLfloat) NAN;
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, NULL) ==
               TGL_SHADER_EXEC_NONFINITE);
    tglShaderRelease(p);
    return 0;
}

static int control_zero_coverage(void)
{
    /* a zero coverage mask is valid: the program still executes */
    TGLShaderInstr code[2];
    TGLShaderProgram *p;
    GLfloat in[4][TGL_SHADER_INPUT_REGS][4];
    GLfloat out[4][4];
    GLuint mask = 0xF;

    code[0] = iconst(3, 0.5f, 0.25f, 0.75f, 1.0f);
    code[1] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 2);
    TASK_CHECK(p != NULL);
    zero_inputs(in);
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0, &mask, NULL) ==
               TGL_SHADER_EXEC_OK);
    TASK_CHECK(mask == 0);
    TASK_CHECK(out[0][0] == 0.5f && out[3][2] == 0.75f);
    tglShaderRelease(p);
    return 0;
}

static int control_alias(void)
{
    TGLShaderInstr code[3];
    TGLShaderProgram *p;
    GLfloat in[4][TGL_SHADER_INPUT_REGS][4];
    GLfloat out[4][4];
    GLuint mask = 0;
    static const GLfloat r0x[4] = {1, 2, 4, 8};

    /* dst/source aliasing within one instruction */
    code[0] = iconst(3, 1, 2, 3, 4);
    code[1] = instr(ADD, 3, 3, 3, 0);
    code[2] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 3);
    TASK_CHECK(p != NULL);
    zero_inputs(in);
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, NULL) ==
               TGL_SHADER_EXEC_OK);
    TASK_CHECK(out[0][0] == 2 && out[0][1] == 4 && out[0][2] == 6 &&
               out[0][3] == 8);
    tglShaderRelease(p);

    /* DDX with dst == a reads the old values of every lane first */
    code[0] = instr(MOV, 3, 0, 0, 0);
    code[1] = instr(DDX, 3, 3, 0, 0);
    code[2] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 3);
    TASK_CHECK(p != NULL);
    zero_inputs(in);
    in[0][0][0] = r0x[0];
    in[1][0][0] = r0x[1];
    in[2][0][0] = r0x[2];
    in[3][0][0] = r0x[3];
    TASK_CHECK(tglShaderExecuteQuad(p, in, out, 0xF, &mask, NULL) ==
               TGL_SHADER_EXEC_OK);
    TASK_CHECK(out[0][0] == 1 && out[1][0] == 1); /* 2-1 for both */
    TASK_CHECK(out[2][0] == 4 && out[3][0] == 4); /* 8-4 for both */
    tglShaderRelease(p);
    return 0;
}

/* ================================================================== */
/* render group: reference rasterizer (test oracle)                    */
/* ================================================================== */

typedef struct {
    GLfloat x, y;   /* screen coords (from the truncated zp values) */
    GLfloat zfix;   /* zp.z as float */
    GLfloat winv;   /* 1/clip.w */
    GLfloat ndcz;   /* clip.z / clip.w */
    GLfloat uov;    /* tex.u * winv */
    GLfloat vov;    /* tex.v * winv */
    GLfloat cov[4]; /* color * winv */
} RefVert;

static GLint ref_clamp(GLfloat r)
{
    if (!isfinite(r))
        return 0;
    if (r >= (GLfloat) INT_MAX)
        return INT_MAX;
    if (r <= (GLfloat) INT_MIN)
        return INT_MIN;
    return (GLint) r;
}

static GLfloat ref_sx, ref_sy, ref_sz, ref_tx, ref_ty, ref_tz;

/* mirrors gl_eval_viewport */
static void ref_viewport(GLint w, GLint h)
{
    GLfloat zsize = (1 << (ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS));
    ref_tx = (GLfloat) (((w - 0.5) / 2.0) + 0);
    ref_ty = (GLfloat) (((h - 0.5) / 2.0) + 0);
    ref_tz = (GLfloat) (((zsize - 0.5) / 2.0) + ((1 << ZB_POINT_Z_FRAC_BITS)) / 2);
    ref_sx = (GLfloat) ((w - 0.5) / 2.0);
    ref_sy = (GLfloat) (-(h - 0.5) / 2.0);
    ref_sz = (GLfloat) (-((zsize - 0.5) / 2.0));
}

/* mirrors gl_transform_to_viewport_vertex_c */
static RefVert ref_vertex(GLfloat cx, GLfloat cy, GLfloat cz, GLfloat cw,
                          GLfloat u, GLfloat v, const GLfloat col[4])
{
    RefVert r;
    GLfloat winv = 1.0f / cw;
    GLint k;
    r.winv = winv;
    r.x = (GLfloat) ref_clamp(cx * winv * ref_sx + ref_tx);
    r.y = (GLfloat) ref_clamp(cy * winv * ref_sy + ref_ty);
    r.zfix = (GLfloat) ref_clamp(cz * winv * ref_sz + ref_tz);
    r.ndcz = cz * winv;
    r.uov = u * winv;
    r.vov = v * winv;
    for (k = 0; k < 4; k++)
        r.cov[k] = col[k] * winv;
    return r;
}

static GLuint ref_to_byte(GLfloat v)
{
    if (v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 255;
    return (GLuint) (v * 255.0f + 0.5f);
}

/* mirrors tgl_shader_fill_triangle (src/shader.c) */
static void ref_triangle(const TGLShaderProgram *prog, const GLuint *tex,
                         const RefVert *vin, PIXEL *fb, GLushort *zb,
                         GLint W, GLint H, GLint dtest, GLint dwrite)
{
    GLfloat x[3], y[3], zf[3], wi[3], nz[3], uo[3], vo[3], co[3][4];
    GLfloat fdx1, fdy1, fdx2, fdy2, area, fz, t;
    GLfloat d_wi_x, d_wi_y, d_nz_x, d_nz_y, d_zf_x, d_zf_y;
    GLfloat d_uo_x, d_uo_y, d_vo_x, d_vo_y;
    GLfloat d_co_x[4], d_co_y[4];
    GLfloat ex[3], ey[3], minx, maxx, miny, maxy;
    GLint topleft[3];
    GLint i, j, k, px0, px1, py0, py1, qx, qy;

    for (i = 0; i < 3; i++) {
        x[i] = vin[i].x;
        y[i] = vin[i].y;
        zf[i] = vin[i].zfix;
        wi[i] = vin[i].winv;
        nz[i] = vin[i].ndcz;
        uo[i] = vin[i].uov;
        vo[i] = vin[i].vov;
        for (k = 0; k < 4; k++)
            co[i][k] = vin[i].cov[k];
    }

    fdx1 = x[1] - x[0];
    fdy1 = y[1] - y[0];
    fdx2 = x[2] - x[0];
    fdy2 = y[2] - y[0];
    area = fdx1 * fdy2 - fdx2 * fdy1;
    if (area == 0.0f)
        return;
    if (area < 0.0f) {
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

#define REF_GRADIENT(Q, gx, gy)                        \
    do {                                               \
        GLfloat d1_ = Q[1] - Q[0];                     \
        GLfloat d2_ = Q[2] - Q[0];                     \
        gx = (fdy2 * d1_ - fdy1 * d2_) * fz;           \
        gy = (fdx1 * d2_ - fdx2 * d1_) * fz;           \
    } while (0)

    REF_GRADIENT(wi, d_wi_x, d_wi_y);
    REF_GRADIENT(nz, d_nz_x, d_nz_y);
    REF_GRADIENT(zf, d_zf_x, d_zf_y);
    REF_GRADIENT(uo, d_uo_x, d_uo_y);
    REF_GRADIENT(vo, d_vo_x, d_vo_y);
    for (k = 0; k < 4; k++) {
        GLfloat d1_ = co[1][k] - co[0][k];
        GLfloat d2_ = co[2][k] - co[0][k];
        d_co_x[k] = (fdy2 * d1_ - fdy1 * d2_) * fz;
        d_co_y[k] = (fdx1 * d2_ - fdx2 * d1_) * fz;
    }

    minx = maxx = x[0];
    miny = maxy = y[0];
    for (i = 1; i < 3; i++) {
        if (x[i] < minx) minx = x[i];
        if (x[i] > maxx) maxx = x[i];
        if (y[i] < miny) miny = y[i];
        if (y[i] > maxy) maxy = y[i];
    }
    px0 = (GLint) ceilf(minx - 0.5f);
    px1 = (GLint) floorf(maxx - 0.5f);
    py0 = (GLint) ceilf(miny - 0.5f);
    py1 = (GLint) floorf(maxy - 0.5f);
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > W - 1) px1 = W - 1;
    if (py1 > H - 1) py1 = H - 1;
    if (px0 > px1 || py0 > py1)
        return;

    for (i = 0; i < 3; i++) {
        j = (i + 1) % 3;
        ex[i] = x[j] - x[i];
        ey[i] = y[j] - y[i];
        topleft[i] = (ey[i] > 0.0f) || (ey[i] == 0.0f && ex[i] > 0.0f);
    }

    for (qy = py0 & ~1; qy <= py1; qy += 2) {
        for (qx = px0 & ~1; qx <= px1; qx += 2) {
            GLuint coverage = 0, wmask = 0;
            GLfloat cx[4], cy[4], zbufv[4];
            GLfloat in[4][TGL_SHADER_INPUT_REGS][4];
            GLfloat out[4][4];
            GLint lane, e;

            for (lane = 0; lane < 4; lane++) {
                GLint lx = qx + (lane & 1);
                GLint ly = qy + (lane >> 1);
                cx[lane] = (GLfloat) lx + 0.5f;
                cy[lane] = (GLfloat) ly + 0.5f;
                if (lx < 0 || lx >= W || ly < 0 || ly >= H)
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

            for (lane = 0; lane < 4; lane++) {
                GLfloat dx = cx[lane] - x[0];
                GLfloat dy = cy[lane] - y[0];
                GLfloat wq = wi[0] + d_wi_x * dx + d_wi_y * dy;
                GLfloat uq, vq, zq;
                if (wq == 0.0f || !isfinite(wq))
                    break;
                uq = (uo[0] + d_uo_x * dx + d_uo_y * dy) / wq;
                vq = (vo[0] + d_vo_x * dx + d_vo_y * dy) / wq;
                zq = nz[0] + d_nz_x * dx + d_nz_y * dy;
                zbufv[lane] = zf[0] + d_zf_x * dx + d_zf_y * dy;
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
            if (lane < 4)
                continue; /* input failure: quad commits nothing */

            if (tglShaderExecuteQuad(prog, in, out, coverage, &wmask, tex) !=
                TGL_SHADER_EXEC_OK)
                continue;

            for (lane = 0; lane < 4; lane++) {
                GLint lx, ly, zi;
                GLuint zz;
                GLushort *pz;
                if (!(wmask & (1u << lane)))
                    continue;
                lx = qx + (lane & 1);
                ly = qy + (lane >> 1);
                zi = (GLint) zbufv[lane];
                zz = ((GLuint) zi) >> ZB_POINT_Z_FRAC_BITS;
                pz = zb + ly * W + lx;
                if (dtest && !(zz >= (GLuint) *pz))
                    continue;
                fb[ly * W + lx] = (PIXEL) ((ref_to_byte(out[lane][0]) << 16) |
                                           (ref_to_byte(out[lane][1]) << 8) |
                                           ref_to_byte(out[lane][2]));
                if (dwrite)
                    *pz = (GLushort) zz;
            }
        }
    }
}

/* ================================================================== */
/* render group: fixtures and comparison                               */
/* ================================================================== */

#define RW 64
#define RH 64
#define COLOR_TOL 1
#define DEPTH_TOL 2

static void fx_begin(ZBuffer *zb)
{
    glInit(zb);
    glViewport(0, 0, RW, RH);
    /* texture first: a matrix op in PROJECTION/MODELVIEW mode must be the
     * last one so the combined matrix is recomputed at the next glBegin */
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ref_viewport(RW, RH);
}

static GLint fx_channel_diff(GLint a, GLint b)
{
    return a > b ? a - b : b - a;
}

/* count of pixels whose RGB channels differ by more than tol */
static GLint fx_fb_diff(const PIXEL *a, const PIXEL *b, GLint n, GLint tol)
{
    GLint i, bad = 0;
    for (i = 0; i < n; i++) {
        if (fx_channel_diff((GLint) GET_RED(a[i]), (GLint) GET_RED(b[i])) >
                tol ||
            fx_channel_diff((GLint) GET_GREEN(a[i]),
                            (GLint) GET_GREEN(b[i])) > tol ||
            fx_channel_diff((GLint) GET_BLUE(a[i]), (GLint) GET_BLUE(b[i])) >
                tol)
            bad++;
    }
    return bad;
}

static GLint fx_zb_diff(const GLushort *a, const GLushort *b, GLint n,
                        GLint tol)
{
    GLint i, bad = 0;
    for (i = 0; i < n; i++)
        if (fx_channel_diff((GLint) a[i], (GLint) b[i]) > tol)
            bad++;
    return bad;
}

static GLint fx_count_nonclear(const PIXEL *a, GLint n)
{
    GLint i, c = 0;
    for (i = 0; i < n; i++)
        if (a[i] & 0xFFFFFF)
            c++;
    return c;
}

static PIXEL *fx_exp_fb(void)
{
    PIXEL *p = (PIXEL *) calloc((size_t) RW * RH, sizeof(PIXEL));
    return p;
}

static GLushort *fx_exp_zb(void)
{
    return (GLushort *) calloc((size_t) RW * RH, sizeof(GLushort));
}

/* ================================================================== */
/* render group: cases                                                 */
/* ================================================================== */

static int render_gouraud(void)
{
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    PIXEL *efb = NULL;
    GLushort *ezb = NULL;
    TGLShaderInstr code[1];
    TGLShaderProgram *p = NULL;
    RefVert rv[3];
    static const GLfloat c0[4] = {1, 0, 0, 1};
    static const GLfloat c1[4] = {0, 1, 0, 1};
    static const GLfloat c2[4] = {0, 0, 1, 1};
    /* NDC coords chosen to land on zp (0,0), (60,0), (30,60) */
    static const GLfloat ax = -1.0f, ay = 1.0f;
    static const GLfloat bx = 28.25f / 31.75f, by = 1.0f;
    static const GLfloat cx = -1.75f / 31.75f, cy = -28.25f / 31.75f;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    code[0] = instr(OUTPUT, 0, 1, 0, 0); /* output the Gouraud color */
    p = create_ok(code, 1);
    TASK_CHECK(p != NULL);
    tglShaderBind(p);

    glBegin(GL_TRIANGLES);
    glColor4f(1, 0, 0, 1);
    glVertex3f(ax, ay, 0.2f);
    glColor4f(0, 1, 0, 1);
    glVertex3f(bx, by, 0.2f);
    glColor4f(0, 0, 1, 1);
    glVertex3f(cx, cy, 0.2f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);

    efb = fx_exp_fb();
    ezb = fx_exp_zb();
    TASK_CHECK(efb != NULL && ezb != NULL);
    rv[0] = ref_vertex(ax, ay, 0.2f, 1, 0, 0, c0);
    rv[1] = ref_vertex(bx, by, 0.2f, 1, 0, 0, c1);
    rv[2] = ref_vertex(cx, cy, 0.2f, 1, 0, 0, c2);
    ref_triangle(p, NULL, rv, efb, ezb, RW, RH, 1, 1);
    TASK_CHECK(fx_fb_diff(zb->pbuf, efb, RW * RH, COLOR_TOL) == 0);
    TASK_CHECK(fx_zb_diff(zb->zbuf, ezb, RW * RH, DEPTH_TOL) == 0);

    /* independent barycentric spot check at pixel (30,20) */
    {
        double px = 30.5, py = 20.5;
        double x0 = rv[0].x, y0 = rv[0].y;
        double x1 = rv[1].x, y1 = rv[1].y;
        double x2 = rv[2].x, y2 = rv[2].y;
        double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        double l1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / det;
        double l2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / det;
        double l0 = 1.0 - l1 - l2;
        GLint er = (GLint) (l0 * 255.0 + 0.5);
        GLint eg = (GLint) (l1 * 255.0 + 0.5);
        GLint eb = (GLint) (l2 * 255.0 + 0.5);
        PIXEL got = zb->pbuf[20 * RW + 30];
        TASK_CHECK(fx_channel_diff((GLint) GET_RED(got), er) <= 1);
        TASK_CHECK(fx_channel_diff((GLint) GET_GREEN(got), eg) <= 1);
        TASK_CHECK(fx_channel_diff((GLint) GET_BLUE(got), eb) <= 1);
    }

    tglShaderBind(NULL);
    tglShaderRelease(p);
    glClose();
    ZB_close(zb);
    free(efb);
    free(ezb);
    return 0;
}

static int render_r2_inputs(void)
{
    /* r2 = (pixel center x, pixel center y, [0,1] depth, 1/clip.w) */
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    TGLShaderInstr code[3];
    TGLShaderProgram *p = NULL;
    PIXEL got;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    code[0] = iconst(4, 1.0f / 64.0f, 1.0f / 64.0f, 1.0f, 1.0f);
    code[1] = instr(MUL, 3, 2, 4, 0);
    code[2] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 3);
    TASK_CHECK(p != NULL);
    tglShaderBind(p);

    glBegin(GL_TRIANGLES);
    glColor4f(1, 1, 1, 1);
    glVertex3f(-1, 1, 0.2f);
    glVertex3f(1, 1, 0.2f);
    glVertex3f(0, -1, 0.2f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);

    /* pixel (10,20): center (10.5,20.5), ndc z 0.2 -> depth 0.6 */
    got = zb->pbuf[20 * RW + 10];
    TASK_CHECK(fx_channel_diff((GLint) GET_RED(got),
                               (GLint) (10.5f / 64.0f * 255.0f + 0.5f)) <= 1);
    TASK_CHECK(fx_channel_diff((GLint) GET_GREEN(got),
                               (GLint) (20.5f / 64.0f * 255.0f + 0.5f)) <= 1);
    TASK_CHECK(fx_channel_diff((GLint) GET_BLUE(got),
                               (GLint) (0.6f * 255.0f + 0.5f)) <= 1);

    /* pixel (40,8): center (40.5,8.5) */
    got = zb->pbuf[8 * RW + 40];
    TASK_CHECK(fx_channel_diff((GLint) GET_RED(got),
                               (GLint) (40.5f / 64.0f * 255.0f + 0.5f)) <= 1);
    TASK_CHECK(fx_channel_diff((GLint) GET_GREEN(got),
                               (GLint) (8.5f / 64.0f * 255.0f + 0.5f)) <= 1);

    tglShaderBind(NULL);
    tglShaderRelease(p);
    glClose();
    ZB_close(zb);
    return 0;
}

static void build_ramp_texture(GLubyte *data)
{
    GLint iu, iv;
    for (iv = 0; iv < 256; iv++)
        for (iu = 0; iu < 256; iu++) {
            data[(iv * 256 + iu) * 3 + 0] = (GLubyte) iu;
            data[(iv * 256 + iu) * 3 + 1] = (GLubyte) iv;
            data[(iv * 256 + iu) * 3 + 2] = 64;
        }
}

static void build_ramp_texels(GLuint *tex)
{
    GLint iu, iv;
    for (iv = 0; iv < 256; iv++)
        for (iu = 0; iu < 256; iu++)
            tex[iv * 256 + iu] =
                ((GLuint) iu << 16) | ((GLuint) iv << 8) | 64u;
}

static GLubyte tex_data[256 * 256 * 3];
static GLuint ramp_texels[256 * 256];

static int render_tex256(void)
{
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    PIXEL *efb = NULL;
    GLushort *ezb = NULL;
    TGLShaderInstr code[2];
    TGLShaderProgram *p = NULL;
    GLuint tn = 0;
    RefVert rv[3];
    static const GLfloat white[4] = {1, 1, 1, 1};

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    build_ramp_texture(tex_data);
    build_ramp_texels(ramp_texels);
    glGenTextures(1, &tn);
    glBindTexture(GL_TEXTURE_2D, tn);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 tex_data);
    glEnable(GL_TEXTURE_2D);

    code[0] = instr(TEX, 3, 0, 0, 0);
    code[1] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 2);
    TASK_CHECK(p != NULL);
    tglShaderBind(p);

    /* a quad (two triangles) with UVs spanning 0..1 */
    glBegin(GL_QUADS);
    glColor4f(1, 1, 1, 1);
    glTexCoord2f(0, 0);
    glVertex3f(-0.9f, -0.9f, 0.2f);
    glTexCoord2f(1, 0);
    glVertex3f(0.9f, -0.9f, 0.2f);
    glTexCoord2f(1, 1);
    glVertex3f(0.9f, 0.9f, 0.2f);
    glTexCoord2f(0, 1);
    glVertex3f(-0.9f, 0.9f, 0.2f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);

    efb = fx_exp_fb();
    ezb = fx_exp_zb();
    TASK_CHECK(efb != NULL && ezb != NULL);
    rv[0] = ref_vertex(-0.9f, -0.9f, 0.2f, 1, 0, 0, white);
    rv[1] = ref_vertex(0.9f, -0.9f, 0.2f, 1, 1, 0, white);
    rv[2] = ref_vertex(0.9f, 0.9f, 0.2f, 1, 1, 1, white);
    ref_triangle(p, ramp_texels, rv, efb, ezb, RW, RH, 1, 1);
    rv[1] = rv[2];
    rv[2] = ref_vertex(-0.9f, 0.9f, 0.2f, 1, 0, 1, white);
    ref_triangle(p, ramp_texels, rv, efb, ezb, RW, RH, 1, 1);
    TASK_CHECK(fx_fb_diff(zb->pbuf, efb, RW * RH, COLOR_TOL) == 0);

    /* spot check: pixel (32,32) center lies in the quad's second triangle
     * (UVs interpolate per-triangle, not bilinearly): barycentric u,v of
     * (32.5,32.5) in zp space are ~(0.5176,0.4825) -> texel (132,123) */
    {
        PIXEL got = zb->pbuf[32 * RW + 32];
        TASK_CHECK(fx_channel_diff((GLint) GET_RED(got), 132) <= 1);
        TASK_CHECK(fx_channel_diff((GLint) GET_GREEN(got), 123) <= 1);
        TASK_CHECK(fx_channel_diff((GLint) GET_BLUE(got), 64) <= 1);
    }

    /* TEX program with texturing disabled: draw is rejected */
    glDisable(GL_TEXTURE_2D);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glTexCoord2f(0, 0);
    glVertex3f(-0.5f, -0.5f, 0.2f);
    glTexCoord2f(1, 0);
    glVertex3f(0.5f, -0.5f, 0.2f);
    glTexCoord2f(0, 1);
    glVertex3f(0, 0.5f, 0.2f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_NO_TEXTURE);
    TASK_CHECK(fx_count_nonclear(zb->pbuf, RW * RH) == 0);

    tglShaderBind(NULL);
    tglShaderRelease(p);
    glClose();
    ZB_close(zb);
    free(efb);
    free(ezb);
    return 0;
}

static int render_perspective(void)
{
    /* non-uniform clip.w: projection P maps eye (x,y,z) to
     * clip (x, y, -3z-4, -z), so w = -z in {1, 1.5, 2}.
     * (glLoadMatrixf takes the OpenGL column-major layout) */
    static const GLfloat P[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                  0, 0, -3, -1, 0, 0, -4, 0};
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    PIXEL *efb = NULL;
    GLushort *ezb = NULL;
    TGLShaderInstr code[2];
    TGLShaderProgram *p = NULL;
    GLuint tn = 0;
    RefVert rv[3];
    static const GLfloat white[4] = {1, 1, 1, 1};
    /* eye-space vertices; ndc.x = x/w with w = -z */
    static const GLfloat ex[3] = {-0.8f, 1.6f, 0.0f};
    static const GLfloat ey[3] = {-0.8f, -1.6f, 1.2f};
    static const GLfloat ez[3] = {-1.0f, -2.0f, -1.5f};
    static const GLfloat eu[3] = {0, 2, 0};
    static const GLfloat ev[3] = {0, 0, 2};
    GLint i;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    build_ramp_texture(tex_data);
    build_ramp_texels(ramp_texels);
    glGenTextures(1, &tn);
    glBindTexture(GL_TEXTURE_2D, tn);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 tex_data);
    glEnable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(P);

    code[0] = instr(TEX, 3, 0, 0, 0);
    code[1] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 2);
    TASK_CHECK(p != NULL);
    tglShaderBind(p);

    glBegin(GL_TRIANGLES);
    for (i = 0; i < 3; i++) {
        glColor4f(1, 1, 1, 1);
        glTexCoord2f(eu[i], ev[i]);
        glVertex3f(ex[i], ey[i], ez[i]);
    }
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);

    efb = fx_exp_fb();
    ezb = fx_exp_zb();
    TASK_CHECK(efb != NULL && ezb != NULL);
    for (i = 0; i < 3; i++) {
        GLfloat pcx = ex[i];
        GLfloat pcy = ey[i];
        GLfloat pcz = -3.0f * ez[i] - 4.0f;
        GLfloat pcw = -ez[i];
        rv[i] = ref_vertex(pcx, pcy, pcz, pcw, eu[i], ev[i], white);
    }
    ref_triangle(p, ramp_texels, rv, efb, ezb, RW, RH, 1, 1);
    TASK_CHECK(fx_fb_diff(zb->pbuf, efb, RW * RH, COLOR_TOL) == 0);
    TASK_CHECK(fx_zb_diff(zb->zbuf, ezb, RW * RH, DEPTH_TOL) == 0);

    /* the result must be genuinely perspective-correct: at pixel (32,32)
     * the perspective u differs clearly from the affine (screen-linear) u */
    {
        double x0 = rv[0].x, y0 = rv[0].y;
        double x1 = rv[1].x, y1 = rv[1].y;
        double x2 = rv[2].x, y2 = rv[2].y;
        double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        double l1 = ((32.5 - x0) * (y2 - y0) - (x2 - x0) * (32.5 - y0)) / det;
        double l2 = ((x1 - x0) * (32.5 - y0) - (32.5 - x0) * (y1 - y0)) / det;
        double l0 = 1.0 - l1 - l2;
        double u_aff = l0 * eu[0] + l1 * eu[1] + l2 * eu[2];
        double w0 = 1.0 / -ez[0], w1 = 1.0 / -ez[1], w2 = 1.0 / -ez[2];
        double wq = l0 * w0 + l1 * w1 + l2 * w2;
        double u_persp =
            (l0 * eu[0] * w0 + l1 * eu[1] * w1 + l2 * eu[2] * w2) / wq;
        /* the u=2 vertex has w=2, so the perspective u is pulled well
         * below the affine u: the test is sensitive to the difference */
        TASK_CHECK(fabs(u_persp - u_aff) > 0.05);
        TASK_CHECK(u_persp < u_aff);
    }

    tglShaderBind(NULL);
    tglShaderRelease(p);
    glClose();
    ZB_close(zb);
    free(efb);
    free(ezb);
    return 0;
}

static int render_tex_matrix(void)
{
    /* r0 must carry UVs after the original texture matrix: u doubled */
    static const GLfloat T[16] = {2, 0, 0, 0, 0, 1, 0, 0,
                                  0, 0, 1, 0, 0, 0, 0, 1};
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    PIXEL *efb = NULL;
    GLushort *ezb = NULL;
    TGLShaderInstr code[2];
    TGLShaderProgram *p = NULL;
    GLuint tn = 0;
    RefVert rv[3];
    static const GLfloat white[4] = {1, 1, 1, 1};

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    build_ramp_texture(tex_data);
    build_ramp_texels(ramp_texels);
    glGenTextures(1, &tn);
    glBindTexture(GL_TEXTURE_2D, tn);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 tex_data);
    glEnable(GL_TEXTURE_2D);
    glMatrixMode(GL_TEXTURE);
    glLoadMatrixf(T);
    /* re-arm the combined-matrix update after the TEXTURE-mode matrix op */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    code[0] = instr(TEX, 3, 0, 0, 0);
    code[1] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 2);
    TASK_CHECK(p != NULL);
    tglShaderBind(p);

    glBegin(GL_TRIANGLES);
    glColor4f(1, 1, 1, 1);
    glTexCoord2f(0, 0);
    glVertex3f(-0.9f, -0.9f, 0.2f);
    glTexCoord2f(0.5f, 0);
    glVertex3f(0.9f, -0.9f, 0.2f);
    glTexCoord2f(0, 0.5f);
    glVertex3f(0, 0.9f, 0.2f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);

    efb = fx_exp_fb();
    ezb = fx_exp_zb();
    TASK_CHECK(efb != NULL && ezb != NULL);
    /* reference sees u after the texture matrix: u' = 2u */
    rv[0] = ref_vertex(-0.9f, -0.9f, 0.2f, 1, 0, 0, white);
    rv[1] = ref_vertex(0.9f, -0.9f, 0.2f, 1, 1.0f, 0, white);
    rv[2] = ref_vertex(0, 0.9f, 0.2f, 1, 0, 0.5f, white);
    ref_triangle(p, ramp_texels, rv, efb, ezb, RW, RH, 1, 1);
    TASK_CHECK(fx_fb_diff(zb->pbuf, efb, RW * RH, COLOR_TOL) == 0);

    tglShaderBind(NULL);
    tglShaderRelease(p);
    glClose();
    ZB_close(zb);
    free(efb);
    free(ezb);
    return 0;
}

static int render_clip_fan(void)
{
    /* a triangle crossing the x < -w clip plane is clipped into fan
     * pieces; the pieces must stitch seamlessly.  The reference
     * rasterizes the same clipped quad (clip plane x = -w exactly, with
     * the clip parameter t = 1/2.6 and colors interpolated with it). */
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    PIXEL *efb = NULL;
    GLushort *ezb = NULL;
    TGLShaderInstr code[1];
    TGLShaderProgram *p = NULL;
    RefVert quad[4];
    static const GLfloat ca[4] = {1, 0.2f, 0.2f, 1};  /* A (outside) */
    static const GLfloat cb[4] = {0.2f, 1, 0.2f, 1};  /* B */
    static const GLfloat cc[4] = {0.2f, 0.2f, 1, 1};  /* C */
    GLfloat cp1[4], cp2[4];
    GLint k;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    code[0] = instr(OUTPUT, 0, 1, 0, 0);
    p = create_ok(code, 1);
    TASK_CHECK(p != NULL);
    tglShaderBind(p);

    /* A x=-2 crosses the left clip plane; B,C stay inside */
    glBegin(GL_TRIANGLES);
    glColor4f(1, 0.2f, 0.2f, 1);
    glVertex3f(-2.0f, -0.8f, 0.5f);
    glColor4f(0.2f, 1, 0.2f, 1);
    glVertex3f(0.6f, -0.8f, 0.5f);
    glColor4f(0.2f, 0.2f, 1, 1);
    glVertex3f(0.6f, 0.8f, 0.5f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);

    /* clipped quad: P1,B,C,P2 with P1 on A->B and P2 on A->C at x=-1;
     * t = (-1 - xA)/(xB - xA) = 1/2.6 = 0.384615... */
    for (k = 0; k < 4; k++) {
        cp1[k] = ca[k] + (1.0f / 2.6f) * (cb[k] - ca[k]);
        cp2[k] = ca[k] + (1.0f / 2.6f) * (cc[k] - ca[k]);
    }
    quad[0] = ref_vertex(-1.0f, -0.8f, 0.5f, 1, 0, 0, cp1);
    quad[1] = ref_vertex(0.6f, -0.8f, 0.5f, 1, 0, 0, cb);
    quad[2] = ref_vertex(0.6f, 0.8f, 0.5f, 1, 0, 0, cc);
    quad[3] = ref_vertex(-1.0f, -0.8f + (1.0f / 2.6f) * 1.6f, 0.5f, 1, 0,
                         0, cp2);

    efb = fx_exp_fb();
    ezb = fx_exp_zb();
    TASK_CHECK(efb != NULL && ezb != NULL);
    {
        RefVert tri[3];
        tri[0] = quad[0];
        tri[1] = quad[1];
        tri[2] = quad[2];
        ref_triangle(p, NULL, tri, efb, ezb, RW, RH, 1, 1);
        tri[0] = quad[0];
        tri[1] = quad[2];
        tri[2] = quad[3];
        ref_triangle(p, NULL, tri, efb, ezb, RW, RH, 1, 1);
    }
    TASK_CHECK(fx_fb_diff(zb->pbuf, efb, RW * RH, COLOR_TOL) == 0);
    TASK_CHECK(fx_zb_diff(zb->zbuf, ezb, RW * RH, DEPTH_TOL) == 0);
    TASK_CHECK(fx_count_nonclear(zb->pbuf, RW * RH) > 100);

    tglShaderBind(NULL);
    tglShaderRelease(p);
    glClose();
    ZB_close(zb);
    free(efb);
    free(ezb);
    return 0;
}

static int render_clip_z(void)
{
    /* a triangle crossing the near plane (z < -w) loses a cap: the
     * clipped fan must cover exactly the clipped polygon.  The reference
     * rasterizes the same clipped quad (clip plane z = -w exactly). */
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    PIXEL *efb = NULL;
    GLushort *ezb = NULL;
    TGLShaderInstr code[1];
    TGLShaderProgram *p = NULL;
    RefVert quad[4];
    static const GLfloat ca[4] = {1, 0.2f, 0.2f, 1};  /* A (clipped away) */
    static const GLfloat cb[4] = {0.2f, 1, 0.2f, 1};  /* B */
    static const GLfloat cc[4] = {0.2f, 0.2f, 1, 1};  /* C */
    GLfloat cp1[4], cp2[4];
    GLint k;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    code[0] = instr(OUTPUT, 0, 1, 0, 0);
    p = create_ok(code, 1);
    TASK_CHECK(p != NULL);
    tglShaderBind(p);

    /* A z=-1.5 crosses the near plane; B,C stay inside */
    glBegin(GL_TRIANGLES);
    glColor4f(1, 0.2f, 0.2f, 1);
    glVertex3f(0.0f, 0.0f, -1.5f);
    glColor4f(0.2f, 1, 0.2f, 1);
    glVertex3f(-0.5f, 0.5f, 0.5f);
    glColor4f(0.2f, 0.2f, 1, 1);
    glVertex3f(0.5f, 0.5f, 0.5f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);

    /* clipped quad: P1,B,C,P2 with P1/P2 the z=-1 intersections;
     * t = (-1 - zA)/(zB - zA) = 0.25, colors interpolate with the same t */
    for (k = 0; k < 4; k++) {
        cp1[k] = ca[k] + 0.25f * (cb[k] - ca[k]);
        cp2[k] = ca[k] + 0.25f * (cc[k] - ca[k]);
    }
    quad[0] = ref_vertex(-0.125f, 0.125f, -1.0f, 1, 0, 0, cp1);
    quad[1] = ref_vertex(-0.5f, 0.5f, 0.5f, 1, 0, 0, cb);
    quad[2] = ref_vertex(0.5f, 0.5f, 0.5f, 1, 0, 0, cc);
    quad[3] = ref_vertex(0.125f, 0.125f, -1.0f, 1, 0, 0, cp2);

    efb = fx_exp_fb();
    ezb = fx_exp_zb();
    TASK_CHECK(efb != NULL && ezb != NULL);
    {
        RefVert tri[3];
        tri[0] = quad[0];
        tri[1] = quad[1];
        tri[2] = quad[2];
        ref_triangle(p, NULL, tri, efb, ezb, RW, RH, 1, 1);
        tri[0] = quad[0];
        tri[1] = quad[2];
        tri[2] = quad[3];
        ref_triangle(p, NULL, tri, efb, ezb, RW, RH, 1, 1);
    }
    TASK_CHECK(fx_fb_diff(zb->pbuf, efb, RW * RH, COLOR_TOL) == 0);
    TASK_CHECK(fx_zb_diff(zb->zbuf, ezb, RW * RH, DEPTH_TOL) == 0);
    TASK_CHECK(fx_count_nonclear(zb->pbuf, RW * RH) > 50);

    tglShaderBind(NULL);
    tglShaderRelease(p);
    glClose();
    ZB_close(zb);
    free(efb);
    free(ezb);
    return 0;
}

static int render_depth_occlusion(void)
{
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    PIXEL *efb = NULL;
    GLushort *ezb = NULL;
    TGLShaderInstr code_r[2], code_b[2];
    TGLShaderProgram *pr = NULL, *pb = NULL;
    RefVert rv[3];
    static const GLfloat red[4] = {1, 0, 0, 1};
    static const GLfloat blue[4] = {0, 0, 1, 1};
    PIXEL center;
    GLint i;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    code_r[0] = iconst(3, 1, 0, 0, 1);
    code_r[1] = instr(OUTPUT, 0, 3, 0, 0);
    code_b[0] = iconst(3, 0, 0, 1, 1);
    code_b[1] = instr(OUTPUT, 0, 3, 0, 0);
    pr = create_ok(code_r, 2);
    pb = create_ok(code_b, 2);
    TASK_CHECK(pr != NULL && pb != NULL);

    /* far (blue, ndc z 0.5) then near (red, ndc z -0.5): near wins */
    tglShaderBind(pb);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, 0.5f);
    glVertex3f(0.8f, -0.8f, 0.5f);
    glVertex3f(0, 0.8f, 0.5f);
    glEnd();
    tglShaderBind(pr);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, -0.5f);
    glVertex3f(0.8f, -0.8f, -0.5f);
    glVertex3f(0, 0.8f, -0.5f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);
    center = zb->pbuf[31 * RW + 31];
    TASK_CHECK(GET_RED(center) > 200 && GET_BLUE(center) < 40);

    /* reverse order: the occluded triangle must not change color/depth */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    tglShaderBind(pr);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, -0.5f);
    glVertex3f(0.8f, -0.8f, -0.5f);
    glVertex3f(0, 0.8f, -0.5f);
    glEnd();
    {
        GLushort znear = zb->zbuf[31 * RW + 31];
        PIXEL cnear = zb->pbuf[31 * RW + 31];
        tglShaderBind(pb);
        glBegin(GL_TRIANGLES);
        glVertex3f(-0.8f, -0.8f, 0.5f);
        glVertex3f(0.8f, -0.8f, 0.5f);
        glVertex3f(0, 0.8f, 0.5f);
        glEnd();
        TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);
        /* far triangle fully occluded here: nothing may change */
        TASK_CHECK(zb->zbuf[31 * RW + 31] == znear);
        TASK_CHECK(zb->pbuf[31 * RW + 31] == cnear);
        TASK_CHECK(GET_RED(zb->pbuf[31 * RW + 31]) > 200);
    }

    /* full-image reference for the two ordered draws */
    efb = fx_exp_fb();
    ezb = fx_exp_zb();
    TASK_CHECK(efb != NULL && ezb != NULL);
    for (i = 0; i < 3; i++) {
        static const GLfloat vx[3] = {-0.8f, 0.8f, 0};
        static const GLfloat vy[3] = {-0.8f, -0.8f, 0.8f};
        rv[i] = ref_vertex(vx[i], vy[i], -0.5f, 1, 0, 0, red);
    }
    ref_triangle(pr, NULL, rv, efb, ezb, RW, RH, 1, 1);
    for (i = 0; i < 3; i++) {
        static const GLfloat vx[3] = {-0.8f, 0.8f, 0};
        static const GLfloat vy[3] = {-0.8f, -0.8f, 0.8f};
        rv[i] = ref_vertex(vx[i], vy[i], 0.5f, 1, 0, 0, blue);
    }
    ref_triangle(pb, NULL, rv, efb, ezb, RW, RH, 1, 1);
    TASK_CHECK(fx_fb_diff(zb->pbuf, efb, RW * RH, COLOR_TOL) == 0);
    TASK_CHECK(fx_zb_diff(zb->zbuf, ezb, RW * RH, DEPTH_TOL) == 0);

    tglShaderBind(NULL);
    tglShaderRelease(pr);
    tglShaderRelease(pb);
    glClose();
    ZB_close(zb);
    free(efb);
    free(ezb);
    return 0;
}

static int render_single_lane(void)
{
    /* tiny triangle: exactly one real lane in its quad; the other three
     * lanes are helpers extrapolating from the triangle's planes */
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    PIXEL *efb = NULL;
    GLushort *ezb = NULL;
    TGLShaderInstr code[2];
    TGLShaderProgram *p = NULL;
    RefVert rv[3];
    static const GLfloat green[4] = {0, 1, 0, 1};
    /* zp targets (30,31), (32,31), (30,32): covers only pixel (30,31) */
    static const GLfloat vx[3] = {-1.75f / 31.75f, 0.25f / 31.75f,
                                  -1.75f / 31.75f};
    static const GLfloat vy[3] = {0.75f / 31.75f, 0.75f / 31.75f,
                                  -0.25f / 31.75f};
    GLint i, covered;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    code[0] = iconst(3, 0, 1, 0, 1);
    code[1] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 2);
    TASK_CHECK(p != NULL);
    tglShaderBind(p);

    glBegin(GL_TRIANGLES);
    for (i = 0; i < 3; i++) {
        glColor4f(0, 1, 0, 1);
        glVertex3f(vx[i], vy[i], 0.2f);
    }
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);

    efb = fx_exp_fb();
    ezb = fx_exp_zb();
    TASK_CHECK(efb != NULL && ezb != NULL);
    for (i = 0; i < 3; i++)
        rv[i] = ref_vertex(vx[i], vy[i], 0.2f, 1, 0, 0, green);
    ref_triangle(p, NULL, rv, efb, ezb, RW, RH, 1, 1);

    covered = fx_count_nonclear(zb->pbuf, RW * RH);
    TASK_CHECK(covered == 1);
    TASK_CHECK(fx_fb_diff(zb->pbuf, efb, RW * RH, COLOR_TOL) == 0);
    /* the written pixel is green */
    for (i = 0; i < RW * RH; i++)
        if (zb->pbuf[i] & 0xFFFFFF) {
            TASK_CHECK(GET_GREEN(zb->pbuf[i]) > 200);
            break;
        }

    tglShaderBind(NULL);
    tglShaderRelease(p);
    glClose();
    ZB_close(zb);
    free(efb);
    free(ezb);
    return 0;
}

static int render_fan_boundary(void)
{
    /* a triangle fan: internal edges are shared by two pieces; every
     * covered pixel must be committed exactly once (checked against the
     * reference, including per-quad derivatives from helper lanes) */
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    PIXEL *efb = NULL;
    GLushort *ezb = NULL;
    TGLShaderInstr code_g[1], code_d[2];
    TGLShaderProgram *pg = NULL, *pd = NULL;
    RefVert rv[6];
    static const GLfloat cols[6][4] = {
        {0.9f, 0.9f, 0.9f, 1}, {1, 0, 0, 1}, {0, 1, 0, 1},
        {0, 0, 1, 1},          {1, 1, 0, 1}, {1, 0, 1, 1},
    };
    static const GLfloat fx[6] = {0, 0.8f, 0.3f, -0.7f, -0.6f, 0.2f};
    static const GLfloat fy[6] = {0, 0, 0.75f, 0.5f, -0.6f, -0.8f};
    static const GLfloat fu[6] = {2, 6, 3.6f, 0.8f, 0.4f, 3.2f};
    static const GLfloat fv[6] = {2, 4, 6.4f, 5.2f, 1.6f, 0.4f};
    GLint i, t;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    code_g[0] = instr(OUTPUT, 0, 1, 0, 0);
    pg = create_ok(code_g, 1);
    code_d[0] = instr(DDX, 3, 0, 0, 0);
    code_d[1] = instr(OUTPUT, 0, 3, 0, 0);
    pd = create_ok(code_d, 2);
    TASK_CHECK(pg != NULL && pd != NULL);
    for (i = 0; i < 6; i++)
        rv[i] = ref_vertex(fx[i], fy[i], 0.2f, 1, fu[i], fv[i], cols[i]);

    /* pass 1: Gouraud colors across the fan */
    tglShaderBind(pg);
    glBegin(GL_TRIANGLE_FAN);
    for (i = 0; i < 6; i++) {
        glColor4f(cols[i][0], cols[i][1], cols[i][2], cols[i][3]);
        glTexCoord2f(fu[i], fv[i]);
        glVertex3f(fx[i], fy[i], 0.2f);
    }
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);
    efb = fx_exp_fb();
    ezb = fx_exp_zb();
    TASK_CHECK(efb != NULL && ezb != NULL);
    for (t = 0; t < 4; t++) {
        RefVert tri[3];
        tri[0] = rv[0];
        tri[1] = rv[t + 1];
        tri[2] = rv[t + 2];
        ref_triangle(pg, NULL, tri, efb, ezb, RW, RH, 1, 1);
    }
    TASK_CHECK(fx_fb_diff(zb->pbuf, efb, RW * RH, COLOR_TOL) == 0);
    TASK_CHECK(fx_zb_diff(zb->zbuf, ezb, RW * RH, DEPTH_TOL) == 0);

    /* pass 2: DDX of the UVs — the value at a real lane depends on the
     * helper lanes extrapolating the *owning* triangle's attributes */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    memset(efb, 0, (size_t) RW * RH * sizeof(PIXEL));
    memset(ezb, 0, (size_t) RW * RH * sizeof(GLushort));
    tglShaderBind(pd);
    glBegin(GL_TRIANGLE_FAN);
    for (i = 0; i < 6; i++) {
        glColor4f(cols[i][0], cols[i][1], cols[i][2], cols[i][3]);
        glTexCoord2f(fu[i], fv[i]);
        glVertex3f(fx[i], fy[i], 0.2f);
    }
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);
    for (t = 0; t < 4; t++) {
        RefVert tri[3];
        tri[0] = rv[0];
        tri[1] = rv[t + 1];
        tri[2] = rv[t + 2];
        ref_triangle(pd, NULL, tri, efb, ezb, RW, RH, 1, 1);
    }
    TASK_CHECK(fx_fb_diff(zb->pbuf, efb, RW * RH, COLOR_TOL) == 0);

    tglShaderBind(NULL);
    tglShaderRelease(pg);
    tglShaderRelease(pd);
    glClose();
    ZB_close(zb);
    free(efb);
    free(ezb);
    return 0;
}

static int render_unbind_restore(void)
{
    /* after unbinding, the fixed pipeline produces the old image again */
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    PIXEL *f0 = NULL, *f1 = NULL;
    TGLShaderInstr code[2];
    TGLShaderProgram *p = NULL;
    GLint i;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    f0 = (PIXEL *) malloc((size_t) RW * RH * sizeof(PIXEL));
    f1 = (PIXEL *) malloc((size_t) RW * RH * sizeof(PIXEL));
    TASK_CHECK(f0 != NULL && f1 != NULL);

    /* fixed pipeline image */
    glBegin(GL_TRIANGLES);
    glColor4f(1, 0, 0, 1);
    glVertex3f(-0.8f, -0.8f, 0.2f);
    glColor4f(0, 1, 0, 1);
    glVertex3f(0.8f, -0.8f, 0.2f);
    glColor4f(0, 0, 1, 1);
    glVertex3f(0, 0.8f, 0.2f);
    glEnd();
    memcpy(f0, zb->pbuf, (size_t) RW * RH * sizeof(PIXEL));

    /* same geometry with a flat-red program bound */
    code[0] = iconst(3, 1, 0, 0, 1);
    code[1] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 2);
    TASK_CHECK(p != NULL);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    tglShaderBind(p);
    glBegin(GL_TRIANGLES);
    glColor4f(1, 0, 0, 1);
    glVertex3f(-0.8f, -0.8f, 0.2f);
    glColor4f(0, 1, 0, 1);
    glVertex3f(0.8f, -0.8f, 0.2f);
    glColor4f(0, 0, 1, 1);
    glVertex3f(0, 0.8f, 0.2f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);
    TASK_CHECK(fx_fb_diff(zb->pbuf, f0, RW * RH, 0) > 100);

    /* unbind: the old fixed-pipeline image must come back, bit exact */
    tglShaderBind(NULL);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor4f(1, 0, 0, 1);
    glVertex3f(-0.8f, -0.8f, 0.2f);
    glColor4f(0, 1, 0, 1);
    glVertex3f(0.8f, -0.8f, 0.2f);
    glColor4f(0, 0, 1, 1);
    glVertex3f(0, 0.8f, 0.2f);
    glEnd();
    memcpy(f1, zb->pbuf, (size_t) RW * RH * sizeof(PIXEL));
    for (i = 0; i < RW * RH; i++)
        TASK_CHECK(f0[i] == f1[i]);

    tglShaderRelease(p);
    glClose();
    ZB_close(zb);
    free(f0);
    free(f1);
    return 0;
}

static int render_display_list(void)
{
    /* binding is immediate state and is not recorded into display lists:
     * a listed triangle replays with whatever program is bound then */
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    TGLShaderInstr cr[2], cg[2];
    TGLShaderProgram *pr = NULL, *pg = NULL;
    GLuint list;
    PIXEL center;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    cr[0] = iconst(3, 1, 0, 0, 1);
    cr[1] = instr(OUTPUT, 0, 3, 0, 0);
    cg[0] = iconst(3, 0, 1, 0, 1);
    cg[1] = instr(OUTPUT, 0, 3, 0, 0);
    pr = create_ok(cr, 2);
    pg = create_ok(cg, 2);
    TASK_CHECK(pr != NULL && pg != NULL);

    /* compile the list while red is bound: the binding must NOT stick */
    tglShaderBind(pr);
    list = glGenLists(1);
    TASK_CHECK(glIsList(list));
    glNewList(list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    glColor4f(0.1f, 0.6f, 0.2f, 1);
    glVertex3f(-0.8f, -0.8f, 0.2f);
    glColor4f(0.2f, 0.6f, 0.1f, 1);
    glVertex3f(0.8f, -0.8f, 0.2f);
    glColor4f(0.1f, 0.2f, 0.6f, 1);
    glVertex3f(0, 0.8f, 0.2f);
    glEnd();
    glEndList();

    /* replay with green bound: green wins */
    tglShaderBind(pg);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCallList(list);
    center = zb->pbuf[31 * RW + 31];
    TASK_CHECK(GET_GREEN(center) > 200 && GET_RED(center) < 40);

    /* replay with red bound: red wins */
    tglShaderBind(pr);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCallList(list);
    center = zb->pbuf[31 * RW + 31];
    TASK_CHECK(GET_RED(center) > 200 && GET_GREEN(center) < 40);

    /* replay unbound: the fixed pipeline Gouraud colors come back */
    tglShaderBind(NULL);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCallList(list);
    center = zb->pbuf[31 * RW + 31];
    TASK_CHECK(GET_RED(center) > 10 && GET_GREEN(center) > 10);
    TASK_CHECK(GET_RED(center) < 250 || GET_GREEN(center) < 250);

    glDeleteList(list);
    tglShaderRelease(pr);
    tglShaderRelease(pg);
    glClose();
    ZB_close(zb);
    return 0;
}

static int render_reject_config(void)
{
    /* blending, GL_FLAT and point/line polygon modes are rejected */
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    TGLShaderInstr code[2];
    TGLShaderProgram *p = NULL;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    code[0] = iconst(3, 1, 0, 0, 1);
    code[1] = instr(OUTPUT, 0, 3, 0, 0);
    p = create_ok(code, 2);
    TASK_CHECK(p != NULL);
    tglShaderBind(p);

#define DRAW_ONE()                             \
    do {                                       \
        glBegin(GL_TRIANGLES);                 \
        glColor4f(1, 1, 1, 1);                 \
        glVertex3f(-0.8f, -0.8f, 0.2f);        \
        glVertex3f(0.8f, -0.8f, 0.2f);         \
        glVertex3f(0, 0.8f, 0.2f);             \
        glEnd();                               \
    } while (0)

    glEnable(GL_BLEND);
    DRAW_ONE();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_UNSUPPORTED);
    TASK_CHECK(fx_count_nonclear(zb->pbuf, RW * RH) == 0);
    glDisable(GL_BLEND);

    glShadeModel(GL_FLAT);
    DRAW_ONE();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_UNSUPPORTED);
    TASK_CHECK(fx_count_nonclear(zb->pbuf, RW * RH) == 0);
    glShadeModel(GL_SMOOTH);

    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    DRAW_ONE();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_UNSUPPORTED);
    TASK_CHECK(fx_count_nonclear(zb->pbuf, RW * RH) == 0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
    DRAW_ONE();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_UNSUPPORTED);
    TASK_CHECK(fx_count_nonclear(zb->pbuf, RW * RH) == 0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* restored configuration draws again with no new error */
    DRAW_ONE();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);
    TASK_CHECK(fx_count_nonclear(zb->pbuf, RW * RH) > 100);

#undef DRAW_ONE
    tglShaderBind(NULL);
    tglShaderRelease(p);
    glClose();
    ZB_close(zb);
    return 0;
}

static int render_depth_mask(void)
{
    /* the depth write toggle is honored in program mode */
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    TGLShaderInstr cr[2], cb[2];
    TGLShaderProgram *pr = NULL, *pb = NULL;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    cr[0] = iconst(3, 1, 0, 0, 1);
    cr[1] = instr(OUTPUT, 0, 3, 0, 0);
    cb[0] = iconst(3, 0, 0, 1, 1);
    cb[1] = instr(OUTPUT, 0, 3, 0, 0);
    pr = create_ok(cr, 2);
    pb = create_ok(cb, 2);
    TASK_CHECK(pr != NULL && pb != NULL);

    /* near red with depth writes off, then far blue: blue overwrites
     * because the near depth was never recorded */
    glDepthMask(GL_FALSE);
    tglShaderBind(pr);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, -0.5f);
    glVertex3f(0.8f, -0.8f, -0.5f);
    glVertex3f(0, 0.8f, -0.5f);
    glEnd();
    TASK_CHECK(zb->zbuf[31 * RW + 31] == 0); /* depth untouched */
    TASK_CHECK(GET_RED(zb->pbuf[31 * RW + 31]) > 200);
    tglShaderBind(pb);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, 0.5f);
    glVertex3f(0.8f, -0.8f, 0.5f);
    glVertex3f(0, 0.8f, 0.5f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);
    TASK_CHECK(GET_BLUE(zb->pbuf[31 * RW + 31]) > 200);
    TASK_CHECK(zb->zbuf[31 * RW + 31] == 0);

    /* with depth writes on, the near triangle wins as usual */
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    tglShaderBind(pr);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, -0.5f);
    glVertex3f(0.8f, -0.8f, -0.5f);
    glVertex3f(0, 0.8f, -0.5f);
    glEnd();
    tglShaderBind(pb);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, 0.5f);
    glVertex3f(0.8f, -0.8f, 0.5f);
    glVertex3f(0, 0.8f, 0.5f);
    glEnd();
    TASK_CHECK(GET_RED(zb->pbuf[31 * RW + 31]) > 200);
    TASK_CHECK(zb->zbuf[31 * RW + 31] != 0);

    tglShaderBind(NULL);
    tglShaderRelease(pr);
    tglShaderRelease(pb);
    glClose();
    ZB_close(zb);
    return 0;
}

/* ndc coordinate that truncates to the desired integer zp exactly */
static GLfloat ndc_for_zp(GLint z)
{
    return ((GLfloat) z + 0.5f - 31.75f) / 31.75f;
}

static int render_zero_divide(void)
{
    /* a helper lane whose extrapolated 1/clip.w is exactly zero: the whole
     * quad is rejected (no lane commits), the error is reported, and quads
     * completed earlier are kept.
     * P2 (column-major) maps eye (x,y,z) to clip (x, y, 0.1, -z). */
    static const GLfloat P2[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                   0, 0, 0, -1, 0, 0, 0.1f, 0};
    ZBuffer *zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    TGLShaderInstr cg[2], cb[2];
    TGLShaderProgram *pg = NULL, *pb = NULL;
    GLint i, good_count;

    TASK_CHECK(zb != NULL);
    fx_begin(zb);
    cg[0] = iconst(3, 0, 1, 0, 1);
    cg[1] = instr(OUTPUT, 0, 3, 0, 0);
    cb[0] = iconst(3, 0, 0, 1, 1);
    cb[1] = instr(OUTPUT, 0, 3, 0, 0);
    pg = create_ok(cg, 2);
    pb = create_ok(cb, 2);
    TASK_CHECK(pg != NULL && pb != NULL);

    /* a good green triangle that must survive the later failure */
    tglShaderBind(pg);
    glBegin(GL_TRIANGLES);
    glVertex3f(ndc_for_zp(40), -ndc_for_zp(40), 0.2f);
    glVertex3f(ndc_for_zp(48), -ndc_for_zp(40), 0.2f);
    glVertex3f(ndc_for_zp(40), -ndc_for_zp(48), 0.2f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_OK);
    good_count = fx_count_nonclear(zb->pbuf, RW * RH);
    TASK_CHECK(good_count > 0);

    /* bad triangle: zp (10,10),(12,10),(10,12) with 1/w = (6,2,2), so the
     * extrapolated 1/w at the helper lane (11.5,11.5) is exactly 0 */
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(P2);
    tglShaderBind(pb);
    glBegin(GL_TRIANGLES);
    for (i = 0; i < 3; i++) {
        static const GLint tzp[3][2] = {{10, 10}, {12, 10}, {10, 12}};
        static const GLfloat twinv[3] = {6, 2, 2};
        GLfloat w = 1.0f / twinv[i];
        glVertex3f(ndc_for_zp(tzp[i][0]) * w, -ndc_for_zp(tzp[i][1]) * w,
                   -w);
    }
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_EXEC_ZERO_DIVIDE);

    /* the failing quad committed nothing */
    TASK_CHECK((zb->pbuf[10 * RW + 10] & 0xFFFFFF) == 0);
    TASK_CHECK((zb->pbuf[10 * RW + 11] & 0xFFFFFF) == 0);
    TASK_CHECK((zb->pbuf[11 * RW + 10] & 0xFFFFFF) == 0);
    TASK_CHECK((zb->pbuf[11 * RW + 11] & 0xFFFFFF) == 0);
    /* the earlier good triangle is fully retained */
    TASK_CHECK(fx_count_nonclear(zb->pbuf, RW * RH) == good_count);
    TASK_CHECK(GET_GREEN(zb->pbuf[42 * RW + 42]) > 200);

    tglShaderBind(NULL);
    tglShaderRelease(pg);
    tglShaderRelease(pb);
    glClose();
    ZB_close(zb);
    return 0;
}

/* ================================================================== */
/* case table                                                          */
/* ================================================================== */

const struct task_case task_cases[] = {
    {"verify", "lifecycle", verify_lifecycle},
    {"verify", "both_sides_read", verify_both_sides_read},
    {"verify", "missing_else", verify_missing_else},
    {"verify", "break_continue_skip", verify_break_continue_skip},
    {"verify", "operands", verify_operands},
    {"verify", "repeat_count", verify_repeat_count},
    {"verify", "structure", verify_structure},
    {"verify", "output_rule", verify_output_rule},
    {"verify", "budget", verify_budget},
    {"verify", "derivative", verify_derivative},
    {"verify", "loop_first_iteration", verify_loop_first_iteration},
    {"verify", "create_keeps_binding", verify_create_keeps_binding},
    {"control", "nested_branch", control_nested_branch},
    {"control", "loop_exit", control_loop_exit},
    {"control", "inner_continue_break", control_inner_continue_break},
    {"control", "discard_derivative", control_discard_derivative},
    {"control", "tex", control_tex},
    {"control", "nonfinite", control_nonfinite},
    {"control", "zero_coverage", control_zero_coverage},
    {"control", "alias", control_alias},
    {"render", "gouraud", render_gouraud},
    {"render", "r2_inputs", render_r2_inputs},
    {"render", "tex256", render_tex256},
    {"render", "perspective", render_perspective},
    {"render", "tex_matrix", render_tex_matrix},
    {"render", "clip_fan", render_clip_fan},
    {"render", "clip_z", render_clip_z},
    {"render", "depth_occlusion", render_depth_occlusion},
    {"render", "single_lane", render_single_lane},
    {"render", "fan_boundary", render_fan_boundary},
    {"render", "unbind_restore", render_unbind_restore},
    {"render", "display_list", render_display_list},
    {"render", "reject_config", render_reject_config},
    {"render", "depth_mask", render_depth_mask},
    {"render", "zero_divide", render_zero_divide},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
