/*
    Contract tests for edwards25519_XMD:SHA-512_ELL2_RO_ (RFC 9380).

    The expected values come straight from the standard text in
    baseline/rfc9380.txt (Appendix J.5.1) and from an independent Python
    reference of the same specification; they are never produced by the
    production mapping under test.  Compressed expected points are rebuilt
    from the standard's big-endian coordinates in rfc_compress() rather than
    copied from a production output.

    Each contract_* function returns the number of successful assertions
    (always positive) or a negative value / aborts on failure.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ed25519_h2c.h"
#include "fe.h"
#include "ge.h"

#include "h2c_vectors.inc"
#include "h2c_extras.inc"

#define RFC_DST "QUUX-V01-CS02-with-edwards25519_XMD:SHA-512_ELL2_RO_"
#define RFC_DST_LEN (sizeof(RFC_DST) - 1)

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            return -1;                                                       \
        }                                                                    \
        ++n;                                                                 \
    } while (0)

static int eq32(const unsigned char *a, const unsigned char *b) {
    return memcmp(a, b, 32) == 0;
}

static void be_to_le(unsigned char le[32], const unsigned char be[32]) {
    int i;
    for (i = 0; i < 32; ++i) {
        le[i] = be[31 - i];
    }
}

/*
    Independent point compression from RFC big-endian (x, y) coordinates:
    little-endian y with the parity of x in the top bit of the last byte.
    This does not call any production hash-to-curve routine.
*/
static void rfc_compress(const unsigned char px_be[32],
                         const unsigned char py_be[32],
                         unsigned char out[32]) {
    be_to_le(out, py_be);
    if (px_be[31] & 1) {
        out[31] |= 0x80;
    }
}

/* Recover canonical affine (x, y) bytes from an extended point using only
   the generic field primitives, not the mapping under test. */
static void p3_affine(const ge_p3 *p, unsigned char x[32],
                      unsigned char y[32]) {
    fe recip, xf, yf;

    fe_invert(recip, p->Z);
    fe_mul(xf, p->X, recip);
    fe_mul(yf, p->Y, recip);
    fe_tobytes(x, xf);
    fe_tobytes(y, yf);
}

static const unsigned char *rfc_dst(void) {
    return (const unsigned char *) RFC_DST;
}

/* ----------------------------------------------------------------------- */
/* J.5.1: five messages, checked layer by layer.                           */
/* ----------------------------------------------------------------------- */
int contract_vectors(void) {
    int n = 0;
    int i;

    for (i = 0; i < 5; ++i) {
        const struct h2c_vec *v = &h2c_vectors[i];
        unsigned char uniform[96];
        unsigned char expect[32], got[32], streamed[32];
        unsigned char xle[32], yle[32];
        fe u0, u1;
        ge_p3 Q0, Q1, R, P;
        ed25519_h2c_ctx ctx;

        /* expand_message_xmd -> 96 uniform bytes. */
        CHECK(ed25519_h2c_test_expand(v->msg, v->msg_len, rfc_dst(),
                                      RFC_DST_LEN, uniform) == 0);

        /* The two 48-byte big-endian integers reduce to the documented
           u[0], u[1] modulo p (not modulo the group order). */
        ed25519_h2c_test_reduce_wide(u0, uniform);
        ed25519_h2c_test_reduce_wide(u1, uniform + 48);
        fe_tobytes(got, u0);
        be_to_le(expect, v->u0);
        CHECK(eq32(got, expect));
        fe_tobytes(got, u1);
        be_to_le(expect, v->u1);
        CHECK(eq32(got, expect));

        /* Each u maps to the documented Q0 / Q1 Edwards point. */
        ed25519_h2c_test_map_to_curve(&Q0, u0);
        ed25519_h2c_test_map_to_curve(&Q1, u1);
        p3_affine(&Q0, xle, yle);
        be_to_le(expect, v->q0x);
        CHECK(eq32(xle, expect));
        be_to_le(expect, v->q0y);
        CHECK(eq32(yle, expect));
        p3_affine(&Q1, xle, yle);
        be_to_le(expect, v->q1x);
        CHECK(eq32(xle, expect));
        be_to_le(expect, v->q1y);
        CHECK(eq32(yle, expect));

        /* Re-run the final stage with the library group operations and
           compare coordinates against the documented P. */
        ge_p3_add(&R, &Q0, &Q1);
        ge_p3_mul8(&P, &R);
        p3_affine(&P, xle, yle);
        be_to_le(expect, v->px);
        CHECK(eq32(xle, expect));
        be_to_le(expect, v->py);
        CHECK(eq32(yle, expect));

        /* One-shot encoding matches compression rebuilt from RFC coords. */
        CHECK(ed25519_hash_to_curve(got, v->msg, v->msg_len, rfc_dst(),
                                   RFC_DST_LEN) == 0);
        rfc_compress(v->px, v->py, expect);
        CHECK(eq32(got, expect));

        /* Streaming over the same message must give the identical point. */
        CHECK(ed25519_h2c_init(&ctx, rfc_dst(), RFC_DST_LEN) == 0);
        if (v->msg_len != 0) {
            CHECK(ed25519_h2c_update(&ctx, v->msg, v->msg_len) == 0);
        }
        CHECK(ed25519_h2c_final(&ctx, streamed) == 0);
        CHECK(eq32(streamed, got));
    }

    return n;
}

/* ----------------------------------------------------------------------- */
/* Mapping internals: wide reduction, zero denominator, sign convention.   */
/* ----------------------------------------------------------------------- */
int contract_mapping(void) {
    int n = 0;
    int i;
    unsigned char got[32], expect[32];
    fe r, ref;
    ge_p3 point;
    static const unsigned char pattern[32] = {
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
        0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0x70
    };
    unsigned char wide48[48];

    /* fe_fromwide boundary answers: p -> 0 and 2^255 -> 19.  A truncated
       fe_frombytes could never produce these from the full 48-byte input. */
    ed25519_h2c_test_reduce_wide(r, wide_p);
    fe_tobytes(got, r);
    CHECK(eq32(got, red_zero));

    ed25519_h2c_test_reduce_wide(r, wide_2p255);
    fe_tobytes(got, r);
    CHECK(eq32(got, red_19));

    /* All-ones 384-bit integer folds every high byte into the result. */
    ed25519_h2c_test_reduce_wide(r, wide_ff);
    fe_tobytes(got, r);
    CHECK(eq32(got, wide_ff_mod_p));

    /* A zero-extended 32-byte value must match fe_frombytes exactly.
       fe_fromwide reads big-endian and fe_frombytes reads little-endian,
       so store the byte-reversed pattern in the wide buffer. */
    memset(wide48, 0, sizeof(wide48));
    for (i = 0; i < 32; ++i) {
        wide48[16 + i] = pattern[31 - i];
    }
    ed25519_h2c_test_reduce_wide(r, wide48);
    fe_frombytes(ref, pattern);
    fe_tobytes(got, r);
    fe_tobytes(expect, ref);
    CHECK(eq32(got, expect));

    /* u = 0: the G.2.2 rational map has a zero denominator and the
       standard CMOV exception must select the identity point. */
    fe_0(r);
    ed25519_h2c_test_map_to_curve(&point, r);
    ge_p3_tobytes(got, &point);
    CHECK(eq32(got, identity_point));
    {
        unsigned char x[32], y[32];
        p3_affine(&point, x, y);
        memset(expect, 0, 32);
        CHECK(eq32(x, expect));          /* x = 0 */
        memset(expect, 0, 32);
        expect[0] = 1;
        CHECK(eq32(y, expect));          /* y = 1 */
    }

    /* Clearing the cofactor of the identity stays the identity. */
    {
        ge_p3 cleared;
        ge_p3_mul8(&cleared, &point);
        ge_p3_tobytes(got, &cleared);
        CHECK(eq32(got, identity_point));
    }

    /* The sign bit of a documented compressed point is the parity of the
       RFC x coordinate, rebuilt here without any production routine. */
    {
        const struct h2c_vec *v = &h2c_vectors[1];     /* "abc" */
        unsigned char one_shot[32];

        rfc_compress(v->px, v->py, expect);
        CHECK(ed25519_hash_to_curve(one_shot, v->msg, v->msg_len, rfc_dst(),
                                   RFC_DST_LEN) == 0);
        CHECK(eq32(one_shot, expect));
        CHECK(((expect[31] >> 7) & 1) == (v->px[31] & 1));
    }

    return n;
}

/* ----------------------------------------------------------------------- */
/* Streaming: chunking, interleaving, DST copy, DST boundaries, NUL data.  */
/* ----------------------------------------------------------------------- */
static int stream_chunked(const unsigned char *msg, size_t len,
                          size_t chunk, const unsigned char *dst,
                          size_t dst_len, unsigned char out[32]) {
    ed25519_h2c_ctx ctx;
    size_t off = 0;

    if (ed25519_h2c_init(&ctx, dst, dst_len) != 0) {
        return -1;
    }
    while (off < len) {
        size_t take = len - off;
        if (chunk != 0 && take > chunk) {
            take = chunk;
        }
        if (ed25519_h2c_update(&ctx, msg + off, take) != 0) {
            return -1;
        }
        off += take;
        if (chunk == 0) {
            break;
        }
    }
    return ed25519_h2c_final(&ctx, out);
}

int contract_streaming(void) {
    int n = 0;
    static const size_t chunks[] = {1, 7, 63, 64, 127, 128, 129, 256, 0};
    static const unsigned char fill_a = 'A';
    static const unsigned char fill_b = 'B';
    unsigned char dst255[255], dst256[256], dst300[300];
    unsigned char dstbuf[64];
    unsigned char one_shot[32], got[32];
    int i, j;

    /* Every vector, under several chunk schedules including SHA-512 block
       (128-byte) boundaries, matches the one-shot result. */
    for (i = 0; i < 5; ++i) {
        const struct h2c_vec *v = &h2c_vectors[i];

        CHECK(ed25519_hash_to_curve(one_shot, v->msg, v->msg_len, rfc_dst(),
                                   RFC_DST_LEN) == 0);
        for (j = 0; j < (int) (sizeof(chunks) / sizeof(chunks[0])); ++j) {
            CHECK(stream_chunked(v->msg, v->msg_len, chunks[j], rfc_dst(),
                                 RFC_DST_LEN, got) == 0);
            CHECK(eq32(got, one_shot));
        }
    }

    /* Empty message: no updates, zero-length updates with a real pointer,
       and zero-length updates with a NULL pointer are all accepted. */
    {
        ed25519_h2c_ctx ctx;
        unsigned char empty_dst_ref[32];

        CHECK(ed25519_hash_to_curve(empty_dst_ref,
                                    (const unsigned char *) "", 0,
                                    rfc_dst(), RFC_DST_LEN) == 0);

        CHECK(ed25519_h2c_init(&ctx, rfc_dst(), RFC_DST_LEN) == 0);
        CHECK(ed25519_h2c_final(&ctx, got) == 0);
        CHECK(eq32(got, empty_dst_ref));

        CHECK(ed25519_h2c_init(&ctx, rfc_dst(), RFC_DST_LEN) == 0);
        CHECK(ed25519_h2c_update(&ctx, (const unsigned char *) "", 0) == 0);
        CHECK(ed25519_h2c_update(&ctx, NULL, 0) == 0);
        CHECK(ed25519_h2c_final(&ctx, got) == 0);
        CHECK(eq32(got, empty_dst_ref));
    }

    /* Two contexts updated alternately stay independent. */
    {
        ed25519_h2c_ctx ca, cb;
        const unsigned char ma[] = "interleaved-message-one";
        const unsigned char mb[] = "different-message-two";
        size_t la = sizeof(ma) - 1, lb = sizeof(mb) - 1;
        size_t off = 0;

        CHECK(ed25519_h2c_init(&ca, rfc_dst(), RFC_DST_LEN) == 0);
        CHECK(ed25519_h2c_init(&cb, rfc_dst(), RFC_DST_LEN) == 0);
        while (off < la || off < lb) {
            if (off < la) {
                CHECK(ed25519_h2c_update(&ca, ma + off, 1) == 0);
            }
            if (off < lb) {
                CHECK(ed25519_h2c_update(&cb, mb + off, 1) == 0);
            }
            ++off;
        }
        CHECK(ed25519_h2c_final(&ca, got) == 0);
        CHECK(ed25519_hash_to_curve(one_shot, ma, la, rfc_dst(),
                                   RFC_DST_LEN) == 0);
        CHECK(eq32(got, one_shot));
        CHECK(ed25519_h2c_final(&cb, got) == 0);
        CHECK(ed25519_hash_to_curve(one_shot, mb, lb, rfc_dst(),
                                   RFC_DST_LEN) == 0);
        CHECK(eq32(got, one_shot));
    }

    /* The DST is copied at init: clobbering the caller's buffer afterwards
       must not change the result. */
    memset(dstbuf, 0, sizeof(dstbuf));
    memcpy(dstbuf, RFC_DST, RFC_DST_LEN);
    {
        ed25519_h2c_ctx ctx;
        const unsigned char msg[] = "dst-ownership";

        CHECK(ed25519_h2c_init(&ctx, dstbuf, RFC_DST_LEN) == 0);
        memset(dstbuf, 0xEE, sizeof(dstbuf));
        CHECK(ed25519_h2c_update(&ctx, msg, sizeof(msg) - 1) == 0);
        CHECK(ed25519_h2c_final(&ctx, got) == 0);
        CHECK(ed25519_hash_to_curve(one_shot, msg, sizeof(msg) - 1,
                                   rfc_dst(), RFC_DST_LEN) == 0);
        CHECK(eq32(got, one_shot));
    }

    /* Empty DST is allowed; 255/256/300-byte DSTs exercise the verbatim
       vs. Section 5.3.3 shortening paths (256 is never just truncated).
       The fill bytes match the independently generated expected table. */
    memset(dst255, fill_a, sizeof(dst255));   /* 255 'A' */
    memset(dst256, fill_b, sizeof(dst256));   /* 256 'B' */
    memset(dst300, 'C', sizeof(dst300));      /* 300 'C' */

    CHECK(ed25519_hash_to_curve(got, (const unsigned char *) "abc", 3,
                                NULL, 0) == 0);
    CHECK(eq32(got, exp_dst_empty));

    CHECK(ed25519_hash_to_curve(got, (const unsigned char *) "abc", 3,
                                dst255, 255) == 0);
    CHECK(eq32(got, exp_dst_255));

    CHECK(ed25519_hash_to_curve(got, (const unsigned char *) "abc", 3,
                                dst256, 256) == 0);
    CHECK(eq32(got, exp_dst_256));

    CHECK(ed25519_hash_to_curve(got, (const unsigned char *) "abc", 3,
                                dst300, 300) == 0);
    CHECK(eq32(got, exp_dst_300));

    /* A 256-byte DST shortened per 5.3.3 differs from truncating it to
       255 bytes: the two encodings must not collide. */
    {
        unsigned char truncated[32];
        CHECK(ed25519_hash_to_curve(truncated,
                                    (const unsigned char *) "abc", 3,
                                    dst256, 255) == 0);
        CHECK(!eq32(truncated, exp_dst_256));
    }

    /* Embedded NUL bytes are real message content, not terminators. */
    {
        const unsigned char with_nul[] = {'a', 0, 'b'};
        const unsigned char no_nul[] = {'a', 'b'};

        CHECK(ed25519_hash_to_curve(got, with_nul, sizeof(with_nul),
                                   rfc_dst(), RFC_DST_LEN) == 0);
        CHECK(eq32(got, exp_msg_nul));
        CHECK(ed25519_hash_to_curve(got, no_nul, sizeof(no_nul),
                                   rfc_dst(), RFC_DST_LEN) == 0);
        CHECK(eq32(got, exp_msg_ab));
        CHECK(!eq32(exp_msg_nul, exp_msg_ab));
    }

    return n;
}

/* ----------------------------------------------------------------------- */
/* Parameter and state error handling.                                     */
/* ----------------------------------------------------------------------- */
int contract_errors(void) {
    int n = 0;
    ed25519_h2c_ctx ctx;
    unsigned char out[32];
    const unsigned char msg[] = "error-path";
    const unsigned char good[32] = {
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5
    };

    /* NULL output is rejected and a NULL context cannot be touched. */
    CHECK(ed25519_hash_to_curve(NULL, msg, sizeof(msg) - 1, rfc_dst(),
                                RFC_DST_LEN) == -1);
    CHECK(ed25519_h2c_init(NULL, rfc_dst(), RFC_DST_LEN) == -1);
    CHECK(ed25519_h2c_update(NULL, msg, 1) == -1);
    CHECK(ed25519_h2c_final(NULL, out) == -1);

    /* NULL data pointers are allowed only at zero length. */
    CHECK(ed25519_hash_to_curve(out, NULL, 4, rfc_dst(), RFC_DST_LEN) == -1);
    CHECK(ed25519_hash_to_curve(out, msg, sizeof(msg) - 1, NULL, 7) == -1);
    CHECK(ed25519_h2c_init(&ctx, NULL, 7) == -1);

    /* Zero-length NULL inputs are valid and leave out populated. */
    CHECK(ed25519_hash_to_curve(out, NULL, 0, NULL, 0) == 0);
    CHECK(!eq32(out, good));

    /* A failed call never modifies the output buffer. */
    memcpy(out, good, 32);
    CHECK(ed25519_hash_to_curve(out, NULL, 4, rfc_dst(), RFC_DST_LEN) == -1);
    CHECK(eq32(out, good));

    /* After final, update and a second final are errors. */
    CHECK(ed25519_h2c_init(&ctx, rfc_dst(), RFC_DST_LEN) == 0);
    CHECK(ed25519_h2c_update(&ctx, msg, sizeof(msg) - 1) == 0);
    CHECK(ed25519_h2c_final(&ctx, out) == 0);
    CHECK(ed25519_h2c_update(&ctx, msg, 1) == -1);
    memcpy(out, good, 32);
    CHECK(ed25519_h2c_final(&ctx, out) == -1);
    CHECK(eq32(out, good));

    /* A parameter error poisons an active context; no partial result is
       published and only re-init makes it usable again. */
    CHECK(ed25519_h2c_init(&ctx, rfc_dst(), RFC_DST_LEN) == 0);
    CHECK(ed25519_h2c_update(&ctx, msg, 2) == 0);
    CHECK(ed25519_h2c_update(&ctx, NULL, 3) == -1);
    CHECK(ed25519_h2c_update(&ctx, msg, 1) == -1);
    memcpy(out, good, 32);
    CHECK(ed25519_h2c_final(&ctx, out) == -1);
    CHECK(eq32(out, good));

    /* Re-init restores the context and the full flow works again. */
    CHECK(ed25519_h2c_init(&ctx, rfc_dst(), RFC_DST_LEN) == 0);
    CHECK(ed25519_h2c_update(&ctx, msg, sizeof(msg) - 1) == 0);
    CHECK(ed25519_h2c_final(&ctx, out) == 0);
    {
        unsigned char expect[32];
        CHECK(ed25519_hash_to_curve(expect, msg, sizeof(msg) - 1, rfc_dst(),
                                   RFC_DST_LEN) == 0);
        CHECK(eq32(out, expect));
    }

    /* final with a NULL output poisons the context as well. */
    CHECK(ed25519_h2c_init(&ctx, rfc_dst(), RFC_DST_LEN) == 0);
    CHECK(ed25519_h2c_final(&ctx, NULL) == -1);
    CHECK(ed25519_h2c_update(&ctx, msg, 1) == -1);
    CHECK(ed25519_h2c_init(&ctx, rfc_dst(), RFC_DST_LEN) == 0);
    CHECK(ed25519_h2c_final(&ctx, out) == 0);

    return n;
}
