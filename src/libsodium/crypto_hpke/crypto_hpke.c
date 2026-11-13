
/*
 * HPKE (RFC 9180), ciphersuite 0x0020/0x0001/0x0003:
 *
 *   DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, ChaCha20Poly1305
 *
 * Only the base mode (0x00) and the authenticated mode (0x02) are
 * implemented.  This is the RFC 9180 construction on top of the
 * library's X25519, HKDF-SHA256 and ChaCha20-Poly1305 primitives; it
 * is not a renamed crypto_kx session key.
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "crypto_aead_chacha20poly1305.h"
#include "crypto_auth_hmacsha256.h"
#include "crypto_hpke.h"
#include "crypto_kdf_hkdf_sha256.h"
#include "crypto_scalarmult_curve25519.h"
#include "randombytes.h"
#include "utils.h"

#define HPKE_NH 32U
#define HPKE_NSK 32U
#define HPKE_NENC 32U
#define HPKE_NDH 32U
#define HPKE_NK 32U
#define HPKE_NN 12U
#define HPKE_NT 16U

#define HPKE_MODE_BASE 0U
#define HPKE_MODE_AUTH 2U

#define HPKE_ROLE_SENDER 1U
#define HPKE_ROLE_RECEIVER 2U

static const unsigned char hpke_version_label[7] = {
    'H', 'P', 'K', 'E', '-', 'v', '1'
};

/* suite_id = concat("KEM", I2OSP(0x0020, 2)) */
static const unsigned char kem_suite_id[5] = {
    'K', 'E', 'M', 0x00U, 0x20U
};

/* suite_id = concat("HPKE", I2OSP(0x0020, 2), I2OSP(0x0001, 2), I2OSP(0x0003, 2)) */
static const unsigned char hpke_suite_id[10] = {
    'H', 'P', 'K', 'E', 0x00U, 0x20U, 0x00U, 0x01U, 0x00U, 0x03U
};

struct crypto_hpke_ctx {
    unsigned char key[HPKE_NK];
    unsigned char base_nonce[HPKE_NN];
    unsigned char exporter_secret[HPKE_NH];
    uint64_t      seq;
    unsigned char role;
};

/*
 * LabeledExtract(salt, label, ikm) from RFC 9180 Section 4:
 *   labeled_ikm = concat("HPKE-v1", suite_id, label, ikm)
 *   Extract(salt, labeled_ikm)
 *
 * The HMAC is fed incrementally so callers never have to allocate a
 * contiguous copy of a large ikm (info, ikmE, ...).
 */
static int
hpke_labeled_extract(unsigned char prk[HPKE_NH],
                     const unsigned char *suite_id, size_t suite_id_len,
                     const unsigned char *salt, size_t salt_len,
                     const char *label, size_t label_len,
                     const unsigned char *ikm, size_t ikm_len)
{
    crypto_kdf_hkdf_sha256_state st;

    if (crypto_kdf_hkdf_sha256_extract_init(&st, salt, salt_len) != 0) {
        return -1;
    }
    if (crypto_kdf_hkdf_sha256_extract_update(&st, hpke_version_label,
                                              sizeof hpke_version_label) != 0 ||
        crypto_kdf_hkdf_sha256_extract_update(&st, suite_id,
                                              suite_id_len) != 0 ||
        crypto_kdf_hkdf_sha256_extract_update
            (&st, (const unsigned char *) label, label_len) != 0 ||
        (ikm_len != 0U &&
         crypto_kdf_hkdf_sha256_extract_update(&st, ikm, ikm_len) != 0)) {
        sodium_memzero(&st, sizeof st);
        return -1;
    }
    return crypto_kdf_hkdf_sha256_extract_final(&st, prk);
}

/*
 * Build labeled_info = I2OSP(L, 2) || "HPKE-v1" || suite_id || label || info
 * into a freshly allocated buffer.
 */
static int
hpke_build_labeled_info(unsigned char **out, size_t *out_len, size_t L,
                        const unsigned char *suite_id, size_t suite_id_len,
                        const char *label, size_t label_len,
                        const unsigned char *info, size_t info_len)
{
    size_t         n;
    unsigned char *p;

    if (L > 0xffffU || suite_id_len > SIZE_MAX - (sizeof hpke_version_label + 2U) ||
        label_len > SIZE_MAX - (sizeof hpke_version_label + 2U + suite_id_len) ||
        info_len > SIZE_MAX - (sizeof hpke_version_label + 2U + suite_id_len +
                               label_len)) {
        return -1;
    }
    n = 2U + sizeof hpke_version_label + suite_id_len + label_len + info_len;
    if ((p = (unsigned char *) malloc(n)) == NULL) {
        return -1;
    }
    n = 0U;
    p[n++] = (unsigned char) (L >> 8);
    p[n++] = (unsigned char) L;
    memcpy(&p[n], hpke_version_label, sizeof hpke_version_label);
    n += sizeof hpke_version_label;
    memcpy(&p[n], suite_id, suite_id_len);
    n += suite_id_len;
    memcpy(&p[n], label, label_len);
    n += label_len;
    if (info_len != 0U) {
        memcpy(&p[n], info, info_len);
        n += info_len;
    }
    *out     = p;
    *out_len = n;

    return 0;
}

/*
 * LabeledExpand(prk, label, info, L) from RFC 9180 Section 4, including
 * the HKDF 255-block output bound (255 * Nh).
 */
static int
hpke_labeled_expand(unsigned char *out, size_t L,
                    const unsigned char *suite_id, size_t suite_id_len,
                    const unsigned char prk[HPKE_NH],
                    const char *label, size_t label_len,
                    const unsigned char *info, size_t info_len)
{
    unsigned char *labeled_info;
    size_t         labeled_info_len;
    int            ret;

    if (L > crypto_kdf_hkdf_sha256_BYTES_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (L == 0U) {
        return 0;
    }
    if (hpke_build_labeled_info(&labeled_info, &labeled_info_len, L,
                                suite_id, suite_id_len, label, label_len,
                                info, info_len) != 0) {
        return -1;
    }
    ret = crypto_kdf_hkdf_sha256_expand(out, L,
                                        (const char *) labeled_info,
                                        labeled_info_len, prk);
    sodium_memzero(labeled_info, labeled_info_len);
    free(labeled_info);

    return ret;
}

/* X25519 DH with the RFC 9180 all-zero output check. */
static int
hpke_x25519_dh(unsigned char shared[HPKE_NDH],
               const unsigned char sk[HPKE_NSK],
               const unsigned char pk[HPKE_NENC])
{
    if (crypto_scalarmult_curve25519(shared, sk, pk) != 0) {
        return -1;
    }
    /* The primitive already rejects small-order public keys; the
     * all-zero shared secret check is required by RFC 9180 7.1.4. */
    if (sodium_is_zero(shared, HPKE_NDH)) {
        return -1;
    }
    return 0;
}

/*
 * DeriveKeyPair(ikm) for X25519, RFC 9180 Section 7.1.3:
 *   dkp_prk = LabeledExtract("", "dkp_prk", ikm)
 *   sk      = LabeledExpand(dkp_prk, "sk", "", Nsk)
 * The scalar is clamped as required by SerializePrivateKey(); ikm is
 * never interpreted as an X25519 private key.
 */
static int
hpke_derive_key_pair(unsigned char sk[HPKE_NSK],
                     unsigned char pk[HPKE_NENC],
                     const unsigned char *ikm, size_t ikm_len)
{
    unsigned char dkp_prk[HPKE_NH];

    if (hpke_labeled_extract(dkp_prk, kem_suite_id, sizeof kem_suite_id,
                             NULL, 0U, "dkp_prk", 7U,
                             ikm, ikm_len) != 0) {
        return -1;
    }
    if (hpke_labeled_expand(sk, HPKE_NSK, kem_suite_id,
                            sizeof kem_suite_id, dkp_prk,
                            "sk", 2U, NULL, 0U) != 0) {
        sodium_memzero(dkp_prk, sizeof dkp_prk);
        return -1;
    }
    sodium_memzero(dkp_prk, sizeof dkp_prk);

    sk[0]  &= 248;
    sk[31] &= 127;
    sk[31] |= 64;

    return crypto_scalarmult_curve25519_base(pk, sk);
}

/* ExtractAndExpand(dh, kem_context), RFC 9180 Section 4.1. */
static int
hpke_extract_and_expand(unsigned char shared_secret[HPKE_NH],
                        const unsigned char *dh, size_t dh_len,
                        const unsigned char *kem_context,
                        size_t kem_context_len)
{
    unsigned char eae_prk[HPKE_NH];

    if (hpke_labeled_extract(eae_prk, kem_suite_id, sizeof kem_suite_id,
                             NULL, 0U, "eae_prk", 7U,
                             dh, dh_len) != 0) {
        return -1;
    }
    if (hpke_labeled_expand(shared_secret, HPKE_NH, kem_suite_id,
                            sizeof kem_suite_id, eae_prk,
                            "shared_secret", 13U,
                            kem_context, kem_context_len) != 0) {
        sodium_memzero(eae_prk, sizeof eae_prk);
        return -1;
    }
    sodium_memzero(eae_prk, sizeof eae_prk);

    return 0;
}

/*
 * KeySchedule<ROLE>(mode, shared_secret, info, "", "") from
 * RFC 9180 Section 5.1, base and auth modes only (default empty PSK).
 * Writes into caller-owned stack buffers so no context object exists
 * until every derivation step has succeeded.
 */
static int
hpke_key_schedule(unsigned char key[HPKE_NK],
                  unsigned char base_nonce[HPKE_NN],
                  unsigned char exporter_secret[HPKE_NH],
                  unsigned char mode,
                  const unsigned char shared_secret[HPKE_NH],
                  const unsigned char *info, size_t info_len)
{
    unsigned char psk_id_hash[HPKE_NH];
    unsigned char info_hash[HPKE_NH];
    unsigned char secret[HPKE_NH];
    unsigned char key_schedule_context[1U + 2U * HPKE_NH];
    int           ret = -1;

    if (hpke_labeled_extract(psk_id_hash, hpke_suite_id,
                             sizeof hpke_suite_id,
                             NULL, 0U, "psk_id_hash", 11U,
                             NULL, 0U) != 0) {
        return -1;
    }
    if (hpke_labeled_extract(info_hash, hpke_suite_id,
                             sizeof hpke_suite_id,
                             NULL, 0U, "info_hash", 9U,
                             info, info_len) != 0) {
        goto cleanup;
    }
    key_schedule_context[0] = mode;
    memcpy(&key_schedule_context[1], psk_id_hash, HPKE_NH);
    memcpy(&key_schedule_context[1 + HPKE_NH], info_hash, HPKE_NH);

    if (hpke_labeled_extract(secret, hpke_suite_id, sizeof hpke_suite_id,
                             shared_secret, HPKE_NH, "secret", 6U,
                             NULL, 0U) != 0) {
        goto cleanup;
    }
    if (hpke_labeled_expand(key, HPKE_NK, hpke_suite_id,
                            sizeof hpke_suite_id, secret,
                            "key", 3U, key_schedule_context,
                            sizeof key_schedule_context) != 0 ||
        hpke_labeled_expand(base_nonce, HPKE_NN, hpke_suite_id,
                            sizeof hpke_suite_id, secret,
                            "base_nonce", 10U, key_schedule_context,
                            sizeof key_schedule_context) != 0 ||
        hpke_labeled_expand(exporter_secret, HPKE_NH, hpke_suite_id,
                            sizeof hpke_suite_id, secret,
                            "exp", 3U, key_schedule_context,
                            sizeof key_schedule_context) != 0) {
        goto cleanup;
    }
    ret = 0;

cleanup:
    sodium_memzero(psk_id_hash, sizeof psk_id_hash);
    sodium_memzero(info_hash, sizeof info_hash);
    sodium_memzero(secret, sizeof secret);
    sodium_memzero(key_schedule_context, sizeof key_schedule_context);

    return ret;
}

static int
hpke_info_args_ok(const unsigned char *p, size_t len)
{
    return p != NULL || len == 0U;
}

int
crypto_hpke_sender_setup(crypto_hpke_ctx **out, unsigned char enc[HPKE_NENC],
                         const unsigned char pkR[HPKE_NENC],
                         const unsigned char *skS,
                         const unsigned char *info, size_t info_len,
                         const unsigned char *ikmE, size_t ikmE_len)
{
    unsigned char  skE[HPKE_NSK];
    unsigned char  pkE[HPKE_NENC];
    unsigned char  pkS[HPKE_NENC];
    unsigned char  dh[2U * HPKE_NDH];
    unsigned char  kem_context[3U * HPKE_NENC];
    unsigned char  shared_secret[HPKE_NH];
    unsigned char  random_ikm[HPKE_NSK];
    unsigned char  key[HPKE_NK];
    unsigned char  base_nonce[HPKE_NN];
    unsigned char  exporter_secret[HPKE_NH];
    const unsigned char *ikm;
    size_t         ikm_len;
    size_t         dh_len;
    size_t         kem_context_len;
    unsigned char  mode;
    crypto_hpke_ctx *ctx;
    int            ret = -1;

    if (out == NULL || enc == NULL || pkR == NULL) {
        return -1;
    }
    *out = NULL;
    if (!hpke_info_args_ok(info, info_len) ||
        !hpke_info_args_ok(ikmE, ikmE_len)) {
        return -1;
    }

    if (ikmE == NULL && ikmE_len == 0U) {
        /* GenerateKeyPair() = DeriveKeyPair(random(Nsk)). */
        randombytes_buf(random_ikm, sizeof random_ikm);
        ikm     = random_ikm;
        ikm_len = sizeof random_ikm;
    } else {
        ikm     = ikmE;
        ikm_len = ikmE_len;
    }

    if (hpke_derive_key_pair(skE, pkE, ikm, ikm_len) != 0) {
        goto cleanup;
    }
    memcpy(enc, pkE, HPKE_NENC);

    if (skS == NULL) {
        mode = HPKE_MODE_BASE;
        if (hpke_x25519_dh(dh, skE, pkR) != 0) {
            goto cleanup;
        }
        dh_len = HPKE_NDH;
        memcpy(kem_context, enc, HPKE_NENC);
        memcpy(kem_context + HPKE_NENC, pkR, HPKE_NENC);
        kem_context_len = 2U * HPKE_NENC;
    } else {
        mode = HPKE_MODE_AUTH;
        if (crypto_scalarmult_curve25519_base(pkS, skS) != 0 ||
            hpke_x25519_dh(dh, skE, pkR) != 0 ||
            hpke_x25519_dh(dh + HPKE_NDH, skS, pkR) != 0) {
            goto cleanup;
        }
        dh_len = 2U * HPKE_NDH;
        memcpy(kem_context, enc, HPKE_NENC);
        memcpy(kem_context + HPKE_NENC, pkR, HPKE_NENC);
        memcpy(kem_context + 2U * HPKE_NENC, pkS, HPKE_NENC);
        kem_context_len = 3U * HPKE_NENC;
    }

    if (hpke_extract_and_expand(shared_secret, dh, dh_len,
                                kem_context, kem_context_len) != 0) {
        goto cleanup;
    }
    if (hpke_key_schedule(key, base_nonce, exporter_secret, mode,
                          shared_secret, info, info_len) != 0) {
        goto cleanup;
    }

    /* All cryptographic steps succeeded: only now publish a context. */
    if ((ctx = (crypto_hpke_ctx *) malloc(sizeof *ctx)) == NULL) {
        goto cleanup;
    }
    memcpy(ctx->key, key, sizeof ctx->key);
    memcpy(ctx->base_nonce, base_nonce, sizeof ctx->base_nonce);
    memcpy(ctx->exporter_secret, exporter_secret, sizeof ctx->exporter_secret);
    ctx->seq  = 0U;
    ctx->role = HPKE_ROLE_SENDER;
    *out      = ctx;
    ret       = 0;

cleanup:
    sodium_memzero(skE, sizeof skE);
    sodium_memzero(pkE, sizeof pkE);
    sodium_memzero(pkS, sizeof pkS);
    sodium_memzero(dh, sizeof dh);
    sodium_memzero(kem_context, sizeof kem_context);
    sodium_memzero(shared_secret, sizeof shared_secret);
    sodium_memzero(random_ikm, sizeof random_ikm);
    sodium_memzero(key, sizeof key);
    sodium_memzero(base_nonce, sizeof base_nonce);
    sodium_memzero(exporter_secret, sizeof exporter_secret);

    return ret;
}

int
crypto_hpke_receiver_setup(crypto_hpke_ctx **out,
                           const unsigned char enc[HPKE_NENC],
                           const unsigned char skR[HPKE_NSK],
                           const unsigned char *pkS,
                           const unsigned char *info, size_t info_len)
{
    unsigned char  pkR[HPKE_NENC];
    unsigned char  dh[2U * HPKE_NDH];
    unsigned char  kem_context[3U * HPKE_NENC];
    unsigned char  shared_secret[HPKE_NH];
    unsigned char  key[HPKE_NK];
    unsigned char  base_nonce[HPKE_NN];
    unsigned char  exporter_secret[HPKE_NH];
    size_t         dh_len;
    size_t         kem_context_len;
    unsigned char  mode;
    crypto_hpke_ctx *ctx;
    int            ret = -1;

    if (out == NULL || enc == NULL || skR == NULL) {
        return -1;
    }
    *out = NULL;
    if (!hpke_info_args_ok(info, info_len)) {
        return -1;
    }

    if (crypto_scalarmult_curve25519_base(pkR, skR) != 0) {
        goto cleanup;
    }

    if (pkS == NULL) {
        mode = HPKE_MODE_BASE;
        if (hpke_x25519_dh(dh, skR, enc) != 0) {
            goto cleanup;
        }
        dh_len = HPKE_NDH;
        memcpy(kem_context, enc, HPKE_NENC);
        memcpy(kem_context + HPKE_NENC, pkR, HPKE_NENC);
        kem_context_len = 2U * HPKE_NENC;
    } else {
        mode = HPKE_MODE_AUTH;
        if (hpke_x25519_dh(dh, skR, enc) != 0 ||
            hpke_x25519_dh(dh + HPKE_NDH, skR, pkS) != 0) {
            goto cleanup;
        }
        dh_len = 2U * HPKE_NDH;
        memcpy(kem_context, enc, HPKE_NENC);
        memcpy(kem_context + HPKE_NENC, pkR, HPKE_NENC);
        memcpy(kem_context + 2U * HPKE_NENC, pkS, HPKE_NENC);
        kem_context_len = 3U * HPKE_NENC;
    }

    if (hpke_extract_and_expand(shared_secret, dh, dh_len,
                                kem_context, kem_context_len) != 0) {
        goto cleanup;
    }
    if (hpke_key_schedule(key, base_nonce, exporter_secret, mode,
                          shared_secret, info, info_len) != 0) {
        goto cleanup;
    }

    if ((ctx = (crypto_hpke_ctx *) malloc(sizeof *ctx)) == NULL) {
        goto cleanup;
    }
    memcpy(ctx->key, key, sizeof ctx->key);
    memcpy(ctx->base_nonce, base_nonce, sizeof ctx->base_nonce);
    memcpy(ctx->exporter_secret, exporter_secret, sizeof ctx->exporter_secret);
    ctx->seq  = 0U;
    ctx->role = HPKE_ROLE_RECEIVER;
    *out      = ctx;
    ret       = 0;

cleanup:
    sodium_memzero(pkR, sizeof pkR);
    sodium_memzero(dh, sizeof dh);
    sodium_memzero(kem_context, sizeof kem_context);
    sodium_memzero(shared_secret, sizeof shared_secret);
    sodium_memzero(key, sizeof key);
    sodium_memzero(base_nonce, sizeof base_nonce);
    sodium_memzero(exporter_secret, sizeof exporter_secret);

    return ret;
}

/* nonce = base_nonce XOR I2OSP(seq, Nn) */
static void
hpke_compute_nonce(unsigned char nonce[HPKE_NN],
                   const unsigned char base_nonce[HPKE_NN], uint64_t seq)
{
    unsigned char seq_bytes[HPKE_NN];
    size_t        i;

    memset(seq_bytes, 0, sizeof seq_bytes);
    for (i = 0U; i < 8U; i++) {
        seq_bytes[HPKE_NN - 1U - i] =
            (unsigned char) (seq >> (8U * (unsigned int) i));
    }
    for (i = 0U; i < HPKE_NN; i++) {
        nonce[i] = base_nonce[i] ^ seq_bytes[i];
    }
}

/*
 * The RFC ceiling is 2^96 - 1 messages.  The sequence counter is a
 * uint64_t, so refuse once it can no longer be incremented rather
 * than wrapping back to a reused nonce.
 */
static int
hpke_seq_available(const crypto_hpke_ctx *ctx)
{
    return ctx->seq != UINT64_MAX ? 0 : -1;
}

int
crypto_hpke_seal(crypto_hpke_ctx *ctx, unsigned char *c,
                 unsigned long long *clen, const unsigned char *m,
                 unsigned long long mlen, const unsigned char *aad,
                 unsigned long long aadlen)
{
    unsigned char nonce[HPKE_NN];

    if (clen != NULL) {
        *clen = 0ULL;
    }
    if (ctx == NULL || c == NULL || ctx->role != HPKE_ROLE_SENDER) {
        return -1;
    }
    if (m == NULL && mlen != 0ULL) {
        return -1;
    }
    if (aad == NULL && aadlen != 0ULL) {
        return -1;
    }
    if (mlen > (unsigned long long)
        crypto_aead_chacha20poly1305_ietf_MESSAGEBYTES_MAX) {
        return -1;
    }
    if (hpke_seq_available(ctx) != 0) {
        return -1;
    }
    hpke_compute_nonce(nonce, ctx->base_nonce, ctx->seq);
    if (crypto_aead_chacha20poly1305_ietf_encrypt
            (c, clen, m, mlen, aad, aadlen, NULL, nonce, ctx->key) != 0) {
        return -1;
    }
    ctx->seq++;

    return 0;
}

int
crypto_hpke_open(crypto_hpke_ctx *ctx, unsigned char *m,
                 unsigned long long *mlen, const unsigned char *c,
                 unsigned long long clen, const unsigned char *aad,
                 unsigned long long aadlen)
{
    unsigned char nonce[HPKE_NN];

    if (mlen != NULL) {
        *mlen = 0ULL;
    }
    if (ctx == NULL || m == NULL || c == NULL ||
        ctx->role != HPKE_ROLE_RECEIVER) {
        return -1;
    }
    if (clen < (unsigned long long) HPKE_NT) {
        return -1;
    }
    if (aad == NULL && aadlen != 0ULL) {
        return -1;
    }
    if (hpke_seq_available(ctx) != 0) {
        return -1;
    }
    hpke_compute_nonce(nonce, ctx->base_nonce, ctx->seq);
    if (crypto_aead_chacha20poly1305_ietf_decrypt
            (m, mlen, NULL, c, clen, aad, aadlen, nonce, ctx->key) != 0) {
        /* Authentication failure: do not advance the sequence number. */
        if (mlen != NULL) {
            *mlen = 0ULL;
        }
        return -1;
    }
    ctx->seq++;

    return 0;
}

int
crypto_hpke_export(const crypto_hpke_ctx *ctx, unsigned char *out,
                   size_t outlen, const unsigned char *exporter_context,
                   size_t contextlen)
{
    if (ctx == NULL || (out == NULL && outlen != 0U) ||
        (exporter_context == NULL && contextlen != 0U)) {
        return -1;
    }
    if (outlen > crypto_hpke_EXPORT_BYTES_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (outlen == 0U) {
        return 0;
    }
    /* LabeledExpand(exporter_secret, "sec", exporter_context, L). */
    return hpke_labeled_expand(out, outlen, hpke_suite_id,
                               sizeof hpke_suite_id, ctx->exporter_secret,
                               "sec", 3U, exporter_context, contextlen);
}

void
crypto_hpke_free(crypto_hpke_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    sodium_memzero(ctx, sizeof *ctx);
    free(ctx);
}

/*
 * Test-only visibility hook, intentionally kept out of the installed
 * public header crypto_hpke.h so application code has no sequence
 * state interface.  The underscore prefix marks it as a private
 * library symbol; the contract tests reach it through the
 * non-installed private/hpke_testonly.h header to exercise the RFC
 * message limit without sealing 2^64 messages.
 */
void
_crypto_hpke_testonly_set_sequence(crypto_hpke_ctx *ctx, uint64_t seq)
{
    if (ctx != NULL) {
        ctx->seq = seq;
    }
}
