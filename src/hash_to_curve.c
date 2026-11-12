/*
    RFC 9380 hash-to-curve, suite edwards25519_XMD:SHA-512_ELL2_RO_.

    Pipeline (all on top of the library's own SHA-512, fe and ge code):

      expand_message_xmd (Section 5.3.1)
        -> hash_to_field, two 48-byte OS2IP integers reduced mod p=2^255-19
           (Section 5.2, L = 48)
        -> Elligator 2 on curve25519 (Appendix G.2.1)
        -> birational map onto edwards25519 (Appendix G.2.2)
        -> Q0 + Q1 (Section 3, hash_to_curve)
        -> clear cofactor h_eff = 8 (Section 7)
        -> standard 32-byte compressed Edwards encoding.

    The wide 384-bit integers are reduced modulo the field prime with
    fe_fromwide; they are never passed to sc_reduce (which reduces modulo
    the group order) and the high bytes are never truncated.
*/

#include <string.h>

#include "ed25519_h2c.h"
#include "fe.h"
#include "ge.h"
#include "sha512.h"

#define H2C_BYTES_PER_FIELD 48     /* L = ceil((255 + 128) / 8)            */
#define H2C_FIELD_COUNT     2
#define H2C_UNIFORM_BYTES   (H2C_BYTES_PER_FIELD * H2C_FIELD_COUNT) /* 96 */
#define SHA512_BLOCK        128
#define SHA512_DIGEST       64
#define SHA512_MAX_BYTES    (UINT64_MAX / 8)  /* 64-bit bit-counter bound */
/* Headroom in the b_0 hash for Z_pad (128), the 3-byte fixed suffix and the
   at-most-256-byte DST_prime. */
#define H2C_B0_OVERHEAD     512
#define OVERSIZE_PREFIX     "H2C-OVERSIZE-DST-"
#define OVERSIZE_PREFIX_LEN 17

/* J = 486662, the curve25519 Montgomery parameter. */
static const fe h2c_J = {486662, 0, 0, 0, 0, 0, 0, 0, 0, 0};

/* c3 = sqrt(-1), canonical (even) representative. */
static const fe h2c_sqrtm1 = {
    34513072, 25610706, 9377949, 3500415, 12389472,
    33281959, 41962654, 31548777, 326685, 11406482
};

/* c2 = 2^((p + 3) / 8). */
static const fe h2c_pow2_252_2 = {
    34513073, 25610706, 9377949, 3500415, 12389472,
    33281959, 41962654, 31548777, 326685, 11406482
};

/* Appendix G.2.2 constant c1 = sqrt(-486664), the root with sgn0(c1) == 0. */
static const fe h2c_ed_c1 = {
    54885894, 25242303, 55597453, 9067496, 51808079,
    33312638, 25456129, 14121551, 54921728, 3972023
};

static const unsigned char h2c_zero_block[SHA512_BLOCK] = {0};

static unsigned int fe_equal(const fe a, const fe b) {
    fe diff;

    fe_sub(diff, a, b);
    return (unsigned int) (fe_isnonzero(diff) == 0);
}

/*
    Copy the caller's DST into the fixed-size context buffer.  DSTs longer
    than 255 bytes are shortened per Section 5.3.3 with SHA-512, never
    truncated.  An empty DST is accepted.  Returns 0 on success, -1 on a
    bad pointer.
*/
static int h2c_copy_dst(ed25519_h2c_ctx *ctx, const unsigned char *dst,
                        size_t dst_len) {
    if (dst_len > ED25519_H2C_DST_MAX) {
        sha512_context hash;

        sha512_init(&hash);
        sha512_update(&hash,
                      (const unsigned char *) OVERSIZE_PREFIX,
                      OVERSIZE_PREFIX_LEN);
        sha512_update(&hash, dst, dst_len);
        sha512_final(&hash, ctx->dst);
        ctx->dst_len = SHA512_DIGEST;
    } else {
        if (dst_len != 0 && dst == NULL) {
            return -1;
        }
        if (dst_len != 0) {
            memcpy(ctx->dst, dst, dst_len);
        }
        ctx->dst_len = (unsigned char) dst_len;
    }
    return 0;
}

int ed25519_h2c_init(ed25519_h2c_ctx *ctx, const unsigned char *dst,
                    size_t dst_len) {
    if (ctx == NULL) {
        return -1;
    }
    if (dst_len != 0 && dst == NULL) {
        return -1;
    }
    if (h2c_copy_dst(ctx, dst, dst_len) != 0) {
        return -1;
    }

    sha512_init(&ctx->hash);
    /* Z_pad = I2OSP(0, s_in_bytes), s_in_bytes = 128 for SHA-512. */
    sha512_update(&ctx->hash, h2c_zero_block, SHA512_BLOCK);

    ctx->msg_len = 0;
    ctx->state = ED25519_H2C_STATE_ACTIVE;
    return 0;
}

int ed25519_h2c_update(ed25519_h2c_ctx *ctx, const unsigned char *message,
                      size_t message_len) {
    if (ctx == NULL) {
        return -1;
    }
    if (ctx->state != ED25519_H2C_STATE_ACTIVE) {
        ctx->state = ED25519_H2C_STATE_ERROR;
        return -1;
    }
    if (message_len != 0 && message == NULL) {
        ctx->state = ED25519_H2C_STATE_ERROR;
        return -1;
    }
    /* Verify both the running length and the SHA-512 64-bit bit-counter
       bound before a single byte is consumed. */
    if (message_len > SHA512_MAX_BYTES - H2C_B0_OVERHEAD - ctx->msg_len) {
        ctx->state = ED25519_H2C_STATE_ERROR;
        return -1;
    }

    if (message_len != 0) {
        if (sha512_update(&ctx->hash, message, message_len) != 0) {
            ctx->state = ED25519_H2C_STATE_ERROR;
            return -1;
        }
        ctx->msg_len += (unsigned long long) message_len;
    }
    return 0;
}

/*
    Finish expand_message_xmd.  The context has already absorbed
    Z_pad || msg.  Append l_i_b_str || 0x00 || DST_prime to obtain b_0,
    then derive b_1 and b_2 and return the first 96 uniform bytes.
*/
static int h2c_expand_finish(ed25519_h2c_ctx *ctx,
                             unsigned char uniform[H2C_UNIFORM_BYTES]) {
    unsigned char b0[SHA512_DIGEST];
    unsigned char b1[SHA512_DIGEST];
    unsigned char xored[SHA512_DIGEST];
    unsigned char suffix[3];
    unsigned char dst_prime_len;
    int i;

    /* l_i_b_str = I2OSP(96, 2), then I2OSP(0, 1). */
    suffix[0] = (unsigned char) (H2C_UNIFORM_BYTES >> 8);
    suffix[1] = (unsigned char) (H2C_UNIFORM_BYTES & 0xff);
    suffix[2] = 0;
    sha512_update(&ctx->hash, suffix, sizeof(suffix));
    sha512_update(&ctx->hash, ctx->dst, ctx->dst_len);
    dst_prime_len = ctx->dst_len;
    sha512_update(&ctx->hash, &dst_prime_len, 1);
    sha512_final(&ctx->hash, b0);

    /* b_1 = H(b_0 || I2OSP(1,1) || DST_prime). */
    sha512_init(&ctx->hash);
    sha512_update(&ctx->hash, b0, SHA512_DIGEST);
    suffix[0] = 1;
    sha512_update(&ctx->hash, suffix, 1);
    sha512_update(&ctx->hash, ctx->dst, ctx->dst_len);
    sha512_update(&ctx->hash, &dst_prime_len, 1);
    sha512_final(&ctx->hash, b1);

    for (i = 0; i < SHA512_DIGEST; ++i) {
        xored[i] = b0[i] ^ b1[i];
    }

    /* b_2 = H(strxor(b_0, b_1) || I2OSP(2,1) || DST_prime). */
    sha512_init(&ctx->hash);
    sha512_update(&ctx->hash, xored, SHA512_DIGEST);
    suffix[0] = 2;
    sha512_update(&ctx->hash, suffix, 1);
    sha512_update(&ctx->hash, ctx->dst, ctx->dst_len);
    sha512_update(&ctx->hash, &dst_prime_len, 1);
    sha512_final(&ctx->hash, xored);  /* reuse xored for b_2 */

    memcpy(uniform, b1, SHA512_DIGEST);
    memcpy(uniform + SHA512_DIGEST, xored,
           H2C_UNIFORM_BYTES - SHA512_DIGEST);  /* 32 bytes of b_2 */
    return 0;
}

/*
    Appendix G.2.1: map_to_curve_elligator2_curve25519.

    Produces (xn / xd, yn / 1) on curve25519.  The projective straight-line
    form never divides, so the Z*u^2 == -1 exceptional case and u = 0 are
    handled by the same formulas with no special-casing or invalid values.
*/
static void h2c_map_montgomery(fe xn_out, fe xd_out, fe yn_out,
                               const fe u) {
    fe tv1, tv2, tv3;
    fe xd, x1n, gxd, gx1;
    fe y11, y12, y1, x2n, y21, y22, gx2, y2, y;
    fe neg_y;
    unsigned int e1, e2, e3, e4;

    fe_sq(tv1, u);                       /* 1. tv1 = u^2            */
    fe_add(tv1, tv1, tv1);               /* 2. tv1 = 2 * u^2        */
    fe_1(tv2);
    fe_add(xd, tv1, tv2);                /* 3. xd = tv1 + 1         */
    fe_neg(x1n, h2c_J);                  /* 4. x1n = -J             */
    fe_sq(tv2, xd);                      /* 5. tv2 = xd^2           */
    fe_mul(gxd, tv2, xd);                /* 6. gxd = xd^3           */
    fe_mul(gx1, h2c_J, tv1);             /* 7. gx1 = J * tv1        */
    fe_mul(gx1, gx1, x1n);               /* 8.                      */
    fe_add(gx1, gx1, tv2);               /* 9.                      */
    fe_mul(gx1, gx1, x1n);               /* 10.                     */
    fe_sq(tv3, gxd);                     /* 11. tv3 = gxd^2         */
    fe_sq(tv2, tv3);                     /* 12. tv2 = gxd^4         */
    fe_mul(tv3, tv3, gxd);               /* 13. tv3 = gxd^3         */
    fe_mul(tv3, tv3, gx1);               /* 14. tv3 = gx1*gxd^3     */
    fe_mul(tv2, tv2, tv3);               /* 15. tv2 = gx1*gxd^7     */
    fe_pow22523(y11, tv2);               /* 16. (p-5)/8 = 2^252 - 3 */
    fe_mul(y11, y11, tv3);               /* 17.                     */
    fe_mul(y12, y11, h2c_sqrtm1);        /* 18. y12 = y11 * c3      */
    fe_sq(tv2, y11);                     /* 19.                     */
    fe_mul(tv2, tv2, gxd);               /* 20.                     */
    e1 = fe_equal(tv2, gx1);             /* 21.                     */
    fe_copy(y1, y12);                    /* 22.                     */
    fe_cmov(y1, y11, e1);
    fe_mul(x2n, x1n, tv1);               /* 23. x2n                 */
    fe_mul(y21, y11, u);                 /* 24.                     */
    fe_mul(y21, y21, h2c_pow2_252_2);    /* 25. y21 *= c2           */
    fe_mul(y22, y21, h2c_sqrtm1);        /* 26. y22 = y21 * c3      */
    fe_mul(gx2, gx1, tv1);               /* 27. gx2                 */
    fe_sq(tv2, y21);                     /* 28.                     */
    fe_mul(tv2, tv2, gxd);               /* 29.                     */
    e2 = fe_equal(tv2, gx2);             /* 30.                     */
    fe_copy(y2, y22);                    /* 31.                     */
    fe_cmov(y2, y21, e2);
    fe_sq(tv2, y1);                      /* 32.                     */
    fe_mul(tv2, tv2, gxd);               /* 33.                     */
    e3 = fe_equal(tv2, gx1);             /* 34.                     */
    fe_copy(xn_out, x2n);                /* 35. xn                  */
    fe_cmov(xn_out, x1n, e3);
    fe_copy(y, y2);                      /* 36. y                   */
    fe_cmov(y, y1, e3);
    e4 = (unsigned int) fe_isnegative(y);/* 37. sgn0(y) == 1        */
    fe_neg(neg_y, y);                    /* 38. fix sign of y       */
    fe_cmov(y, neg_y, (e3 ^ e4) & 1u);

    fe_copy(xd_out, xd);
    fe_copy(yn_out, y);                  /* 39. yd = 1              */
}

/*
    Appendix G.2.2: map the curve25519 point onto edwards25519 and emit it
    in extended (ge_p3) coordinates.  The sign of h2c_ed_c1 is fixed
    (sgn0 == 0) so the map agrees with RFC 7748 / Erratum 4730; nothing
    here negates the resulting point.
*/
static void h2c_map_to_curve(ge_p3 *out, const fe u) {
    fe xMn, xMd, yMn;       /* Montgomery (xMn/xMd, yMn/1) */
    fe vn, zd, wn, wd;      /* Edwards (vn/zd, wn/wd)       */
    fe tv;
    unsigned int e;

    h2c_map_montgomery(xMn, xMd, yMn, u);

    fe_mul(vn, xMn, h2c_ed_c1);          /* 2-3. xn = xMn * c1 (yMd = 1) */
    fe_mul(zd, xMd, yMn);                /* 4. xd = xMd * yMn            */
    fe_sub(wn, xMn, xMd);                /* 5. yn = xMn - xMd            */
    fe_add(wd, xMn, xMd);                /* 6. yd = xMn + xMd            */

    fe_mul(tv, zd, wd);                  /* 7-8. e = (xd * yd) == 0      */
    e = (unsigned int) (fe_isnonzero(tv) == 0);

    fe_0(tv);                            /* 9.  xn = CMOV(xn, 0, e)      */
    fe_cmov(vn, tv, e);
    fe_1(tv);                            /* 10. xd = CMOV(xd, 1, e)      */
    fe_cmov(zd, tv, e);
    fe_1(tv);                            /* 11. yn = CMOV(yn, 1, e)      */
    fe_cmov(wn, tv, e);
    fe_1(tv);                            /* 12. yd = CMOV(yd, 1, e)      */
    fe_cmov(wd, tv, e);

    /* Build extended coordinates: v = vn/zd, w = wn/wd. */
    fe_mul(out->X, vn, wd);
    fe_mul(out->Y, wn, zd);
    fe_mul(out->Z, zd, wd);
    fe_mul(out->T, vn, wn);
}

static void h2c_hash_to_point(const unsigned char uniform[H2C_UNIFORM_BYTES],
                              ge_p3 *P) {
    fe u0, u1;
    ge_p3 Q0, Q1, R;

    fe_fromwide(u0, uniform);
    fe_fromwide(u1, uniform + H2C_BYTES_PER_FIELD);

    h2c_map_to_curve(&Q0, u0);
    h2c_map_to_curve(&Q1, u1);

    ge_p3_add(&R, &Q0, &Q1);             /* R = Q0 + Q1            */
    ge_p3_mul8(P, &R);                   /* P = clear_cofactor(R)  */
}

int ed25519_h2c_final(ed25519_h2c_ctx *ctx, unsigned char *out) {
    unsigned char uniform[H2C_UNIFORM_BYTES];
    ge_p3 P;

    if (ctx == NULL || out == NULL) {
        /* A parameter error on an active context poisons it: only
           ed25519_h2c_init can make it usable again. */
        if (ctx != NULL) {
            ctx->state = ED25519_H2C_STATE_ERROR;
        }
        return -1;
    }
    if (ctx->state != ED25519_H2C_STATE_ACTIVE) {
        ctx->state = ED25519_H2C_STATE_ERROR;
        return -1;
    }

    /* Leave the active state before touching the output; only the final
       encoding writes to out, so a failure never modifies it. */
    ctx->state = ED25519_H2C_STATE_DONE;
    h2c_expand_finish(ctx, uniform);
    h2c_hash_to_point(uniform, &P);
    ge_p3_tobytes(out, &P);
    return 0;
}

int ed25519_hash_to_curve(unsigned char *out, const unsigned char *message,
                          size_t message_len, const unsigned char *dst,
                          size_t dst_len) {
    ed25519_h2c_ctx ctx;

    if (out == NULL) {
        return -1;
    }
    if (ed25519_h2c_init(&ctx, dst, dst_len) != 0) {
        return -1;
    }
    if (ed25519_h2c_update(&ctx, message, message_len) != 0) {
        return -1;
    }
    return ed25519_h2c_final(&ctx, out);
}

#ifdef ED25519_H2C_TESTING
int ed25519_h2c_test_expand(const unsigned char *message, size_t message_len,
                            const unsigned char *dst, size_t dst_len,
                            unsigned char uniform[H2C_UNIFORM_BYTES]) {
    ed25519_h2c_ctx ctx;

    if (ed25519_h2c_init(&ctx, dst, dst_len) != 0) {
        return -1;
    }
    if (ed25519_h2c_update(&ctx, message, message_len) != 0) {
        return -1;
    }
    return h2c_expand_finish(&ctx, uniform);
}

void ed25519_h2c_test_reduce_wide(fe out, const unsigned char wide[48]) {
    fe_fromwide(out, wide);
}

void ed25519_h2c_test_map_to_curve(ge_p3 *out, const fe u) {
    h2c_map_to_curve(out, u);
}
#endif
