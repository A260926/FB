#ifndef ED25519_H2C_H
#define ED25519_H2C_H

/*
    RFC 9380 hash-to-curve for edwards25519.

    This header adds the edwards25519_XMD:SHA-512_ELL2_RO_ suite on top of
    the existing ed25519 library.  It implements the random-oracle encoding
    only (hash_to_curve, two field elements); the NU suite and ristretto255
    are intentionally not supported.

    The output is a 32-byte standard compressed Edwards point (RFC 8032
    encoding): the y coordinate in little-endian form, with the sign of x
    stored in the most significant bit of the last byte.
*/

#include <stddef.h>

#include "ed25519.h"
#include "sha512.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of bytes written by ed25519_hash_to_curve / ed25519_h2c_final. */
#define ED25519_H2C_HASH_LEN 32

/* Longest DST accepted verbatim; longer DSTs are shortened per RFC 9380 5.3.3. */
#define ED25519_H2C_DST_MAX 255

/*
    Streaming context for the hash-to-curve suite.

    The context is fully contained in this struct (no heap allocation, no
    external pointers are retained): it holds the in-progress SHA-512 state
    of the expand_message_xmd b_0 hash, a private copy of the effective DST
    (at most 255 bytes), the running message length and a state flag.
    sizeof(ed25519_h2c_ctx) is fixed regardless of message or DST length.
*/
typedef struct ed25519_h2c_ctx_ {
    /* b_0 = H(Z_pad || msg || l_i_b_str || 0x00 || DST_prime), fed
       incrementally as message fragments arrive. */
    sha512_context hash;

    /* Private copy of the effective DST (already shortened if necessary). */
    unsigned char dst[ED25519_H2C_DST_MAX];
    unsigned char dst_len;

    /* Total message bytes consumed; checked against the SHA-512 bit counter
       limit before any input is absorbed. */
    unsigned long long msg_len;

    /* One of the ED25519_H2C_STATE_* values below. */
    unsigned char state;
} ed25519_h2c_ctx;

#define ED25519_H2C_STATE_ACTIVE 1
#define ED25519_H2C_STATE_DONE   2
#define ED25519_H2C_STATE_ERROR  3

/*
    One-shot random-oracle encoding.

    Writes ED25519_H2C_HASH_LEN compressed point bytes to out and returns 0.
    A NULL message is permitted only when message_len is 0 (empty message);
    a NULL dst is permitted only when dst_len is 0 (empty DST, which the
    suite accepts).  DSTs longer than 255 bytes are shortened per Section
    5.3.3 rather than truncated.

    Returns -1 on any parameter or state error.  On failure out is never
    modified.
*/
int ED25519_DECLSPEC ed25519_hash_to_curve(unsigned char *out,
                                          const unsigned char *message,
                                          size_t message_len,
                                          const unsigned char *dst,
                                          size_t dst_len);

/*
    Initialize a streaming context with the given DST.  The DST is copied
    (and, if longer than 255 bytes, hashed down per Section 5.3.3); the
    caller's buffer may be reused or modified afterwards.  Returns 0 on
    success, -1 on a parameter error.  Reinitializing a context makes it
    usable again regardless of its previous state.
*/
int ED25519_DECLSPEC ed25519_h2c_init(ed25519_h2c_ctx *ctx,
                                     const unsigned char *dst,
                                     size_t dst_len);

/*
    Absorb one message fragment.  Fragments are streamed straight into the
    SHA-512 state: the full message is never assembled and no external
    pointer is retained.  message may be NULL only when message_len is 0;
    zero-length updates are allowed and have no effect.  Length limits are
    checked before any byte is consumed.

    Returns 0 on success, -1 on a parameter error or when the context is
    not in the active state.  A failed update poisons the context: only
    ed25519_h2c_init can make it usable again.
*/
int ED25519_DECLSPEC ed25519_h2c_update(ed25519_h2c_ctx *ctx,
                                       const unsigned char *message,
                                       size_t message_len);

/*
    Finalize the encoding and write the compressed point to out.  Returns 0
    on success; on failure out is left untouched and -1 is returned.  After
    success (or failure) the context leaves the active state, so further
    update/final calls return -1 until the context is reinitialized.
*/
int ED25519_DECLSPEC ed25519_h2c_final(ed25519_h2c_ctx *ctx,
                                      unsigned char *out);

#ifdef ED25519_H2C_TESTING
/*
    Private entry points for tests/h2c_contract.c only.  These are not part
    of the supported API and are compiled in solely when
    ED25519_H2C_TESTING is defined.
*/
#include "fe.h"
#include "ge.h"

/* Full 96-byte expand_message_xmd output (two 48-byte field encodings). */
int ed25519_h2c_test_expand(const unsigned char *message,
                           size_t message_len,
                           const unsigned char *dst,
                           size_t dst_len,
                           unsigned char uniform[96]);

/* Reduce one 48-byte big-endian OS2IP integer modulo p = 2^255 - 19. */
void ed25519_h2c_test_reduce_wide(fe out, const unsigned char wide[48]);

/* Elligator2 (curve25519) plus the rational map onto edwards25519. */
void ed25519_h2c_test_map_to_curve(ge_p3 *out, const fe u);
#endif

#ifdef __cplusplus
}
#endif

#endif
