/*
 * Checks for the fragment bytecode shader (verify / control / render).
 *
 * Exports task_cases/task_case_count for task_support/driver.c.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <TGL/gl.h>
#include <TGL/glshader.h>
#include "zbuffer.h"
#include "task_support/check.h"

#define ARRLEN(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------ */
/* instruction builders                                                 */
/* ------------------------------------------------------------------ */

static TGLShaderInst mk(int op, int dst, int a, int b, int c)
{
    TGLShaderInst in;
    memset(&in, 0, sizeof in);
    in.op = op;
    in.dst = dst;
    in.a = a;
    in.b = b;
    in.c = c;
    return in;
}

static TGLShaderInst mkc(int dst, float x, float y, float z, float w)
{
    TGLShaderInst in = mk(TGL_SHADER_CONST, dst, 0, 0, 0);
    in.imm[0] = x;
    in.imm[1] = y;
    in.imm[2] = z;
    in.imm[3] = w;
    return in;
}

static TGLShaderInst mkr(unsigned count)
{
    TGLShaderInst in = mk(TGL_SHADER_REPEAT, 0, 0, 0, 0);
    in.count = count;
    return in;
}

static int expect_err(const TGLShaderInst *code, unsigned n, int want_code,
                      int want_index)
{
    TGLShaderError err;
    TGLShaderProgram *p;
    err.code = -1;
    err.index = -2;
    p = tglShaderCreate(code, n, &err);
    if (p) {
        tglShaderRelease(p);
        return 0;
    }
    return err.code == want_code && err.index == want_index;
}

static int expect_ok(const TGLShaderInst *code, unsigned n)
{
    TGLShaderError err;
    TGLShaderProgram *p;
    err.code = -1;
    err.index = -2;
    p = tglShaderCreate(code, n, &err);
    if (!p)
        return 0;
    tglShaderRelease(p);
    return err.code == TGL_SHADER_OK;
}

/* ------------------------------------------------------------------ */
/* verify group: validation                                             */
/* ------------------------------------------------------------------ */

static int v_both_sides(void)
{
    TGLShaderInst c[7];
    c[0] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    c[1] = mkc(5, 1, 0, 0, 1);
    c[2] = mk(TGL_SHADER_ELSE, 0, 0, 0, 0);
    c[3] = mkc(5, 0, 1, 0, 1);
    c[4] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[5] = mk(TGL_SHADER_MOV, 6, 5, 0, 0);
    c[6] = mk(TGL_SHADER_OUTPUT, 0, 6, 0, 0);
    TASK_CHECK(expect_ok(c, ARRLEN(c)));
    /* only the then side assigns: the read after the block is undefined */
    c[3] = mk(TGL_SHADER_MOV, 7, 0, 0, 0);
    TASK_CHECK(expect_err(c, ARRLEN(c), TGL_SHADER_ERR_UNDEF, 5));
    return 0;
}

static int v_missing_else(void)
{
    TGLShaderInst c[4];
    c[0] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    c[1] = mkc(5, 1, 1, 1, 1);
    c[2] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[3] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    TASK_CHECK(expect_err(c, ARRLEN(c), TGL_SHADER_ERR_UNDEF, 3));
    return 0;
}

static int v_undef_lowest_index(void)
{
    TGLShaderInst c[6];
    /* two undefined reads: the lowest original index is reported */
    c[0] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    c[1] = mkc(5, 1, 1, 1, 1);
    c[2] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[3] = mk(TGL_SHADER_MOV, 6, 7, 0, 0); /* r7 never defined */
    c[4] = mk(TGL_SHADER_MOV, 8, 5, 0, 0); /* r5 maybe undefined */
    c[5] = mk(TGL_SHADER_OUTPUT, 0, 8, 0, 0);
    TASK_CHECK(expect_err(c, ARRLEN(c), TGL_SHADER_ERR_UNDEF, 3));
    /* several undefined sources in one instruction report that index */
    c[3] = mk(TGL_SHADER_ADD, 6, 7, 9, 0);
    c[4] = mk(TGL_SHADER_MOV, 8, 6, 0, 0);
    TASK_CHECK(expect_err(c, ARRLEN(c), TGL_SHADER_ERR_UNDEF, 3));
    return 0;
}

static int v_repeat_semantics(void)
{
    TGLShaderInst c[5];
    /* REPEAT executes its body at least once */
    c[0] = mkr(1);
    c[1] = mkc(5, 1, 1, 1, 1);
    c[2] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[3] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    TASK_CHECK(expect_ok(c, 4));
    /* ...but BREAK_IF in the first iteration can skip the definition */
    c[0] = mkr(1);
    c[1] = mk(TGL_SHADER_BREAK_IF, 0, 0, 0, 0);
    c[2] = mkc(5, 1, 1, 1, 1);
    c[3] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[4] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    TASK_CHECK(expect_err(c, 5, TGL_SHADER_ERR_UNDEF, 4));
    return 0;
}

static int v_break_bypass(void)
{
    TGLShaderInst c[7];
    c[0] = mkr(2);
    c[1] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    c[2] = mkc(5, 1, 1, 1, 1);
    c[3] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[4] = mk(TGL_SHADER_BREAK_IF, 0, 1, 0, 0);
    c[5] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[6] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    /* break in the first iteration with r0 false leaves r5 undefined */
    TASK_CHECK(expect_err(c, ARRLEN(c), TGL_SHADER_ERR_UNDEF, 6));
    return 0;
}

static int v_continue_bypass(void)
{
    TGLShaderInst c[5];
    c[0] = mkr(2);
    c[1] = mk(TGL_SHADER_CONTINUE_IF, 0, 0, 0, 0);
    c[2] = mkc(5, 1, 1, 1, 1);
    c[3] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[4] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    /* continue can skip the only definition in every iteration */
    TASK_CHECK(expect_err(c, ARRLEN(c), TGL_SHADER_ERR_UNDEF, 4));
    return 0;
}

static int v_defined_before_loop(void)
{
    TGLShaderInst c[6];
    c[0] = mkc(5, 0, 0, 0, 0);
    c[1] = mkr(2);
    c[2] = mk(TGL_SHADER_CONTINUE_IF, 0, 0, 0, 0);
    c[3] = mkc(5, 1, 1, 1, 1);
    c[4] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[5] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    /* r5 defined before the loop: skip is harmless */
    TASK_CHECK(expect_ok(c, ARRLEN(c)));
    return 0;
}

static int v_bad_operands(void)
{
    TGLShaderInst c[2];
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);

    c[0] = mkc(2, 1, 1, 1, 1); /* r2 is read-only */
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_OPERAND, 0));
    c[0] = mkc(16, 1, 1, 1, 1); /* dst out of range */
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_OPERAND, 0));
    c[0] = mkc(-1, 1, 1, 1, 1); /* negative dst */
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_OPERAND, 0));
    c[0] = mk(TGL_SHADER_MOV, 3, 16, 0, 0); /* src out of range */
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_OPERAND, 0));
    c[0] = mk(TGL_SHADER_MOV, 3, -1, 0, 0); /* negative src */
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_OPERAND, 0));
    c[0] = mk(TGL_SHADER_MAD, 3, 0, 1, 16); /* third src out of range */
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_OPERAND, 0));
    c[0] = mk(TGL_SHADER_MOV, 3, 0, 99, 0); /* unused field ignored */
    TASK_CHECK(expect_ok(c, 2));
    c[0] = mk(TGL_SHADER_MOV, 15, 0, 0, 0); /* boundary registers ok */
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 15, 0, 0);
    TASK_CHECK(expect_ok(c, 2));
    return 0;
}

static int v_structure(void)
{
    TGLShaderInst c[5];
    /* ELSE without IF */
    c[0] = mk(TGL_SHADER_ELSE, 0, 0, 0, 0);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_STRUCTURE, 0));
    /* ENDIF without IF */
    c[0] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_STRUCTURE, 0));
    /* ENDREPEAT without REPEAT */
    c[0] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_STRUCTURE, 0));
    /* unclosed IF reports its opening instruction */
    c[0] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    c[1] = mk(TGL_SHADER_MOV, 3, 0, 0, 0);
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_STRUCTURE, 0));
    /* unclosed REPEAT reports its opening instruction */
    c[0] = mkr(2);
    c[1] = mk(TGL_SHADER_MOV, 3, 0, 0, 0);
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_STRUCTURE, 0));
    /* missing OUTPUT */
    c[0] = mk(TGL_SHADER_MOV, 3, 0, 0, 0);
    TASK_CHECK(expect_err(c, 1, TGL_SHADER_ERR_STRUCTURE, 0));
    /* OUTPUT not final */
    c[0] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);
    c[1] = mk(TGL_SHADER_MOV, 3, 0, 0, 0);
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_STRUCTURE, 0));
    /* OUTPUT inside a block */
    c[0] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);
    c[2] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    TASK_CHECK(expect_err(c, 3, TGL_SHADER_ERR_STRUCTURE, 1));
    /* second ELSE */
    c[0] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    c[1] = mk(TGL_SHADER_ELSE, 0, 0, 0, 0);
    c[2] = mk(TGL_SHADER_ELSE, 0, 0, 0, 0);
    c[3] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[4] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(expect_err(c, 5, TGL_SHADER_ERR_STRUCTURE, 2));
    /* ENDIF closing a REPEAT */
    c[0] = mkr(2);
    c[1] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[2] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[3] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(expect_err(c, 4, TGL_SHADER_ERR_STRUCTURE, 1));
    /* ENDREPEAT closing an IF */
    c[0] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    c[1] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[2] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[3] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(expect_err(c, 4, TGL_SHADER_ERR_STRUCTURE, 1));
    /* structure is validated before definite initialization: the
     * structural error wins even with an undefined read earlier */
    c[0] = mk(TGL_SHADER_MOV, 4, 5, 0, 0); /* r5 undefined */
    c[1] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_STRUCTURE, 1));
    return 0;
}

static int v_loopctl(void)
{
    TGLShaderInst c[4];
    /* BREAK_IF outside any REPEAT */
    c[0] = mk(TGL_SHADER_BREAK_IF, 0, 0, 0, 0);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_LOOPCTL, 0));
    /* CONTINUE_IF inside IF but outside any REPEAT */
    c[0] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    c[1] = mk(TGL_SHADER_CONTINUE_IF, 0, 0, 0, 0);
    c[2] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[3] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(expect_err(c, 4, TGL_SHADER_ERR_LOOPCTL, 1));
    /* BREAK_IF inside REPEAT is fine */
    c[0] = mkr(2);
    c[1] = mk(TGL_SHADER_BREAK_IF, 0, 0, 0, 0);
    c[2] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[3] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(expect_ok(c, 4));
    return 0;
}

static int v_derivative(void)
{
    TGLShaderInst c[5];
    /* DDX inside IF */
    c[0] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    c[1] = mk(TGL_SHADER_DDX, 3, 0, 0, 0);
    c[2] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[3] = mk(TGL_SHADER_MOV, 4, 3, 0, 0);
    c[4] = mk(TGL_SHADER_OUTPUT, 0, 4, 0, 0);
    TASK_CHECK(expect_err(c, 5, TGL_SHADER_ERR_DERIVATIVE, 1));
    /* DDY inside REPEAT */
    c[0] = mkr(2);
    c[1] = mk(TGL_SHADER_DDY, 3, 0, 0, 0);
    c[2] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[3] = mk(TGL_SHADER_MOV, 4, 3, 0, 0);
    c[4] = mk(TGL_SHADER_OUTPUT, 0, 4, 0, 0);
    TASK_CHECK(expect_err(c, 5, TGL_SHADER_ERR_DERIVATIVE, 1));
    /* top level derivatives are fine */
    c[0] = mk(TGL_SHADER_DDX, 3, 0, 0, 0);
    c[1] = mk(TGL_SHADER_DDY, 4, 0, 0, 0);
    c[2] = mk(TGL_SHADER_ADD, 5, 3, 4, 0);
    c[3] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    TASK_CHECK(expect_ok(c, 4));
    return 0;
}

static int v_budget(void)
{
    TGLShaderInst c[24];
    int n, i;

    /* 15 * 16 * 16 = 3840 plus OUTPUT: fits the 4096 budget */
    n = 0;
    c[n++] = mkr(16);
    c[n++] = mkr(16);
    for (i = 0; i < 15; i++)
        c[n++] = mkc(5, 1, 1, 1, 1);
    c[n++] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[n++] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[n++] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    TASK_CHECK(expect_ok(c, (unsigned) n));

    /* 16 * 16 * 16 = 4096 plus OUTPUT: exceeds at the final instruction */
    n = 0;
    c[n++] = mkr(16);
    c[n++] = mkr(16);
    for (i = 0; i < 16; i++)
        c[n++] = mkc(5, 1, 1, 1, 1);
    c[n++] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[n++] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[n++] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    TASK_CHECK(expect_err(c, (unsigned) n, TGL_SHADER_ERR_BUDGET, 20));

    /* 17 * 16 * 16: exceeds inside the loops, reports the inner REPEAT */
    n = 0;
    c[n++] = mkr(16);
    c[n++] = mkr(16);
    for (i = 0; i < 17; i++)
        c[n++] = mkc(5, 1, 1, 1, 1);
    c[n++] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[n++] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[n++] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    TASK_CHECK(expect_err(c, (unsigned) n, TGL_SHADER_ERR_BUDGET, 1));
    return 0;
}

static int v_repeat_count(void)
{
    TGLShaderInst c[3];
    c[1] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[2] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);
    c[0] = mkr(0);
    TASK_CHECK(expect_err(c, 3, TGL_SHADER_ERR_COUNT, 0));
    c[0] = mkr(17);
    TASK_CHECK(expect_err(c, 3, TGL_SHADER_ERR_COUNT, 0));
    c[0] = mkr(16);
    TASK_CHECK(expect_ok(c, 3));
    c[0] = mkr(1);
    TASK_CHECK(expect_ok(c, 3));
    return 0;
}

static int v_imm(void)
{
    TGLShaderInst c[2];
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    c[0] = mkc(3, (float) NAN, 0, 0, 1);
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_IMM, 0));
    c[0] = mkc(3, 0, (float) INFINITY, 0, 1);
    TASK_CHECK(expect_err(c, 2, TGL_SHADER_ERR_IMM, 0));
    c[0] = mkc(3, 0, 1e30f, -1e30f, 1);
    TASK_CHECK(expect_ok(c, 2));
    return 0;
}

static int v_size(void)
{
    TGLShaderInst c[260];
    TGLShaderError err;
    TGLShaderProgram *p;
    unsigned i;
    c[0] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);

    err.code = -1;
    err.index = -2;
    p = tglShaderCreate(c, 0, &err);
    TASK_CHECK(p == NULL && err.code == TGL_SHADER_ERR_SIZE);

    for (i = 0; i < 259; i++)
        c[i] = mk(TGL_SHADER_MOV, 3, 0, 0, 0);
    c[258] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = tglShaderCreate(c, 259, &err);
    TASK_CHECK(p == NULL && err.code == TGL_SHADER_ERR_SIZE);

    /* 256 instructions is the most allowed: 255 MOVs + OUTPUT */
    c[255] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = tglShaderCreate(c, 256, &err);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);
    return 0;
}

static int v_depth(void)
{
    TGLShaderInst c[20];
    int n, i;
    /* 9 nested IFs exceed the depth limit of 8 */
    n = 0;
    for (i = 0; i < 9; i++)
        c[n++] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    TASK_CHECK(expect_err(c, (unsigned) n, TGL_SHADER_ERR_DEPTH, 8));
    /* 8 nested IFs are fine */
    n = 0;
    for (i = 0; i < 8; i++)
        c[n++] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    for (i = 0; i < 8; i++)
        c[n++] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[n++] = mk(TGL_SHADER_OUTPUT, 0, 0, 0, 0);
    TASK_CHECK(expect_ok(c, (unsigned) n));
    return 0;
}

static int v_create_keeps_binding(void)
{
    TGLShaderInst good[2], bad[2];
    TGLShaderProgram *p, *q;
    TGLShaderError err;

    good[0] = mkc(3, 1, 0, 0, 1);
    good[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    bad[0] = mkc(1, 1, 0, 0, 1); /* r1 is read-only */
    bad[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);

    p = tglShaderCreate(good, 2, &err);
    TASK_CHECK(p != NULL);
    tglBindShader(p);
    TASK_CHECK(tglGetBoundShader() == p);

    /* a failed creation must not change the binding */
    q = tglShaderCreate(bad, 2, &err);
    TASK_CHECK(q == NULL);
    TASK_CHECK(err.code == TGL_SHADER_ERR_OPERAND && err.index == 0);
    TASK_CHECK(tglGetBoundShader() == p);

    tglBindShader(NULL);
    TASK_CHECK(tglGetBoundShader() == NULL);
    tglShaderRelease(p);

    /* NULL error pointer is accepted */
    p = tglShaderCreate(good, 2, NULL);
    TASK_CHECK(p != NULL);
    tglShaderRelease(p);
    q = tglShaderCreate(bad, 2, NULL);
    TASK_CHECK(q == NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* control group: independent quad execution vs per-lane reference      */
/* ------------------------------------------------------------------ */

typedef struct RefQ {
    const TGLShaderInst *code;
    int n;
    float R[4][16][4];
    float out[4][4];
    unsigned disc;
    const unsigned char *tex;
} RefQ;

static int ref_match(const TGLShaderInst *code, int n, int i)
{
    int d = 0, j;
    for (j = i; j < n; j++) {
        int op = code[j].op;
        if (op == TGL_SHADER_IF || op == TGL_SHADER_REPEAT)
            d++;
        else if (op == TGL_SHADER_ENDIF || op == TGL_SHADER_ENDREPEAT) {
            d--;
            if (d == 0)
                return j;
        }
    }
    return n - 1;
}

static int ref_else(const TGLShaderInst *code, int n, int i)
{
    int d = 0, j;
    for (j = i + 1; j < n; j++) {
        int op = code[j].op;
        if (op == TGL_SHADER_IF || op == TGL_SHADER_REPEAT)
            d++;
        else if (op == TGL_SHADER_ENDIF || op == TGL_SHADER_ENDREPEAT) {
            if (d == 0)
                return -1;
            d--;
        } else if (op == TGL_SHADER_ELSE && d == 0)
            return j;
    }
    return -1;
}

static void ref_scalar(RefQ *q, int lane, const TGLShaderInst *in)
{
    float(*R)[4] = q->R[lane];
    float t[4];
    int k;
    switch (in->op) {
    case TGL_SHADER_CONST:
        for (k = 0; k < 4; k++)
            t[k] = in->imm[k];
        break;
    case TGL_SHADER_MOV:
        for (k = 0; k < 4; k++)
            t[k] = R[in->a][k];
        break;
    case TGL_SHADER_ADD:
        for (k = 0; k < 4; k++)
            t[k] = R[in->a][k] + R[in->b][k];
        break;
    case TGL_SHADER_MUL:
        for (k = 0; k < 4; k++)
            t[k] = R[in->a][k] * R[in->b][k];
        break;
    case TGL_SHADER_MAD:
        for (k = 0; k < 4; k++)
            t[k] = R[in->a][k] * R[in->b][k] + R[in->c][k];
        break;
    case TGL_SHADER_LT:
        for (k = 0; k < 4; k++)
            t[k] = R[in->a][k] < R[in->b][k] ? 1.0f : 0.0f;
        break;
    case TGL_SHADER_TEX: {
        float u = R[in->a][0], v = R[in->a][1];
        float fu = u - floorf(u), fv = v - floorf(v);
        int tx = (int) floorf(fu * 256.0f);
        int ty = (int) floorf(fv * 256.0f);
        const unsigned char *p;
        if (tx < 0)
            tx = 0;
        if (tx > 255)
            tx = 255;
        if (ty < 0)
            ty = 0;
        if (ty > 255)
            ty = 255;
        p = q->tex + ((unsigned) ty * 256u + (unsigned) tx) * 4u;
        t[0] = p[0] / 255.0f;
        t[1] = p[1] / 255.0f;
        t[2] = p[2] / 255.0f;
        t[3] = p[3] / 255.0f;
        break;
    }
    case TGL_SHADER_DISCARD_IF:
        if (R[in->a][0] != 0.0f)
            q->disc |= (unsigned) (1u << lane);
        return;
    case TGL_SHADER_OUTPUT:
        for (k = 0; k < 4; k++)
            q->out[lane][k] = R[in->a][k];
        return;
    default:
        return;
    }
    for (k = 0; k < 4; k++)
        R[in->dst][k] = t[k];
}

/* scalar per-lane block execution; returns 1 = break, 2 = continue */
static int ref_exec(RefQ *q, int lane, int s, int e)
{
    int i = s;
    while (i < e) {
        const TGLShaderInst *in = &q->code[i];
        switch (in->op) {
        case TGL_SHADER_IF: {
            int endif = ref_match(q->code, q->n, i);
            int els = ref_else(q->code, q->n, i);
            int r = 0;
            if (q->R[lane][in->a][0] != 0.0f)
                r = ref_exec(q, lane, i + 1, els >= 0 ? els : endif);
            else if (els >= 0)
                r = ref_exec(q, lane, els + 1, endif);
            if (r)
                return r;
            i = endif + 1;
        } break;
        case TGL_SHADER_REPEAT: {
            int endr = ref_match(q->code, q->n, i);
            unsigned k;
            for (k = 0; k < in->count; k++) {
                int r = ref_exec(q, lane, i + 1, endr);
                if (r == 1)
                    break;
            }
            i = endr + 1;
        } break;
        case TGL_SHADER_BREAK_IF:
            if (q->R[lane][in->a][0] != 0.0f)
                return 1;
            i++;
            break;
        case TGL_SHADER_CONTINUE_IF:
            if (q->R[lane][in->a][0] != 0.0f)
                return 2;
            i++;
            break;
        default:
            ref_scalar(q, lane, in);
            i++;
        }
    }
    return 0;
}

/* per-lane reference: lanes run independently and only synchronize at
 * top-level DDX/DDY (which are the only quad-wide instructions) */
static void ref_quad(const TGLShaderInst *code, int n,
                     const float in[4][3][4], float out[4][4],
                     unsigned coverage, unsigned *wmask,
                     const unsigned char *tex)
{
    RefQ q;
    int i = 0, l, r, k;
    memset(&q, 0, sizeof q);
    q.code = code;
    q.n = n;
    q.tex = tex;
    for (l = 0; l < 4; l++)
        for (r = 0; r < 3; r++)
            for (k = 0; k < 4; k++)
                q.R[l][r][k] = in[l][r][k];
    while (i < n) {
        int op = code[i].op;
        if (op == TGL_SHADER_DDX || op == TGL_SHADER_DDY) {
            float res[4][4];
            int dst = code[i].dst, a = code[i].a;
            for (k = 0; k < 4; k++) {
                if (op == TGL_SHADER_DDX) {
                    float t = q.R[1][a][k] - q.R[0][a][k];
                    float b = q.R[3][a][k] - q.R[2][a][k];
                    res[0][k] = t;
                    res[1][k] = t;
                    res[2][k] = b;
                    res[3][k] = b;
                } else {
                    float le = q.R[2][a][k] - q.R[0][a][k];
                    float ri = q.R[3][a][k] - q.R[1][a][k];
                    res[0][k] = le;
                    res[2][k] = le;
                    res[1][k] = ri;
                    res[3][k] = ri;
                }
            }
            for (l = 0; l < 4; l++)
                for (k = 0; k < 4; k++)
                    q.R[l][dst][k] = res[l][k];
            i++;
        } else if (op == TGL_SHADER_IF || op == TGL_SHADER_REPEAT) {
            int m = ref_match(code, n, i);
            for (l = 0; l < 4; l++)
                ref_exec(&q, l, i, m + 1);
            i = m + 1;
        } else {
            for (l = 0; l < 4; l++)
                ref_exec(&q, l, i, i + 1);
            i++;
        }
    }
    for (l = 0; l < 4; l++)
        for (k = 0; k < 4; k++)
            out[l][k] = q.out[l][k];
    *wmask = (coverage & 0xFu) & ~q.disc;
}

static void test_sampler(void *ctx, int tx, int ty, float rgba[4])
{
    const unsigned char *p =
        (const unsigned char *) ctx + ((unsigned) ty * 256u + (unsigned) tx) * 4u;
    rgba[0] = p[0] / 255.0f;
    rgba[1] = p[1] / 255.0f;
    rgba[2] = p[2] / 255.0f;
    rgba[3] = p[3] / 255.0f;
}

/* run the VM and the per-lane reference on the same program/inputs and
 * compare outputs and write masks */
static int check_quad(const TGLShaderInst *code, unsigned n,
                      const float in[4][3][4], unsigned coverage,
                      const unsigned char *tex)
{
    TGLShaderError err;
    TGLShaderProgram *p;
    float out_vm[4][4], out_ref[4][4];
    unsigned wm_vm = 0xDEAD, wm_ref = 0xBEEF;
    int rc, l, k, fails = 0;
    memset(out_vm, 0, sizeof out_vm);
    memset(out_ref, 0, sizeof out_ref);
    p = tglShaderCreate(code, n, &err);
    fails += !task_check(p != NULL, "create ok", __FILE__, __LINE__);
    if (!p)
        return 1;
    rc = tglShaderExecQuad(p, in, out_vm, coverage, &wm_vm,
                           tex ? test_sampler : NULL, (void *) tex);
    fails += !task_check(rc == 0, "exec ok", __FILE__, __LINE__);
    ref_quad(code, (int) n, in, out_ref, coverage, &wm_ref, tex);
    fails += !task_check(wm_vm == wm_ref, "write mask", __FILE__, __LINE__);
    for (l = 0; l < 4; l++)
        for (k = 0; k < 4; k++)
            fails += !task_check(fabsf(out_vm[l][k] - out_ref[l][k]) <= 1e-5f,
                                 "lane output", __FILE__, __LINE__);
    tglShaderRelease(p);
    return fails;
}

static int c_nested_branch(void)
{
    TGLShaderInst c[16];
    float in[4][3][4];
    c[0] = mk(TGL_SHADER_IF, 0, 0, 0, 0);
    c[1] = mkc(3, 1, 0, 0, 1);
    c[2] = mk(TGL_SHADER_IF, 0, 1, 0, 0);
    c[3] = mkc(4, 0, 1, 0, 1);
    c[4] = mk(TGL_SHADER_ELSE, 0, 0, 0, 0);
    c[5] = mkc(4, 0, 0, 1, 1);
    c[6] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[7] = mk(TGL_SHADER_ADD, 3, 3, 4, 0);
    c[8] = mk(TGL_SHADER_ELSE, 0, 0, 0, 0);
    c[9] = mkc(3, 0, 0, 0, 1);
    c[10] = mk(TGL_SHADER_IF, 0, 2, 0, 0);
    c[11] = mkc(3, 1, 1, 0, 1);
    c[12] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[13] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
    c[14] = mk(TGL_SHADER_MOV, 5, 3, 0, 0);
    c[15] = mk(TGL_SHADER_MUL, 5, 5, 1, 0);
    {
        TGLShaderInst cc[17];
        memcpy(cc, c, sizeof c);
        cc[16] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
        memset(in, 0, sizeof in);
        /* r0.x: lanes 0,2 take the then side */
        in[0][0][0] = 2.0f;
        in[2][0][0] = 2.0f;
        /* r1: lane-varying colors and branch conditions */
        in[0][1][0] = 1.0f;
        in[0][1][1] = 0.5f;
        in[0][1][2] = 0.25f;
        in[0][1][3] = 1.0f;
        in[1][1][0] = 0.5f;
        in[1][1][1] = 1.0f;
        in[1][1][2] = 0.5f;
        in[1][1][3] = 1.0f;
        in[2][1][0] = 0.5f;
        in[2][1][1] = 0.5f;
        in[2][1][2] = 1.0f;
        in[2][1][3] = 1.0f;
        in[3][1][0] = 0.0f;
        in[3][1][1] = 1.0f;
        in[3][1][2] = 1.0f;
        in[3][1][3] = 1.0f;
        /* r2.x: lane 1 takes the inner then side */
        in[1][2][0] = 1.0f;
        in[3][2][0] = 0.0f;
        TASK_CHECK(check_quad(cc, ARRLEN(cc), in, 0xF, NULL) == 0);
    }
    return 0;
}

static int c_loop_exit_rounds(void)
{
    TGLShaderInst c[8];
    float in[4][3][4];
    c[0] = mkc(3, 0, 0, 0, 0);
    c[1] = mkc(4, 1, 1, 1, 1);
    c[2] = mkr(16);
    c[3] = mk(TGL_SHADER_ADD, 3, 3, 4, 0);
    c[4] = mk(TGL_SHADER_LT, 5, 0, 3, 0);
    c[5] = mk(TGL_SHADER_BREAK_IF, 0, 5, 0, 0);
    c[6] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[7] = mk(TGL_SHADER_MUL, 3, 3, 1, 0);
    {
        TGLShaderInst cc[8];
        memcpy(cc, c, sizeof c);
        memset(in, 0, sizeof in);
        /* each lane exits the loop in a different round */
        in[0][0][0] = 3.5f;
        in[1][0][0] = 7.5f;
        in[2][0][0] = 0.5f;
        in[3][0][0] = 12.5f;
        in[0][1][0] = 1.0f;
        in[1][1][1] = 1.0f;
        in[2][1][2] = 1.0f;
        in[3][1][3] = 1.0f;
        {
            TGLShaderInst c2[9];
            float out_vm[4][4];
            unsigned wm = 0;
            TGLShaderProgram *p;
            TGLShaderError err;
            memcpy(c2, cc, sizeof cc);
            c2[7] = mk(TGL_SHADER_MUL, 3, 3, 1, 0);
            c2[8] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
            p = tglShaderCreate(c2, 9, &err);
            TASK_CHECK(p != NULL);
            TASK_CHECK(tglShaderExecQuad(p, in, out_vm, 0xF, &wm, NULL,
                                         NULL) == 0);
            /* lanes broke after 4, 8, 1 and 13 increments */
            TASK_CHECK(fabsf(out_vm[0][0] - 4.0f) < 1e-5f);
            TASK_CHECK(fabsf(out_vm[1][1] - 8.0f) < 1e-5f);
            TASK_CHECK(fabsf(out_vm[2][2] - 1.0f) < 1e-5f);
            TASK_CHECK(fabsf(out_vm[3][3] - 13.0f) < 1e-5f);
            TASK_CHECK(wm == 0xF);
            tglShaderRelease(p);
            TASK_CHECK(check_quad(c2, 9, in, 0xF, NULL) == 0);
        }
    }
    return 0;
}

static int c_inner_continue_break(void)
{
    TGLShaderInst c[12];
    float in[4][3][4];
    c[0] = mkc(3, 0, 0, 0, 0);
    c[1] = mkc(4, 1, 1, 1, 1);
    c[2] = mkr(5);
    c[3] = mkr(16);
    c[4] = mk(TGL_SHADER_ADD, 3, 3, 4, 0);
    c[5] = mk(TGL_SHADER_LT, 5, 0, 3, 0);
    c[6] = mk(TGL_SHADER_CONTINUE_IF, 0, 1, 0, 0);
    c[7] = mk(TGL_SHADER_ADD, 3, 3, 4, 0);
    c[8] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[9] = mk(TGL_SHADER_BREAK_IF, 0, 5, 0, 0);
    c[10] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[11] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    memset(in, 0, sizeof in);
    in[0][0][0] = 10.5f; /* r0.x thresholds */
    in[1][0][0] = 3.5f;
    in[2][0][0] = 40.5f;
    in[3][0][0] = 0.5f;
    in[1][1][0] = 1.0f; /* r1.x: lanes 1,3 always continue */
    in[3][1][0] = 1.0f;
    {
        float out_vm[4][4];
        unsigned wm = 0;
        TGLShaderProgram *p;
        TGLShaderError err;
        p = tglShaderCreate(c, ARRLEN(c), &err);
        TASK_CHECK(p != NULL);
        TASK_CHECK(tglShaderExecQuad(p, in, out_vm, 0xF, &wm, NULL, NULL) == 0);
        /* lane0: 2 per inner round, breaks after one outer round -> 32
         * lane1: 1 per inner round (continue) -> 16
         * lane2: two outer rounds -> 64
         * lane3: 1 per inner round -> 16 */
        TASK_CHECK(fabsf(out_vm[0][0] - 32.0f) < 1e-4f);
        TASK_CHECK(fabsf(out_vm[1][0] - 16.0f) < 1e-4f);
        TASK_CHECK(fabsf(out_vm[2][0] - 64.0f) < 1e-4f);
        TASK_CHECK(fabsf(out_vm[3][0] - 16.0f) < 1e-4f);
        tglShaderRelease(p);
    }
    TASK_CHECK(check_quad(c, ARRLEN(c), in, 0xF, NULL) == 0);
    return 0;
}

static int c_discard_derivative(void)
{
    TGLShaderInst c[8];
    float in[4][3][4];
    float out_vm[4][4];
    unsigned wm = 0;
    TGLShaderProgram *p;
    TGLShaderError err;
    c[0] = mkc(3, 4, 4, 4, 4);
    c[1] = mk(TGL_SHADER_LT, 4, 0, 3, 0);
    c[2] = mk(TGL_SHADER_DISCARD_IF, 0, 4, 0, 0);
    c[3] = mk(TGL_SHADER_MOV, 5, 0, 0, 0);
    c[4] = mk(TGL_SHADER_DDX, 6, 5, 0, 0);
    c[5] = mk(TGL_SHADER_DDY, 7, 5, 0, 0);
    c[6] = mk(TGL_SHADER_ADD, 8, 6, 7, 0);
    c[7] = mk(TGL_SHADER_OUTPUT, 0, 8, 0, 0);
    memset(in, 0, sizeof in);
    /* r0 = (2*px, 3*py, 0, 1) per lane position */
    in[0][0][0] = 0.0f;
    in[0][0][1] = 0.0f;
    in[0][0][3] = 1.0f;
    in[1][0][0] = 2.0f;
    in[1][0][1] = 3.0f;
    in[1][0][3] = 1.0f;
    in[2][0][0] = 4.0f;
    in[2][0][1] = 6.0f;
    in[2][0][3] = 1.0f;
    in[3][0][0] = 6.0f;
    in[3][0][1] = 9.0f;
    in[3][0][3] = 1.0f;
    p = tglShaderCreate(c, ARRLEN(c), &err);
    TASK_CHECK(p != NULL);
    TASK_CHECK(tglShaderExecQuad(p, in, out_vm, 0xF, &wm, NULL, NULL) == 0);
    /* lanes 0,1 discard (r0.x < 4) but keep computing: lane 1's DDX
     * still uses discarded lane 0's registers */
    TASK_CHECK(wm == 0xC);
    TASK_CHECK(fabsf(out_vm[1][0] - 6.0f) < 1e-5f); /* ddx.x + ddy.x */
    TASK_CHECK(fabsf(out_vm[1][1] - 9.0f) < 1e-5f); /* ddx.y + ddy.y */
    TASK_CHECK(fabsf(out_vm[0][0] - 6.0f) < 1e-5f); /* discarded lane output */
    TASK_CHECK(fabsf(out_vm[3][0] - 6.0f) < 1e-5f);
    tglShaderRelease(p);
    TASK_CHECK(check_quad(c, ARRLEN(c), in, 0xF, NULL) == 0);
    return 0;
}

static int c_helper_lanes(void)
{
    TGLShaderInst c[4];
    float in[4][3][4];
    c[0] = mk(TGL_SHADER_DDX, 3, 0, 0, 0);
    c[1] = mk(TGL_SHADER_DDY, 4, 0, 0, 0);
    c[2] = mk(TGL_SHADER_ADD, 5, 3, 4, 0);
    c[3] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    memset(in, 0, sizeof in);
    /* only lane 0 is real; helpers extrapolate u = 10+2x+5y, v = 100+3x+7y */
    in[0][0][0] = 10.0f;
    in[0][0][1] = 100.0f;
    in[1][0][0] = 12.0f;
    in[1][0][1] = 103.0f;
    in[2][0][0] = 15.0f;
    in[2][0][1] = 107.0f;
    in[3][0][0] = 17.0f;
    in[3][0][1] = 110.0f;
    {
        float out_vm[4][4];
        unsigned wm = 0xF;
        TGLShaderProgram *p;
        TGLShaderError err;
        p = tglShaderCreate(c, ARRLEN(c), &err);
        TASK_CHECK(p != NULL);
        TASK_CHECK(tglShaderExecQuad(p, in, out_vm, 0x1, &wm, NULL, NULL) == 0);
        TASK_CHECK(wm == 0x1);
        /* derivatives come from the helper lanes too */
        TASK_CHECK(fabsf(out_vm[0][0] - 7.0f) < 1e-5f);
        TASK_CHECK(fabsf(out_vm[0][1] - 10.0f) < 1e-5f);
        tglShaderRelease(p);
    }
    TASK_CHECK(check_quad(c, ARRLEN(c), in, 0x1, NULL) == 0);
    return 0;
}

static int c_zero_coverage(void)
{
    TGLShaderInst c[6];
    float in[4][3][4];
    float out_vm[4][4];
    unsigned wm = 0xF;
    TGLShaderProgram *p;
    TGLShaderError err;
    c[0] = mkc(3, 1, 2, 3, 4);
    c[1] = mkr(3);
    c[2] = mk(TGL_SHADER_ADD, 3, 3, 3, 0);
    c[3] = mk(TGL_SHADER_ENDREPEAT, 0, 0, 0, 0);
    c[4] = mk(TGL_SHADER_MOV, 4, 3, 0, 0);
    c[5] = mk(TGL_SHADER_OUTPUT, 0, 4, 0, 0);
    memset(in, 0, sizeof in);
    p = tglShaderCreate(c, ARRLEN(c), &err);
    TASK_CHECK(p != NULL);
    /* zero coverage is valid, still executes, produces a zero write mask */
    TASK_CHECK(tglShaderExecQuad(p, in, out_vm, 0, &wm, NULL, NULL) == 0);
    TASK_CHECK(wm == 0);
    TASK_CHECK(fabsf(out_vm[0][0] - 8.0f) < 1e-5f);
    tglShaderRelease(p);
    return 0;
}

static int c_nonfinite(void)
{
    TGLShaderInst c[3];
    float in[4][3][4];
    float out_vm[4][4];
    unsigned wm;
    TGLShaderProgram *p;
    TGLShaderError err;
    int rc;

    memset(in, 0, sizeof in);
    c[0] = mkc(3, 1e30f, 1e30f, 1e30f, 1e30f);
    c[1] = mk(TGL_SHADER_MUL, 3, 3, 3, 0);
    c[2] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = tglShaderCreate(c, ARRLEN(c), &err);
    TASK_CHECK(p != NULL);
    out_vm[0][0] = 123.0f;
    wm = 0x7;
    rc = tglShaderExecQuad(p, in, out_vm, 0xF, &wm, NULL, NULL);
    TASK_CHECK(rc == TGL_SHADER_RT_NONFINITE);
    /* on failure the caller's outputs and mask are unchanged */
    TASK_CHECK(fabsf(out_vm[0][0] - 123.0f) < 1e-6f && wm == 0x7);
    tglShaderRelease(p);

    /* NaN texture coordinate is rejected, not converted to an integer */
    c[0] = mk(TGL_SHADER_TEX, 3, 0, 0, 0);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = tglShaderCreate(c, 2, &err);
    TASK_CHECK(p != NULL);
    in[0][0][0] = (float) NAN;
    in[0][0][1] = 0.5f;
    out_vm[0][0] = 55.0f;
    wm = 0x3;
    {
        static unsigned char dummy_tex[4];
        rc = tglShaderExecQuad(p, in, out_vm, 0xF, &wm, test_sampler,
                               (void *) dummy_tex);
    }
    TASK_CHECK(rc == TGL_SHADER_RT_NONFINITE);
    TASK_CHECK(fabsf(out_vm[0][0] - 55.0f) < 1e-6f && wm == 0x3);
    tglShaderRelease(p);
    return 0;
}

static int c_tex(void)
{
    static unsigned char tex8[256 * 256 * 4];
    TGLShaderInst c[2];
    float in[4][3][4];
    float out_vm[4][4];
    unsigned wm = 0;
    TGLShaderProgram *p;
    TGLShaderError err;
    int x, y;

    for (y = 0; y < 256; y++)
        for (x = 0; x < 256; x++) {
            tex8[(y * 256 + x) * 4 + 0] = (unsigned char) x;
            tex8[(y * 256 + x) * 4 + 1] = (unsigned char) y;
            tex8[(y * 256 + x) * 4 + 2] = (unsigned char) (x ^ y);
            tex8[(y * 256 + x) * 4 + 3] = 255;
        }

    c[0] = mk(TGL_SHADER_TEX, 3, 0, 0, 0);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = tglShaderCreate(c, 2, &err);
    TASK_CHECK(p != NULL);
    TASK_CHECK(tglShaderUsesTexture(p) == 1);
    memset(in, 0, sizeof in);
    in[0][0][0] = 0.5f;
    in[0][0][1] = 0.25f;
    in[1][0][0] = 1.5f;  /* repeats to 0.5 */
    in[1][0][1] = -0.25f; /* repeats to 0.75 */
    in[2][0][0] = -1.5f; /* repeats to 0.5 */
    in[2][0][1] = 2.5f;  /* repeats to 0.5 */
    in[3][0][0] = 0.999f;
    in[3][0][1] = 0.001f;
    TASK_CHECK(tglShaderExecQuad(p, in, out_vm, 0xF, &wm, test_sampler,
                                 (void *) tex8) == 0);
    TASK_CHECK(wm == 0xF);
    /* lane 0: texel (128, 64) */
    TASK_CHECK(fabsf(out_vm[0][0] - 128.0f / 255.0f) < 1e-6f);
    TASK_CHECK(fabsf(out_vm[0][1] - 64.0f / 255.0f) < 1e-6f);
    /* lane 1: texel (128, 192) */
    TASK_CHECK(fabsf(out_vm[1][0] - 128.0f / 255.0f) < 1e-6f);
    TASK_CHECK(fabsf(out_vm[1][1] - 192.0f / 255.0f) < 1e-6f);
    /* lane 2: texel (128, 128) */
    TASK_CHECK(fabsf(out_vm[2][0] - 128.0f / 255.0f) < 1e-6f);
    TASK_CHECK(fabsf(out_vm[2][1] - 128.0f / 255.0f) < 1e-6f);
    /* lane 3: texel (255, 0) */
    TASK_CHECK(fabsf(out_vm[3][0] - 255.0f / 255.0f) < 1e-6f);
    TASK_CHECK(fabsf(out_vm[3][1] - 0.0f) < 1e-6f);
    TASK_CHECK(fabsf(out_vm[3][3] - 1.0f) < 1e-6f);
    tglShaderRelease(p);
    TASK_CHECK(check_quad(c, 2, in, 0xF, tex8) == 0);
    return 0;
}

static int c_no_texture(void)
{
    TGLShaderInst c[3];
    float in[4][3][4];
    float out_vm[4][4];
    unsigned wm;
    TGLShaderProgram *p;
    TGLShaderError err;

    memset(in, 0, sizeof in);
    /* a TEX program requires a sampler in the independent context */
    c[0] = mk(TGL_SHADER_TEX, 3, 0, 0, 0);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = tglShaderCreate(c, 2, &err);
    TASK_CHECK(p != NULL);
    wm = 0x5;
    TASK_CHECK(tglShaderExecQuad(p, in, out_vm, 0xF, &wm, NULL, NULL) ==
               TGL_SHADER_RT_NO_TEXTURE);
    TASK_CHECK(wm == 0x5);
    tglShaderRelease(p);

    /* a program without TEX executes fine without a texture */
    c[0] = mkc(3, 0.5f, 0.25f, 1.0f, 1.0f);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = tglShaderCreate(c, 2, &err);
    TASK_CHECK(p != NULL);
    TASK_CHECK(tglShaderUsesTexture(p) == 0);
    wm = 0;
    TASK_CHECK(tglShaderExecQuad(p, in, out_vm, 0xF, &wm, NULL, NULL) == 0);
    TASK_CHECK(wm == 0xF);
    TASK_CHECK(fabsf(out_vm[2][0] - 0.5f) < 1e-6f);
    tglShaderRelease(p);
    return 0;
}

static int c_program_copy_lifetime(void)
{
    TGLShaderInst c[2];
    TGLShaderProgram *p;
    TGLShaderError err;
    float in[4][3][4];
    float out_vm[4][4];
    unsigned wm = 0;

    c[0] = mkc(3, 0.25f, 0.5f, 0.75f, 1.0f);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = tglShaderCreate(c, 2, &err);
    TASK_CHECK(p != NULL);
    /* the program owns a copy: mutating the source array changes nothing */
    c[0] = mkc(3, 9, 9, 9, 9);
    memset(in, 0, sizeof in);
    TASK_CHECK(tglShaderExecQuad(p, in, out_vm, 0xF, &wm, NULL, NULL) == 0);
    TASK_CHECK(fabsf(out_vm[0][0] - 0.25f) < 1e-6f);
    TASK_CHECK(fabsf(out_vm[0][2] - 0.75f) < 1e-6f);

    /* binding keeps its own reference */
    tglBindShader(p);
    TASK_CHECK(tglGetBoundShader() == p);
    tglShaderRelease(p); /* caller handle released */
    TASK_CHECK(tglGetBoundShader() == p);
    wm = 0;
    TASK_CHECK(tglShaderExecQuad(p, in, out_vm, 0xF, &wm, NULL, NULL) == 0);
    TASK_CHECK(wm == 0xF);
    tglBindShader(NULL);
    TASK_CHECK(tglGetBoundShader() == NULL);
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_NONE);
    return 0;
}

/* ------------------------------------------------------------------ */
/* render group: real draws through the GL                              */
/* ------------------------------------------------------------------ */

#define RW 64
#define RH 64

static ZBuffer *g_zb;

static void rsetup(void)
{
    g_zb = ZB_open(RW, RH, ZB_MODE_RGBA, NULL);
    glInit(g_zb);
    glViewport(0, 0, RW, RH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_TEXTURE);
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
}

static void rteardown(void)
{
    tglBindShader(NULL);
    glClose();
    ZB_close(g_zb);
    g_zb = NULL;
}

static int pix_r(int x, int y)
{
    return (int) GET_RED(g_zb->pbuf[y * RW + x]);
}

static int pix_g(int x, int y)
{
    return (int) GET_GREEN(g_zb->pbuf[y * RW + x]);
}

static int pix_b(int x, int y)
{
    return (int) GET_BLUE(g_zb->pbuf[y * RW + x]);
}

static int count_nonblack(void)
{
    int i, n = 0;
    for (i = 0; i < RW * RH; i++)
        if (g_zb->pbuf[i] & 0xffffffu)
            n++;
    return n;
}

/* replicate the viewport transform of the pipeline (64x64 viewport) */
static int vp_xform(float ndc)
{
    return (int) (ndc * 31.75f + 31.75f);
}

/* replicate the fixed-point window depth transform */
static int vp_zxform(float zndc)
{
    float scaleZ = -(float) ((1073741824.0 - 0.5) / 2.0);
    float transZ = (float) ((1073741824.0 - 0.5) / 2.0 + 8192.0);
    return (int) (zndc * scaleZ + transZ);
}

/* contract coverage rule: pixel centers, half-open (top-left) edges */
static int test_covered(const float x[3], const float y[3], int px, int py)
{
    float area, nx[3], ny[3];
    double cx = px + 0.5, cy = py + 0.5;
    int k;
    area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if (area == 0.0f)
        return 0;
    nx[0] = x[0];
    ny[0] = y[0];
    if (area < 0.0f) {
        nx[1] = x[2];
        ny[1] = y[2];
        nx[2] = x[1];
        ny[2] = y[1];
    } else {
        nx[1] = x[1];
        ny[1] = y[1];
        nx[2] = x[2];
        ny[2] = y[2];
    }
    for (k = 0; k < 3; k++) {
        int k2 = (k + 1) % 3;
        double dx = nx[k2] - nx[k], dy = ny[k2] - ny[k];
        double f = dx * (cy - ny[k]) - dy * (cx - nx[k]);
        int topleft = (dy < 0) || (dy == 0 && dx > 0);
        if (f < 0)
            return 0;
        if (f == 0 && !topleft)
            return 0;
    }
    return 1;
}

static void tri_plane(const float x[3], const float y[3], const float a[3],
                      double *dax, double *day)
{
    double area =
        (double) (x[1] - x[0]) * (y[2] - y[0]) - (double) (x[2] - x[0]) * (y[1] - y[0]);
    *dax = (a[0] * (y[1] - y[2]) + a[1] * (y[2] - y[0]) + a[2] * (y[0] - y[1])) /
           area;
    *day = (a[0] * (x[2] - x[1]) + a[1] * (x[0] - x[2]) + a[2] * (x[1] - x[0])) /
           area;
}

static double plane_at(const float x[3], const float y[3], const float a[3],
                       double dax, double day, double px, double py)
{
    return a[0] + dax * (px - x[0]) + day * (py - y[0]);
}

static TGLShaderProgram *make_prog(const TGLShaderInst *c, unsigned n)
{
    TGLShaderError err;
    TGLShaderProgram *p = tglShaderCreate(c, n, &err);
    return p;
}

static int r_gouraud(void)
{
    TGLShaderInst c[2];
    TGLShaderProgram *p;
    float sx[3], sy[3], sr[3], sg[3], sb[3];
    double dax, day;
    int px, py, covered = 0, fails = 0;

    c[0] = mk(TGL_SHADER_MOV, 3, 1, 0, 0);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = make_prog(c, 2);
    TASK_CHECK(p != NULL);

    rsetup();
    tglBindShader(p);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0);
    glVertex3f(-0.9f, -0.8f, 0.1f);
    glColor3f(0, 1, 0);
    glVertex3f(0.85f, -0.6f, 0.1f);
    glColor3f(0, 0, 1);
    glVertex3f(-0.2f, 0.9f, 0.1f);
    glEnd();
    tglBindShader(NULL);

    sx[0] = (float) vp_xform(-0.9f);
    sx[1] = (float) vp_xform(0.85f);
    sx[2] = (float) vp_xform(-0.2f);
    sy[0] = (float) vp_xform(0.8f);
    sy[1] = (float) vp_xform(0.6f);
    sy[2] = (float) vp_xform(-0.9f);
    sr[0] = 1;
    sr[1] = 0;
    sr[2] = 0;
    sg[0] = 0;
    sg[1] = 1;
    sg[2] = 0;
    sb[0] = 0;
    sb[1] = 0;
    sb[2] = 1;

    for (py = 0; py < RH; py++)
        for (px = 0; px < RW; px++) {
            if (!test_covered(sx, sy, px, py)) {
                fails += !task_check((g_zb->pbuf[py * RW + px] & 0xffffffu) == 0,
                                     "background stays black", __FILE__,
                                     __LINE__);
                continue;
            }
            covered++;
            {
                double cx = px + 0.5, cy = py + 0.5;
                double er, eg, eb;
                tri_plane(sx, sy, sr, &dax, &day);
                er = plane_at(sx, sy, sr, dax, day, cx, cy);
                tri_plane(sx, sy, sg, &dax, &day);
                eg = plane_at(sx, sy, sg, dax, day, cx, cy);
                tri_plane(sx, sy, sb, &dax, &day);
                eb = plane_at(sx, sy, sb, dax, day, cx, cy);
                fails += !task_check(abs(pix_r(px, py) -
                                         (int) (er * 255.0 + 0.5)) <= 2,
                                     "red channel", __FILE__, __LINE__);
                fails += !task_check(abs(pix_g(px, py) -
                                         (int) (eg * 255.0 + 0.5)) <= 2,
                                     "green channel", __FILE__, __LINE__);
                fails += !task_check(abs(pix_b(px, py) -
                                         (int) (eb * 255.0 + 0.5)) <= 2,
                                     "blue channel", __FILE__, __LINE__);
            }
        }
    fails += !task_check(covered > 1000, "triangle covers pixels", __FILE__,
                         __LINE__);
    fails += !task_check(tglShaderGetError() == TGL_SHADER_RT_NONE,
                         "no runtime error", __FILE__, __LINE__);
    rteardown();
    tglShaderRelease(p);
    TASK_CHECK(fails == 0);
    return 0;
}

/* Sutherland-Hodgman clip against the 6 NDC planes, replicating the
 * pipeline's float intersection math */
typedef struct PVert {
    float v[4];
} PVert;

static float pvert_get(PVert a, int i)
{
    return a.v[i];
}

static PVert clip_edge(PVert a, PVert b, int dir, float sign)
{
    float d[4], av[4], bv[4], cv[4], den, t;
    int i, d1, d2;
    PVert c;
    for (i = 0; i < 4; i++) {
        av[i] = a.v[i];
        bv[i] = b.v[i];
        d[i] = bv[i] - av[i];
    }
    den = -(sign * d[dir]) + d[3];
    if (den == 0.0f)
        t = 0.0f;
    else
        t = (sign * av[dir] - av[3]) / den;
    d1 = (dir == 0) ? 1 : 0;
    d2 = (dir == 2) ? 1 : 2;
    cv[d1] = av[d1] + t * d[d1];
    cv[d2] = av[d2] + t * d[d2];
    cv[3] = av[3] + t * d[3];
    cv[dir] = sign * cv[3];
    for (i = 0; i < 4; i++)
        c.v[i] = cv[i];
    return c;
}

static int sh_clip(PVert *poly, int n, int dir, float sign)
{
    PVert out[16];
    int m = 0, i;
    for (i = 0; i < n; i++) {
        PVert a = poly[i];
        PVert b = poly[(i + 1) % n];
        float wa = a.v[3] * (1.0f + 1e-5f);
        float wb = b.v[3] * (1.0f + 1e-5f);
        int ain = sign * pvert_get(a, dir) <= wa;
        int bin = sign * pvert_get(b, dir) <= wb;
        if (ain && bin) {
            out[m++] = b;
        } else if (ain && !bin) {
            out[m++] = clip_edge(a, b, dir, sign);
        } else if (!ain && bin) {
            out[m++] = clip_edge(a, b, dir, sign);
            out[m++] = b;
        }
    }
    for (i = 0; i < m; i++)
        poly[i] = out[i];
    return m;
}

static int r_clipped(void)
{
    TGLShaderInst c[2];
    TGLShaderProgram *p;
    PVert poly[16];
    int n = 3, i, px, py, covered = 0, fails = 0;
    static const int dirs[6] = {0, 0, 1, 1, 2, 2};
    static const float signs[6] = {-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f};
    float sx[9], sy[9];
    int exp_r, exp_g, exp_b;

    c[0] = mkc(3, 0.2f, 0.6f, 0.9f, 1.0f);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = make_prog(c, 2);
    TASK_CHECK(p != NULL);

    rsetup();
    tglBindShader(p);
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.5f, -1.0f, 0.2f);
    glVertex3f(1.2f, -0.8f, 0.2f);
    glVertex3f(0.1f, 1.5f, 0.2f);
    glEnd();
    tglBindShader(NULL);

    /* independently clip and rasterize the same triangle */
    poly[0].v[0] = -1.5f;
    poly[0].v[1] = -1.0f;
    poly[0].v[2] = 0.2f;
    poly[0].v[3] = 1.0f;
    poly[1].v[0] = 1.2f;
    poly[1].v[1] = -0.8f;
    poly[1].v[2] = 0.2f;
    poly[1].v[3] = 1.0f;
    poly[2].v[0] = 0.1f;
    poly[2].v[1] = 1.5f;
    poly[2].v[2] = 0.2f;
    poly[2].v[3] = 1.0f;
    for (i = 0; i < 6; i++)
        n = sh_clip(poly, n, dirs[i], signs[i]);
    TASK_CHECK(n >= 3);
    for (i = 0; i < n; i++) {
        float winv = 1.0f / poly[i].v[3];
        sx[i] = (float) (int) (poly[i].v[0] * winv * 31.75f + 31.75f);
        sy[i] = (float) (int) (poly[i].v[1] * winv * -31.75f + 31.75f);
    }

    exp_r = (int) (0.2f * 255.0f + 0.5f);
    exp_g = (int) (0.6f * 255.0f + 0.5f);
    exp_b = (int) (0.9f * 255.0f + 0.5f);
    for (py = 0; py < RH; py++)
        for (px = 0; px < RW; px++) {
            int cov = 0, k;
            for (k = 1; k + 1 < n; k++) {
                float fx[3], fy[3];
                fx[0] = sx[0];
                fy[0] = sy[0];
                fx[1] = sx[k];
                fy[1] = sy[k];
                fx[2] = sx[k + 1];
                fy[2] = sy[k + 1];
                if (test_covered(fx, fy, px, py))
                    cov = 1;
            }
            if (cov) {
                covered++;
                fails += !task_check(pix_r(px, py) == exp_r &&
                                         pix_g(px, py) == exp_g &&
                                         pix_b(px, py) == exp_b,
                                     "clipped pixel color", __FILE__, __LINE__);
            } else {
                fails += !task_check((g_zb->pbuf[py * RW + px] & 0xffffffu) == 0,
                                     "clipped-away pixel stays black",
                                     __FILE__, __LINE__);
            }
        }
    fails += !task_check(covered > 1500, "clipped triangle covers pixels",
                         __FILE__, __LINE__);
    fails += !task_check(tglShaderGetError() == TGL_SHADER_RT_NONE,
                         "no runtime error", __FILE__, __LINE__);
    rteardown();
    tglShaderRelease(p);
    TASK_CHECK(fails == 0);
    return 0;
}

static int r_perspective_uv(void)
{
    static unsigned char texdata[256 * 256 * 3];
    TGLShaderInst c[2];
    TGLShaderProgram *p;
    GLuint tex = 0;
    int x, y, px, py, fails = 0, covered = 0, divergent = 0, divergent_ok = 0,
        match = 0;
    float M[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -0.5f, -1, 0, 0, -0.5f, 0};
    float vx[4] = {-0.9f, 1.5f, 1.5f, -0.9f};
    float vy[4] = {-0.6f, -0.6f, 1.2f, 1.2f};
    float vz[4] = {-2.0f, -5.0f, -5.0f, -2.0f};
    float vu[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    float vv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float pw[4], pz[4], iw[4], uw[4], vw[4];
    float sx1[3], sy1[3], sx2[3], sy2[3];
    int i;

    for (y = 0; y < 256; y++)
        for (x = 0; x < 256; x++) {
            texdata[(y * 256 + x) * 3 + 0] = (unsigned char) x;
            texdata[(y * 256 + x) * 3 + 1] = (unsigned char) y;
            texdata[(y * 256 + x) * 3 + 2] = 0;
        }

    c[0] = mk(TGL_SHADER_TEX, 3, 0, 0, 0);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = make_prog(c, 2);
    TASK_CHECK(p != NULL);

    rsetup();
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(M);
    glMatrixMode(GL_MODELVIEW);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 texdata);
    glEnable(GL_TEXTURE_2D);
    tglBindShader(p);
    glBegin(GL_TRIANGLES);
    glTexCoord2f(vu[0], vv[0]);
    glVertex3f(vx[0], vy[0], vz[0]);
    glTexCoord2f(vu[1], vv[1]);
    glVertex3f(vx[1], vy[1], vz[1]);
    glTexCoord2f(vu[2], vv[2]);
    glVertex3f(vx[2], vy[2], vz[2]);
    glTexCoord2f(vu[0], vv[0]);
    glVertex3f(vx[0], vy[0], vz[0]);
    glTexCoord2f(vu[2], vv[2]);
    glVertex3f(vx[2], vy[2], vz[2]);
    glTexCoord2f(vu[3], vv[3]);
    glVertex3f(vx[3], vy[3], vz[3]);
    glEnd();
    tglBindShader(NULL);

    /* replicate the vertex transform: w = -z, z' = -0.5z - 0.5 */
    for (i = 0; i < 4; i++) {
        pw[i] = -vz[i];
        pz[i] = -0.5f * vz[i] - 0.5f;
        iw[i] = 1.0f / pw[i];
        uw[i] = vu[i] * iw[i];
        vw[i] = vv[i] * iw[i];
        (void) pz;
    }
    for (i = 0; i < 3; i++) {
        float winv = 1.0f / pw[i];
        sx1[i] = (float) (int) (vx[i] * winv * 31.75f + 31.75f);
        sy1[i] = (float) (int) (vy[i] * winv * -31.75f + 31.75f);
    }
    sx2[0] = sx1[0];
    sy2[0] = sy1[0];
    sx2[1] = sx1[2];
    sy2[1] = sy1[2];
    {
        float winv = 1.0f / pw[3];
        sx2[2] = (float) (int) (vx[3] * winv * 31.75f + 31.75f);
        sy2[2] = (float) (int) (vy[3] * winv * -31.75f + 31.75f);
    }

    /* check every covered pixel against the perspective model and
     * against the (wrong) linear model */
    for (py = 0; py < RH; py++)
        for (px = 0; px < RW; px++) {
            int cov = test_covered(sx1, sy1, px, py) ||
                      test_covered(sx2, sy2, px, py);
            if (!cov)
                continue;
            {
                /* evaluate both triangles' models; use the covering one */
                const float *tx, *ty;
                int tri, found = 0;
                double lu = 0, lv = 0;
                for (tri = 0; tri < 2 && !found; tri++) {
                    const float *gx = tri ? sx2 : sx1;
                    const float *gy = tri ? sy2 : sy1;
                    int idx[3];
                    float fiw[3], fuw[3], fvw[3], fu[3], fv[3];
                    double d1, d2, d3, d4;
                    if (!test_covered(gx, gy, px, py))
                        continue;
                    found = 1;
                    tx = gx;
                    ty = gy;
                    idx[0] = 0;
                    idx[1] = tri ? 2 : 1;
                    idx[2] = tri ? 3 : 2;
                    for (i = 0; i < 3; i++) {
                        fiw[i] = iw[idx[i]];
                        fuw[i] = uw[idx[i]];
                        fvw[i] = vw[idx[i]];
                        fu[i] = vu[idx[i]];
                        fv[i] = vv[idx[i]];
                    }
                    tri_plane(tx, ty, fiw, &d1, &d2);
                    tri_plane(tx, ty, fuw, &d3, &d4);
                    {
                        double cx = px + 0.5, cy = py + 0.5;
                        double w = plane_at(tx, ty, fiw, d1, d2, cx, cy);
                        double u, v, du, dv, ddx1, ddy1, ddx2, ddy2;
                        int etx, ety, ltx, lty;
                        u = plane_at(tx, ty, fuw, d3, d4, cx, cy) / w;
                        tri_plane(tx, ty, fvw, &d3, &d4);
                        v = plane_at(tx, ty, fvw, d3, d4, cx, cy) / w;
                        tri_plane(tx, ty, fu, &ddx1, &ddy1);
                        tri_plane(tx, ty, fv, &ddx2, &ddy2);
                        lu = plane_at(tx, ty, fu, ddx1, ddy1, cx, cy);
                        lv = plane_at(tx, ty, fv, ddx2, ddy2, cx, cy);
                        du = u - floor(u);
                        dv = v - floor(v);
                        etx = (int) floor(du * 256.0);
                        ety = (int) floor(dv * 256.0);
                        du = lu - floor(lu);
                        dv = lv - floor(lv);
                        ltx = (int) floor(du * 256.0);
                        lty = (int) floor(dv * 256.0);
                        covered++;
                        {
                            int dr = abs(pix_r(px, py) - etx);
                            int dg = abs(pix_g(px, py) - ety);
                            if (dr > 255)
                                dr = 255;
                            if (dg > 255)
                                dg = 255;
                            if (dr <= 1 && dg <= 1)
                                match++;
                            if (abs(etx - ltx) >= 2 || abs(ety - lty) >= 2) {
                                divergent++;
                                if (dr <= 1 && dg <= 1)
                                    divergent_ok++;
                            }
                        }
                    }
                }
            }
        }
    fails += !task_check(covered > 400, "quad covers pixels", __FILE__,
                         __LINE__);
    fails += !task_check(match * 10 >= covered * 9,
                         "pixels match perspective model", __FILE__, __LINE__);
    fails += !task_check(divergent >= 40,
                         "perspective and linear models diverge", __FILE__,
                         __LINE__);
    fails += !task_check(divergent_ok * 10 >= divergent * 9,
                         "divergent pixels follow perspective", __FILE__,
                         __LINE__);
    fails += !task_check(tglShaderGetError() == TGL_SHADER_RT_NONE,
                         "no runtime error", __FILE__, __LINE__);
    rteardown();
    tglShaderRelease(p);
    TASK_CHECK(fails == 0);
    return 0;
}

static int r_derivative(void)
{
    static unsigned char texdata[256 * 256 * 3];
    TGLShaderInst c[7];
    TGLShaderProgram *p;
    GLuint tex = 0;
    int x, y, px, py, fails = 0, checked = 0;
    float M[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -0.5f, -1, 0, 0, -0.5f, 0};
    float vx[4] = {-0.9f, 1.5f, 1.5f, -0.9f};
    float vy[4] = {-0.6f, -0.6f, 1.2f, 1.2f};
    float vz[4] = {-2.0f, -5.0f, -5.0f, -2.0f};
    float vu[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    float vv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float pw[4], iw[4], uw[4], vw[4];
    float sx1[3], sy1[3], sx2[3], sy2[3];
    int i;

    for (y = 0; y < 256; y++)
        for (x = 0; x < 256; x++) {
            texdata[(y * 256 + x) * 3 + 0] = (unsigned char) x;
            texdata[(y * 256 + x) * 3 + 1] = (unsigned char) y;
            texdata[(y * 256 + x) * 3 + 2] = 0;
        }

    /* OUTPUT = 8 * (DDX r0 + DDY r0) */
    c[0] = mk(TGL_SHADER_DDX, 3, 0, 0, 0);
    c[1] = mk(TGL_SHADER_DDY, 4, 0, 0, 0);
    c[2] = mk(TGL_SHADER_ADD, 5, 3, 4, 0);
    c[3] = mkc(6, 8, 8, 8, 8);
    c[4] = mk(TGL_SHADER_MUL, 5, 5, 6, 0);
    c[5] = mk(TGL_SHADER_MOV, 7, 5, 0, 0);
    c[6] = mk(TGL_SHADER_OUTPUT, 0, 7, 0, 0);
    p = make_prog(c, ARRLEN(c));
    TASK_CHECK(p != NULL);

    rsetup();
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(M);
    glMatrixMode(GL_MODELVIEW);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 texdata);
    glEnable(GL_TEXTURE_2D);
    tglBindShader(p);
    glBegin(GL_TRIANGLES);
    glTexCoord2f(vu[0], vv[0]);
    glVertex3f(vx[0], vy[0], vz[0]);
    glTexCoord2f(vu[1], vv[1]);
    glVertex3f(vx[1], vy[1], vz[1]);
    glTexCoord2f(vu[2], vv[2]);
    glVertex3f(vx[2], vy[2], vz[2]);
    glTexCoord2f(vu[0], vv[0]);
    glVertex3f(vx[0], vy[0], vz[0]);
    glTexCoord2f(vu[2], vv[2]);
    glVertex3f(vx[2], vy[2], vz[2]);
    glTexCoord2f(vu[3], vv[3]);
    glVertex3f(vx[3], vy[3], vz[3]);
    glEnd();
    tglBindShader(NULL);

    for (i = 0; i < 4; i++) {
        pw[i] = -vz[i];
        iw[i] = 1.0f / pw[i];
        uw[i] = vu[i] * iw[i];
        vw[i] = vv[i] * iw[i];
    }
    for (i = 0; i < 3; i++) {
        float winv = 1.0f / pw[i];
        sx1[i] = (float) (int) (vx[i] * winv * 31.75f + 31.75f);
        sy1[i] = (float) (int) (vy[i] * winv * -31.75f + 31.75f);
    }
    sx2[0] = sx1[0];
    sy2[0] = sy1[0];
    sx2[1] = sx1[2];
    sy2[1] = sy1[2];
    {
        float winv = 1.0f / pw[3];
        sx2[2] = (float) (int) (vx[3] * winv * 31.75f + 31.75f);
        sy2[2] = (float) (int) (vy[3] * winv * -31.75f + 31.75f);
    }

    for (py = 0; py < RH; py++)
        for (px = 0; px < RW; px++) {
            int tri, found = 0;
            for (tri = 0; tri < 2 && !found; tri++) {
                const float *gx = tri ? sx2 : sx1;
                const float *gy = tri ? sy2 : sy1;
                int idx[3];
                float fiw[3], fuw[3], fvw[3];
                double diwx, diwy, duwx, duwy, dvwx, dvwy;
                if (!test_covered(gx, gy, px, py))
                    continue;
                found = 1;
                idx[0] = 0;
                idx[1] = tri ? 2 : 1;
                idx[2] = tri ? 3 : 2;
                for (i = 0; i < 3; i++) {
                    fiw[i] = iw[idx[i]];
                    fuw[i] = uw[idx[i]];
                    fvw[i] = vw[idx[i]];
                }
                tri_plane(gx, gy, fiw, &diwx, &diwy);
                tri_plane(gx, gy, fuw, &duwx, &duwy);
                tri_plane(gx, gy, fvw, &dvwx, &dvwy);
                {
                    /* quad-aligned lane positions */
                    int qx = px & ~1, qy = py & ~1;
                    double u00, u10, u01, w00, w10, w01;
                    double cx0 = qx + 0.5, cx1 = qx + 1.5;
                    double cy0 = qy + 0.5, cy1 = qy + 1.5;
                    double cyr = py + 0.5, cxr = px + 0.5;
                    double ddx_u, ddy_u, ddx_v, ddy_v;
                    int er, eg;
                    w00 = plane_at(gx, gy, fiw, diwx, diwy, cx0, cyr);
                    w10 = plane_at(gx, gy, fiw, diwx, diwy, cx1, cyr);
                    w01 = plane_at(gx, gy, fiw, diwx, diwy, cxr, cy1);
                    u00 = plane_at(gx, gy, fuw, duwx, duwy, cx0, cyr) / w00;
                    u10 = plane_at(gx, gy, fuw, duwx, duwy, cx1, cyr) / w10;
                    u01 = plane_at(gx, gy, fuw, duwx, duwy, cxr, cy1) / w01;
                    ddx_u = u10 - u00;
                    ddy_u = u01 -
                            plane_at(gx, gy, fuw, duwx, duwy, cxr, cy0) /
                                plane_at(gx, gy, fiw, diwx, diwy, cxr, cy0);
                    w00 = plane_at(gx, gy, fiw, diwx, diwy, cx0, cyr);
                    ddx_v =
                        plane_at(gx, gy, fvw, dvwx, dvwy, cx1, cyr) / w10 -
                        plane_at(gx, gy, fvw, dvwx, dvwy, cx0, cyr) / w00;
                    ddy_v =
                        plane_at(gx, gy, fvw, dvwx, dvwy, cxr, cy1) / w01 -
                        plane_at(gx, gy, fvw, dvwx, dvwy, cxr, cy0) /
                            plane_at(gx, gy, fiw, diwx, diwy, cxr, cy0);
                    er = (int) ((ddx_u + ddy_u) * 8.0 * 255.0 + 0.5);
                    eg = (int) ((ddx_v + ddy_v) * 8.0 * 255.0 + 0.5);
                    /* the OUTPUT is clamped to [0,1] at byte conversion */
                    if (er < 0)
                        er = 0;
                    if (er > 255)
                        er = 255;
                    if (eg < 0)
                        eg = 0;
                    if (eg > 255)
                        eg = 255;
                    fails += !task_check(abs(pix_r(px, py) - er) <= 3,
                                         "ddx/ddy u channel", __FILE__,
                                         __LINE__);
                    fails += !task_check(abs(pix_g(px, py) - eg) <= 3,
                                         "ddx/ddy v channel", __FILE__,
                                         __LINE__);
                    checked++;
                }
            }
        }
    fails += !task_check(checked > 400, "derivative pixels checked", __FILE__,
                         __LINE__);
    fails += !task_check(tglShaderGetError() == TGL_SHADER_RT_NONE,
                         "no runtime error", __FILE__, __LINE__);
    rteardown();
    tglShaderRelease(p);
    TASK_CHECK(fails == 0);
    return 0;
}

static int r_depth_occlusion(void)
{
    TGLShaderInst cr[2], cb[2];
    TGLShaderProgram *pred, *pblue;
    int expected_zz;

    cr[0] = mkc(3, 1, 0, 0, 1);
    cr[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    cb[0] = mkc(3, 0, 0, 1, 1);
    cb[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    pred = make_prog(cr, 2);
    pblue = make_prog(cb, 2);
    TASK_CHECK(pred != NULL && pblue != NULL);

    /* shader over shader: the nearer triangle wins */
    rsetup();
    tglBindShader(pred);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.5f);
    glVertex3f(0.5f, -0.5f, 0.5f);
    glVertex3f(0.0f, 0.5f, 0.5f);
    glEnd();
    tglBindShader(pblue);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, -0.5f);
    glVertex3f(0.5f, -0.5f, -0.5f);
    glVertex3f(0.0f, 0.5f, -0.5f);
    glEnd();
    tglBindShader(NULL);
    TASK_CHECK(pix_b(32, 32) > 250 && pix_r(32, 32) < 5);
    expected_zz = (int) ((unsigned) vp_zxform(-0.5f) >> 14);
    TASK_CHECK(abs((int) g_zb->zbuf[32 * RW + 32] - expected_zz) <= 1);
    rteardown();

    /* fixed pipeline over shader: nearer fixed triangle wins */
    rsetup();
    tglBindShader(pred);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.5f);
    glVertex3f(0.5f, -0.5f, 0.5f);
    glVertex3f(0.0f, 0.5f, 0.5f);
    glEnd();
    tglBindShader(NULL);
    glColor3f(0, 1, 0);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, -0.5f);
    glVertex3f(0.5f, -0.5f, -0.5f);
    glVertex3f(0.0f, 0.5f, -0.5f);
    glEnd();
    TASK_CHECK(pix_g(32, 32) > 250 && pix_r(32, 32) < 5);
    rteardown();

    /* shader over fixed pipeline: nearer shader triangle wins */
    rsetup();
    glColor3f(0, 1, 0);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.5f);
    glVertex3f(0.5f, -0.5f, 0.5f);
    glVertex3f(0.0f, 0.5f, 0.5f);
    glEnd();
    tglBindShader(pblue);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, -0.5f);
    glVertex3f(0.5f, -0.5f, -0.5f);
    glVertex3f(0.0f, 0.5f, -0.5f);
    glEnd();
    tglBindShader(NULL);
    TASK_CHECK(pix_b(32, 32) > 250 && pix_g(32, 32) < 5);
    rteardown();

    tglShaderRelease(pred);
    tglShaderRelease(pblue);
    return 0;
}

static float ndc_for_pix(float k)
{
    return (k + 0.25f - 31.75f) / 31.75f;
}

/* the viewport y scale is negative: screen y = 31.75 - 31.75 * y_ndc */
static float ndc_for_pix_y(float k)
{
    return -ndc_for_pix(k);
}

static int r_single_lane(void)
{
    TGLShaderInst c[2];
    TGLShaderProgram *p;
    float x0, x1, y0, y1;

    /* OUTPUT = DDX r2: with extrapolated helpers the x derivative of the
     * pixel-center coordinate is exactly 1 even for a one-lane quad */
    c[0] = mk(TGL_SHADER_DDX, 3, 2, 0, 0);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = make_prog(c, 2);
    TASK_CHECK(p != NULL);

    x0 = ndc_for_pix(10.0f);
    x1 = ndc_for_pix(12.0f);
    y0 = ndc_for_pix_y(10.0f);
    y1 = ndc_for_pix_y(12.0f);

    rsetup();
    tglBindShader(p);
    glBegin(GL_TRIANGLES);
    glVertex3f(x0, y0, 0.0f);
    glVertex3f(x1, y0, 0.0f);
    glVertex3f(x0, y1, 0.0f);
    glEnd();
    tglBindShader(NULL);

    /* exactly one real lane at pixel (10,10) */
    TASK_CHECK(pix_r(10, 10) >= 250);
    TASK_CHECK(pix_g(10, 10) <= 5 && pix_b(10, 10) <= 5);
    TASK_CHECK((g_zb->pbuf[10 * RW + 11] & 0xffffffu) == 0);
    TASK_CHECK((g_zb->pbuf[11 * RW + 10] & 0xffffffu) == 0);
    TASK_CHECK((g_zb->pbuf[11 * RW + 11] & 0xffffffu) == 0);
    TASK_CHECK(g_zb->zbuf[10 * RW + 10] > 0);
    TASK_CHECK(g_zb->zbuf[10 * RW + 11] == 0);
    TASK_CHECK(g_zb->zbuf[11 * RW + 10] == 0);
    TASK_CHECK(count_nonblack() == 1);
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_NONE);
    rteardown();
    tglShaderRelease(p);
    return 0;
}

static int r_discard_depth(void)
{
    TGLShaderInst c1[5], c2[2];
    TGLShaderProgram *p1, *p2;
    int near_zz, far_zz;

    /* discard the left half of the screen, output red elsewhere */
    c1[0] = mkc(3, 32, 0, 0, 0);
    c1[1] = mk(TGL_SHADER_LT, 4, 2, 3, 0);
    c1[2] = mk(TGL_SHADER_DISCARD_IF, 0, 4, 0, 0);
    c1[3] = mkc(5, 1, 0, 0, 1);
    c1[4] = mk(TGL_SHADER_OUTPUT, 0, 5, 0, 0);
    c2[0] = mkc(3, 0, 1, 0, 1);
    c2[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p1 = make_prog(c1, ARRLEN(c1));
    p2 = make_prog(c2, ARRLEN(c2));
    TASK_CHECK(p1 != NULL && p2 != NULL);

    rsetup();
    /* near red triangle with the discard pattern */
    tglBindShader(p1);
    glBegin(GL_TRIANGLES);
    glVertex3f(-2.0f, -2.0f, -0.5f);
    glVertex3f(2.0f, -2.0f, -0.5f);
    glVertex3f(0.0f, 2.0f, -0.5f);
    glEnd();
    /* far green triangle behind it */
    tglBindShader(p2);
    glBegin(GL_TRIANGLES);
    glVertex3f(-2.0f, -2.0f, 0.5f);
    glVertex3f(2.0f, -2.0f, 0.5f);
    glVertex3f(0.0f, 2.0f, 0.5f);
    glEnd();
    tglBindShader(NULL);

    /* discarded lanes wrote neither color nor depth: green shows through */
    TASK_CHECK(pix_g(16, 32) > 250 && pix_r(16, 32) < 5);
    /* kept lanes are red and occlude the far triangle */
    TASK_CHECK(pix_r(48, 32) > 250 && pix_g(48, 32) < 5);
    near_zz = (int) ((unsigned) vp_zxform(-0.5f) >> 14);
    far_zz = (int) ((unsigned) vp_zxform(0.5f) >> 14);
    TASK_CHECK(abs((int) g_zb->zbuf[32 * RW + 48] - near_zz) <= 1);
    TASK_CHECK(abs((int) g_zb->zbuf[32 * RW + 16] - far_zz) <= 1);
    rteardown();
    tglShaderRelease(p1);
    tglShaderRelease(p2);
    return 0;
}

static int r_unbind_fixed(void)
{
    TGLShaderInst c[2];
    TGLShaderProgram *p;
    PIXEL *ref;

    c[0] = mkc(3, 0, 0, 1, 1);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = make_prog(c, 2);
    TASK_CHECK(p != NULL);

    /* draw with the program, unbind, clear, redraw with the fixed pipeline */
    rsetup();
    tglBindShader(p);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    TASK_CHECK(pix_b(32, 32) > 250);
    tglBindShader(NULL);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(1, 0, 0);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    ref = (PIXEL *) malloc(sizeof(PIXEL) * RW * RH);
    TASK_CHECK(ref != NULL);
    memcpy(ref, g_zb->pbuf, sizeof(PIXEL) * RW * RH);
    rteardown();

    /* a fresh context drawing only the fixed triangle must match exactly */
    rsetup();
    glColor3f(1, 0, 0);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    TASK_CHECK(memcmp(ref, g_zb->pbuf, sizeof(PIXEL) * RW * RH) == 0);
    TASK_CHECK(count_nonblack() > 500);
    rteardown();
    free(ref);
    tglShaderRelease(p);
    return 0;
}

static int r_display_list(void)
{
    TGLShaderInst c[2];
    TGLShaderProgram *p;
    GLuint list;

    c[0] = mkc(3, 0, 1, 0, 1);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = make_prog(c, 2);
    TASK_CHECK(p != NULL);

    rsetup();
    /* compile a triangle while bound: the binding is not recorded */
    tglBindShader(p);
    list = glGenLists(1);
    TASK_CHECK(glIsList(list));
    glNewList(list, GL_COMPILE);
    glColor3f(1, 0, 0);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    glEndList();
    /* replay unbound: fixed pipeline output */
    tglBindShader(NULL);
    glCallList(list);
    TASK_CHECK(pix_r(32, 32) > 250 && pix_g(32, 32) < 5);
    /* replay bound: the program bound at replay time is used */
    tglBindShader(p);
    glCallList(list);
    TASK_CHECK(pix_g(32, 32) > 250 && pix_r(32, 32) < 5);
    tglBindShader(NULL);
    glDeleteList(list);
    rteardown();
    tglShaderRelease(p);
    return 0;
}

static int r_state_reject(void)
{
    TGLShaderInst c[2], ct[2];
    TGLShaderProgram *p, *pt;
    static unsigned char texdata[256 * 256 * 3];
    GLuint tex = 0;

    c[0] = mkc(3, 1, 0, 0, 1);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    ct[0] = mk(TGL_SHADER_TEX, 3, 0, 0, 0);
    ct[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = make_prog(c, 2);
    pt = make_prog(ct, 2);
    TASK_CHECK(p != NULL && pt != NULL);

    rsetup();
    tglBindShader(p);

    /* GL_FLAT is rejected */
    glShadeModel(GL_FLAT);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_STATE);
    TASK_CHECK(count_nonblack() == 0);
    glShadeModel(GL_SMOOTH);

    /* blending is rejected */
    glEnable(GL_BLEND);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_STATE);
    TASK_CHECK(count_nonblack() == 0);
    glDisable(GL_BLEND);

    /* line polygon mode is rejected */
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_STATE);
    TASK_CHECK(count_nonblack() == 0);
    /* point polygon mode is rejected */
    glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_STATE);
    TASK_CHECK(count_nonblack() == 0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* a TEX program without an enabled texture is rejected */
    tglBindShader(pt);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_NO_TEXTURE);
    TASK_CHECK(count_nonblack() == 0);

    /* with a texture bound and enabled it draws */
    memset(texdata, 200, sizeof texdata);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 texdata);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_NONE);
    TASK_CHECK(count_nonblack() > 500);

    tglBindShader(NULL);
    rteardown();
    tglShaderRelease(p);
    tglShaderRelease(pt);
    return 0;
}

static int r_lifetime(void)
{
    TGLShaderInst c[2];
    TGLShaderProgram *p;

    c[0] = mkc(3, 1, 1, 0, 1);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = make_prog(c, 2);
    TASK_CHECK(p != NULL);

    rsetup();
    tglBindShader(p);
    /* the caller releases its handle; the binding keeps drawing */
    tglShaderRelease(p);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    TASK_CHECK(count_nonblack() > 500);
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_NONE);
    /* glClose releases the context reference */
    tglBindShader(NULL);
    p = make_prog(c, 2);
    TASK_CHECK(p != NULL);
    tglBindShader(p);
    tglShaderRelease(p);
    glClose();
    TASK_CHECK(tglGetBoundShader() == NULL);
    ZB_close(g_zb);
    g_zb = NULL;
    return 0;
}

static int r_r2_inputs(void)
{
    TGLShaderInst c[3];
    TGLShaderProgram *p;
    int px, py, fails = 0, checked = 0;
    int zpix, expected_blue;

    /* OUTPUT = r2 * (1/64, 1/64, 1, 1) */
    c[0] = mkc(3, 1.0f / 64.0f, 1.0f / 64.0f, 1.0f, 1.0f);
    c[1] = mk(TGL_SHADER_MUL, 4, 2, 3, 0);
    c[2] = mk(TGL_SHADER_OUTPUT, 0, 4, 0, 0);
    p = make_prog(c, ARRLEN(c));
    TASK_CHECK(p != NULL);

    rsetup();
    tglBindShader(p);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.75f, -0.75f, 0.0f);
    glVertex3f(0.75f, -0.75f, 0.0f);
    glVertex3f(0.0f, 0.75f, 0.0f);
    glEnd();
    tglBindShader(NULL);

    zpix = vp_zxform(0.0f);
    expected_blue = (int) (((zpix >> 14) / 65535.0) * 255.0 + 0.5);
    for (py = 0; py < RH; py++)
        for (px = 0; px < RW; px++) {
            int er, eg;
            if ((g_zb->pbuf[py * RW + px] & 0xffffffu) == 0)
                continue;
            er = (int) ((px + 0.5) / 64.0 * 255.0 + 0.5);
            eg = (int) ((py + 0.5) / 64.0 * 255.0 + 0.5);
            fails += !task_check(abs(pix_r(px, py) - er) <= 1,
                                 "pixel center x", __FILE__, __LINE__);
            fails += !task_check(abs(pix_g(px, py) - eg) <= 1,
                                 "pixel center y", __FILE__, __LINE__);
            fails += !task_check(abs(pix_b(px, py) - expected_blue) <= 1,
                                 "window depth", __FILE__, __LINE__);
            checked++;
        }
    fails += !task_check(checked > 500, "pixels checked", __FILE__, __LINE__);
    fails += !task_check(tglShaderGetError() == TGL_SHADER_RT_NONE,
                         "no runtime error", __FILE__, __LINE__);
    rteardown();
    tglShaderRelease(p);
    TASK_CHECK(fails == 0);
    return 0;
}

static int r_texture_matrix(void)
{
    static unsigned char texdata[256 * 256 * 3];
    TGLShaderInst c[2];
    TGLShaderProgram *p;
    GLuint tex = 0;
    int x, y, px, py, fails = 0, checked = 0;
    float sx[3], sy[3], su[3], sv[3];
    double dux, duy, dvx, dvy;

    for (y = 0; y < 256; y++)
        for (x = 0; x < 256; x++) {
            texdata[(y * 256 + x) * 3 + 0] = (unsigned char) x;
            texdata[(y * 256 + x) * 3 + 1] = (unsigned char) y;
            texdata[(y * 256 + x) * 3 + 2] = 0;
        }

    c[0] = mk(TGL_SHADER_TEX, 3, 0, 0, 0);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = make_prog(c, 2);
    TASK_CHECK(p != NULL);

    rsetup();
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 texdata);
    glEnable(GL_TEXTURE_2D);
    /* the texture matrix applies to r0 */
    glMatrixMode(GL_TEXTURE);
    glTranslatef(0.25f, 0.25f, 0.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity(); /* make the pipeline re-evaluate the texture matrix */
    tglBindShader(p);
    glBegin(GL_TRIANGLES);
    glTexCoord2f(0.0f, 0.0f);
    glVertex3f(-0.75f, -0.75f, 0.0f);
    glTexCoord2f(0.5f, 0.0f);
    glVertex3f(0.75f, -0.75f, 0.0f);
    glTexCoord2f(0.0f, 0.5f);
    glVertex3f(0.0f, 0.75f, 0.0f);
    glEnd();
    tglBindShader(NULL);

    sx[0] = (float) vp_xform(-0.75f);
    sx[1] = (float) vp_xform(0.75f);
    sx[2] = (float) vp_xform(0.0f);
    sy[0] = (float) vp_xform(0.75f);
    sy[1] = (float) vp_xform(0.75f);
    sy[2] = (float) vp_xform(-0.75f);
    su[0] = 0.25f;
    su[1] = 0.75f;
    su[2] = 0.25f;
    sv[0] = 0.25f;
    sv[1] = 0.25f;
    sv[2] = 0.75f;
    tri_plane(sx, sy, su, &dux, &duy);
    tri_plane(sx, sy, sv, &dvx, &dvy);

    for (py = 0; py < RH; py++)
        for (px = 0; px < RW; px++) {
            if (!test_covered(sx, sy, px, py))
                continue;
            {
                double cx = px + 0.5, cy = py + 0.5;
                double u = plane_at(sx, sy, su, dux, duy, cx, cy);
                double v = plane_at(sx, sy, sv, dvx, dvy, cx, cy);
                int etx = (int) ((u - floor(u)) * 256.0);
                int ety = (int) ((v - floor(v)) * 256.0);
                fails += !task_check(abs(pix_r(px, py) - etx) <= 1,
                                     "texture matrix u", __FILE__, __LINE__);
                fails += !task_check(abs(pix_g(px, py) - ety) <= 1,
                                     "texture matrix v", __FILE__, __LINE__);
                checked++;
            }
        }
    fails += !task_check(checked > 500, "pixels checked", __FILE__, __LINE__);
    fails += !task_check(tglShaderGetError() == TGL_SHADER_RT_NONE,
                         "no runtime error", __FILE__, __LINE__);
    rteardown();
    tglShaderRelease(p);
    TASK_CHECK(fails == 0);
    return 0;
}

static int r_texture_256(void)
{
    static unsigned char texdata[256 * 256 * 3];
    TGLShaderInst c[2];
    TGLShaderProgram *p;
    GLuint tex = 0;
    int x, y, px, py, fails = 0, checked = 0;
    float sx[3], sy[3], su[3], sv[3];
    double dux, duy, dvx, dvy;

    for (y = 0; y < 256; y++)
        for (x = 0; x < 256; x++) {
            texdata[(y * 256 + x) * 3 + 0] = (unsigned char) x;
            texdata[(y * 256 + x) * 3 + 1] = (unsigned char) y;
            texdata[(y * 256 + x) * 3 + 2] = (unsigned char) (x + y);
        }

    c[0] = mk(TGL_SHADER_TEX, 3, 0, 0, 0);
    c[1] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = make_prog(c, 2);
    TASK_CHECK(p != NULL);

    rsetup();
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 texdata);
    glEnable(GL_TEXTURE_2D);
    tglBindShader(p);
    glBegin(GL_TRIANGLES);
    glTexCoord2f(0.0f, 0.0f);
    glVertex3f(-0.9f, -0.9f, 0.0f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex3f(0.9f, -0.9f, 0.0f);
    glTexCoord2f(0.0f, 1.0f);
    glVertex3f(-0.9f, 0.9f, 0.0f);
    glEnd();
    tglBindShader(NULL);

    sx[0] = (float) vp_xform(-0.9f);
    sx[1] = (float) vp_xform(0.9f);
    sx[2] = (float) vp_xform(-0.9f);
    sy[0] = (float) vp_xform(0.9f);
    sy[1] = (float) vp_xform(0.9f);
    sy[2] = (float) vp_xform(-0.9f);
    su[0] = 0.0f;
    su[1] = 1.0f;
    su[2] = 0.0f;
    sv[0] = 0.0f;
    sv[1] = 0.0f;
    sv[2] = 1.0f;
    tri_plane(sx, sy, su, &dux, &duy);
    tri_plane(sx, sy, sv, &dvx, &dvy);

    for (py = 0; py < RH; py++)
        for (px = 0; px < RW; px++) {
            if (!test_covered(sx, sy, px, py))
                continue;
            {
                double cx = px + 0.5, cy = py + 0.5;
                double u = plane_at(sx, sy, su, dux, duy, cx, cy);
                double v = plane_at(sx, sy, sv, dvx, dvy, cx, cy);
                int etx = (int) ((u - floor(u)) * 256.0);
                int ety = (int) ((v - floor(v)) * 256.0);
                fails += !task_check(abs(pix_r(px, py) - etx) <= 1,
                                     "texel x", __FILE__, __LINE__);
                fails += !task_check(abs(pix_g(px, py) - ety) <= 1,
                                     "texel y", __FILE__, __LINE__);
                fails += !task_check(abs(pix_b(px, py) - ((etx + ety) & 0xff)) <=
                                         2,
                                     "texel sum", __FILE__, __LINE__);
                checked++;
            }
        }
    fails += !task_check(checked > 1000, "pixels checked", __FILE__, __LINE__);
    fails += !task_check(tglShaderGetError() == TGL_SHADER_RT_NONE,
                         "no runtime error", __FILE__, __LINE__);
    rteardown();
    tglShaderRelease(p);
    TASK_CHECK(fails == 0);
    return 0;
}

static int r_runtime_error(void)
{
    TGLShaderInst c[3];
    TGLShaderProgram *p;

    /* 1e30 * 1e30 overflows to infinity at runtime */
    c[0] = mkc(3, 1e30f, 1e30f, 1e30f, 1e30f);
    c[1] = mk(TGL_SHADER_MUL, 3, 3, 3, 0);
    c[2] = mk(TGL_SHADER_OUTPUT, 0, 3, 0, 0);
    p = make_prog(c, ARRLEN(c));
    TASK_CHECK(p != NULL);

    rsetup();
    tglBindShader(p);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glEnd();
    tglBindShader(NULL);
    /* the error is reported and no lane of the failing quads is committed */
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_NONFINITE);
    TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_NONE);
    TASK_CHECK(count_nonblack() == 0);
    rteardown();
    tglShaderRelease(p);

    /* quads that fail are dropped, quads completed before are kept */
    {
        TGLShaderInst c2[7];
        TGLShaderProgram *p2;
        c2[0] = mkc(3, 1e30f, 1e30f, 1e30f, 1e30f);
        c2[1] = mkc(4, 32, 0, 0, 0);
        c2[2] = mk(TGL_SHADER_LT, 5, 4, 2, 0); /* 32 < pixel x */
        c2[3] = mk(TGL_SHADER_IF, 0, 5, 0, 0);
        c2[4] = mk(TGL_SHADER_MUL, 3, 3, 3, 0); /* overflows right of 32 */
        c2[5] = mk(TGL_SHADER_ENDIF, 0, 0, 0, 0);
        c2[6] = mk(TGL_SHADER_OUTPUT, 0, 1, 0, 0);
        p2 = make_prog(c2, ARRLEN(c2));
        TASK_CHECK(p2 != NULL);

        rsetup();
        tglBindShader(p2);
        glColor3f(0.8f, 0.6f, 0.4f);
        glBegin(GL_TRIANGLES);
        glVertex3f(-1.0f, -1.0f, 0.0f);
        glVertex3f(1.0f, -1.0f, 0.0f);
        glVertex3f(0.0f, 1.0f, 0.0f);
        glEnd();
        tglBindShader(NULL);
        TASK_CHECK(tglShaderGetError() == TGL_SHADER_RT_NONFINITE);
        /* left-of-32 quads committed, the failing right side did not */
        TASK_CHECK(pix_r(16, 40) > 200);
        TASK_CHECK((g_zb->pbuf[40 * RW + 48] & 0xffffffu) == 0);
        /* a quad straddling the boundary drops all of its lanes */
        TASK_CHECK((g_zb->pbuf[40 * RW + 32] & 0xffffffu) == 0);
        rteardown();
        tglShaderRelease(p2);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* case table                                                           */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"verify", "both_sides", v_both_sides},
    {"verify", "missing_else", v_missing_else},
    {"verify", "undef_lowest_index", v_undef_lowest_index},
    {"verify", "repeat_semantics", v_repeat_semantics},
    {"verify", "break_bypass", v_break_bypass},
    {"verify", "continue_bypass", v_continue_bypass},
    {"verify", "defined_before_loop", v_defined_before_loop},
    {"verify", "bad_operands", v_bad_operands},
    {"verify", "structure", v_structure},
    {"verify", "loopctl", v_loopctl},
    {"verify", "derivative", v_derivative},
    {"verify", "budget", v_budget},
    {"verify", "repeat_count", v_repeat_count},
    {"verify", "imm", v_imm},
    {"verify", "size", v_size},
    {"verify", "depth", v_depth},
    {"verify", "create_keeps_binding", v_create_keeps_binding},
    {"control", "nested_branch", c_nested_branch},
    {"control", "loop_exit_rounds", c_loop_exit_rounds},
    {"control", "inner_continue_break", c_inner_continue_break},
    {"control", "discard_derivative", c_discard_derivative},
    {"control", "helper_lanes", c_helper_lanes},
    {"control", "zero_coverage", c_zero_coverage},
    {"control", "nonfinite", c_nonfinite},
    {"control", "tex", c_tex},
    {"control", "no_texture", c_no_texture},
    {"control", "program_copy_lifetime", c_program_copy_lifetime},
    {"render", "gouraud", r_gouraud},
    {"render", "clipped", r_clipped},
    {"render", "perspective_uv", r_perspective_uv},
    {"render", "derivative", r_derivative},
    {"render", "depth_occlusion", r_depth_occlusion},
    {"render", "single_lane", r_single_lane},
    {"render", "discard_depth", r_discard_depth},
    {"render", "unbind_fixed", r_unbind_fixed},
    {"render", "display_list", r_display_list},
    {"render", "state_reject", r_state_reject},
    {"render", "lifetime", r_lifetime},
    {"render", "r2_inputs", r_r2_inputs},
    {"render", "texture_matrix", r_texture_matrix},
    {"render", "texture_256", r_texture_256},
    {"render", "runtime_error", r_runtime_error},
};
const size_t task_case_count = ARRLEN(task_cases);
