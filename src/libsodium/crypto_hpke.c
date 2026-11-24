#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "crypto_aead_chacha20poly1305.h"
#include "crypto_auth_hmacsha256.h"
#include "crypto_hpke.h"
#include "crypto_kdf_hkdf_sha256.h"
#include "crypto_scalarmult_curve25519.h"
#include "randombytes.h"
#include "utils.h"

#include "crypto_hpke_local.h"

#define HPKE_KEM_ID       0x0020U
#define HPKE_KDF_ID       0x0001U
#define HPKE_AEAD_ID      0x0003U

#define HPKE_NH           32U
#define HPKE_NK           32U
#define HPKE_NN           12U
#define HPKE_NT           16U
#define HPKE_ENCBYTES     32U
#define HPKE_PUBLICKEYBYTES 32U
#define HPKE_SECRETKEYBYTES 32U

#define HPKE_ROLE_SENDER   1U
#define HPKE_ROLE_RECEIVER 2U
#define HPKE_MODE_BASE     0U
#define HPKE_MODE_AUTH     2U

/* RFC 9180 Section 7.2.1 application input limits for HKDF-SHA256. */
#define HPKE_INFO_MAX             ((UINT64_C(1) << 61) - UINT64_C(91))
#define HPKE_IKM_MAX              ((UINT64_C(1) << 61) - UINT64_C(84))
#define HPKE_EXPORT_CONTEXT_MAX   ((UINT64_C(1) << 61) - UINT64_C(120))

static const unsigned char hpke_v1[7] = {
    'H', 'P', 'K', 'E', '-', 'v', '1'
};

static const unsigned char kem_suite_id[5] = {
    'K', 'E', 'M',
    (unsigned char) (HPKE_KEM_ID >> 8),
    (unsigned char) (HPKE_KEM_ID & 0xff)
};

static const unsigned char hpke_suite_id[10] = {
    'H', 'P', 'K', 'E',
    (unsigned char) (HPKE_KEM_ID >> 8),
    (unsigned char) (HPKE_KEM_ID & 0xff),
    (unsigned char) (HPKE_KDF_ID >> 8),
    (unsigned char) (HPKE_KDF_ID & 0xff),
    (unsigned char) (HPKE_AEAD_ID >> 8),
    (unsigned char) (HPKE_AEAD_ID & 0xff)
};

static const unsigned char empty_bytes[1] = { 0 };

static int
length_allowed(size_t len, uint64_t maximum)
{
    return (uint64_t) len <= maximum;
}

/*
 * RFC 9180 LabeledExtract. The HKDF salt is permitted to be zero-length;
 * empty_bytes gives HMAC a valid non-NULL key pointer in that case.
 */
static int
labeled_extract(const unsigned char *suite_id, size_t suite_id_len,
                const unsigned char *salt, size_t salt_len,
                const char *label, size_t label_len,
                const unsigned char *ikm, size_t ikm_len,
                unsigned char prk[HPKE_NH])
{
    crypto_kdf_hkdf_sha256_state state;

    if ((salt == NULL && salt_len != 0U) ||
        (ikm == NULL && ikm_len != 0U) ||
        label == NULL) {
        return -1;
    }
    if (salt == NULL) {
        salt = empty_bytes;
    }
    if (crypto_kdf_hkdf_sha256_extract_init(&state, salt, salt_len) != 0) {
        return -1;
    }

    crypto_kdf_hkdf_sha256_extract_update(&state, hpke_v1, sizeof hpke_v1);
    crypto_kdf_hkdf_sha256_extract_update(&state, suite_id, suite_id_len);
    crypto_kdf_hkdf_sha256_extract_update(&state,
                                          (const unsigned char *) label,
                                          label_len);
    if (ikm_len != 0U) {
        crypto_kdf_hkdf_sha256_extract_update(&state, ikm, ikm_len);
    }
    crypto_kdf_hkdf_sha256_extract_final(&state, prk);
    sodium_memzero(&state, sizeof state);

    return 0;
}

/*
 * RFC 9180 LabeledExpand, with HKDF-Expand block generation streamed through
 * HMAC-SHA256. Enforcing the 255-block limit also keeps the two-byte L
 * encoding valid.
 */
static int
labeled_expand(const unsigned char *suite_id, size_t suite_id_len,
               const unsigned char prk[HPKE_NH],
               const char *label, size_t label_len,
               const unsigned char *info, size_t info_len,
               unsigned char *out, size_t out_len)
{
    crypto_auth_hmacsha256_state state;
    unsigned char                t[HPKE_NH];
    unsigned char                length_bytes[2];
    unsigned char                counter;
    size_t                       done = 0;
    size_t                       take;

    if (prk == NULL || label == NULL ||
        (info == NULL && info_len != 0U) ||
        (out == NULL && out_len != 0U) ||
        out_len > crypto_hpke_EXPORT_BYTES_MAX) {
        return -1;
    }

    length_bytes[0] = (unsigned char) ((out_len >> 8) & 0xffU);
    length_bytes[1] = (unsigned char) (out_len & 0xffU);
    counter = 1U;

    while (done < out_len) {
        if (crypto_auth_hmacsha256_init(&state, prk, HPKE_NH) != 0) {
            sodium_memzero(t, sizeof t);
            return -1;
        }
        if (counter != 1U) {
            crypto_auth_hmacsha256_update(&state, t, sizeof t);
        }
        crypto_auth_hmacsha256_update(&state, length_bytes,
                                      sizeof length_bytes);
        crypto_auth_hmacsha256_update(&state, hpke_v1, sizeof hpke_v1);
        crypto_auth_hmacsha256_update(&state, suite_id, suite_id_len);
        crypto_auth_hmacsha256_update(&state,
                                      (const unsigned char *) label,
                                      label_len);
        if (info_len != 0U) {
            crypto_auth_hmacsha256_update(&state, info, info_len);
        }
        crypto_auth_hmacsha256_update(&state, &counter, 1U);
        crypto_auth_hmacsha256_final(&state, t);
        sodium_memzero(&state, sizeof state);

        take = out_len - done;
        if (take > HPKE_NH) {
            take = HPKE_NH;
        }
        memcpy(out + done, t, take);
        done += take;
        counter++;
    }

    sodium_memzero(t, sizeof t);
    return 0;
}

static int
x25519_dh(unsigned char shared[32],
          const unsigned char sk[32], const unsigned char pk[32])
{
    if (crypto_scalarmult_curve25519(shared, sk, pk) != 0) {
        return -1;
    }
    /* RFC 9180 Section 7.1.4: abort on an all-zero X25519 output. */
    if (sodium_is_zero(shared, 32)) {
        return -1;
    }
    return 0;
}

static int
sequence_is_at_limit(const unsigned char seq[HPKE_NN])
{
    size_t i;

    for (i = 0U; i < HPKE_NN; i++) {
        if (seq[i] != 0xffU) {
            return 0;
        }
    }
    return 1;
}

static void
sequence_increment(unsigned char seq[HPKE_NN])
{
    size_t i = HPKE_NN;

    /* The caller rejects the all-0xff limit before reaching this point. */
    while (i-- > 0U) {
        if (seq[i] != 0xffU) {
            seq[i]++;
            return;
        }
        seq[i] = 0x00U;
    }
}

static void
compute_nonce(unsigned char nonce[HPKE_NN],
              const struct crypto_hpke_ctx *ctx)
{
    size_t i;

    for (i = 0U; i < HPKE_NN; i++) {
        nonce[i] = ctx->base_nonce[i] ^ ctx->seq[i];
    }
}

static int
key_schedule(struct crypto_hpke_ctx *ctx, unsigned char mode,
             const unsigned char shared_secret[HPKE_NH],
             const unsigned char *info, size_t info_len)
{
    unsigned char psk_id_hash[HPKE_NH];
    unsigned char info_hash[HPKE_NH];
    unsigned char secret[HPKE_NH];
    unsigned char schedule_context[1U + HPKE_NH + HPKE_NH];

    if (info_len != 0U && info == NULL) {
        return -1;
    }
    if (!length_allowed(info_len, HPKE_INFO_MAX)) {
        return -1;
    }

    if (labeled_extract(hpke_suite_id, sizeof hpke_suite_id,
                         NULL, 0U, "psk_id_hash",
                         sizeof "psk_id_hash" - 1U,
                         NULL, 0U, psk_id_hash) != 0 ||
        labeled_extract(hpke_suite_id, sizeof hpke_suite_id,
                        NULL, 0U, "info_hash",
                        sizeof "info_hash" - 1U,
                        info, info_len, info_hash) != 0) {
        sodium_memzero(psk_id_hash, sizeof psk_id_hash);
        sodium_memzero(info_hash, sizeof info_hash);
        return -1;
    }

    schedule_context[0] = mode;
    memcpy(schedule_context + 1U, psk_id_hash, HPKE_NH);
    memcpy(schedule_context + 1U + HPKE_NH, info_hash, HPKE_NH);

    if (labeled_extract(hpke_suite_id, sizeof hpke_suite_id,
                        shared_secret, HPKE_NH, "secret",
                        sizeof "secret" - 1U,
                        NULL, 0U, secret) != 0 ||
        labeled_expand(hpke_suite_id, sizeof hpke_suite_id,
                       secret, "key", sizeof "key" - 1U,
                       schedule_context, sizeof schedule_context,
                       ctx->key, HPKE_NK) != 0 ||
        labeled_expand(hpke_suite_id, sizeof hpke_suite_id,
                       secret, "base_nonce",
                       sizeof "base_nonce" - 1U,
                       schedule_context, sizeof schedule_context,
                       ctx->base_nonce, HPKE_NN) != 0 ||
        labeled_expand(hpke_suite_id, sizeof hpke_suite_id,
                       secret, "exp", sizeof "exp" - 1U,
                       schedule_context, sizeof schedule_context,
                       ctx->exporter_secret, HPKE_NH) != 0) {
        sodium_memzero(psk_id_hash, sizeof psk_id_hash);
        sodium_memzero(info_hash, sizeof info_hash);
        sodium_memzero(secret, sizeof secret);
        return -1;
    }

    ctx->mode = mode;
    memset(ctx->seq, 0, sizeof ctx->seq);
    sodium_memzero(psk_id_hash, sizeof psk_id_hash);
    sodium_memzero(info_hash, sizeof info_hash);
    sodium_memzero(secret, sizeof secret);

    return 0;
}

int
crypto_hpke_sender_setup(crypto_hpke_ctx **out,
                         unsigned char enc[HPKE_ENCBYTES],
                         const unsigned char pkR[HPKE_PUBLICKEYBYTES],
                         const unsigned char *skS,
                         const unsigned char *info, size_t info_len,
                         const unsigned char *ikmE, size_t ikmE_len)
{
    struct crypto_hpke_ctx  local;
    crypto_hpke_ctx         *published = NULL;
    unsigned char           ikm_random[32];
    const unsigned char    *ikm;
    unsigned char           dkp_prk[HPKE_NH];
    unsigned char           skE[32];
    unsigned char           enc_local[HPKE_ENCBYTES];
    unsigned char           pkS[HPKE_PUBLICKEYBYTES];
    unsigned char           dh[64];
    unsigned char           eae_prk[HPKE_NH];
    unsigned char           shared_secret[HPKE_NH];
    unsigned char           kem_context[HPKE_ENCBYTES +
                                        HPKE_PUBLICKEYBYTES +
                                        HPKE_PUBLICKEYBYTES];
    size_t                  dh_len;
    size_t                  kem_context_len;
    int                     auth;
    int                     ret = -1;

    if (out == NULL) {
        return -1;
    }
    *out = NULL;

    if (sodium_init() < 0 || enc == NULL || pkR == NULL) {
        return -1;
    }
    if ((info == NULL && info_len != 0U) ||
        !length_allowed(info_len, HPKE_INFO_MAX)) {
        return -1;
    }

    auth = (skS != NULL);
    if (ikmE == NULL) {
        if (ikmE_len != 0U) {
            return -1;
        }
        randombytes_buf(ikm_random, sizeof ikm_random);
        ikm = ikm_random;
        ikmE_len = sizeof ikm_random;
    } else {
        if (!length_allowed(ikmE_len, HPKE_IKM_MAX)) {
            return -1;
        }
        ikm = ikmE;
    }

    memset(&local, 0, sizeof local);

    /* DHKEM(X25519, HKDF-SHA256) DeriveKeyPair(), not raw IKM as skE. */
    if (labeled_extract(kem_suite_id, sizeof kem_suite_id,
                        NULL, 0U, "dkp_prk", sizeof "dkp_prk" - 1U,
                        ikm, ikmE_len, dkp_prk) != 0 ||
        labeled_expand(kem_suite_id, sizeof kem_suite_id,
                       dkp_prk, "sk", sizeof "sk" - 1U,
                       NULL, 0U, skE, sizeof skE) != 0 ||
        crypto_scalarmult_curve25519_base(enc_local, skE) != 0) {
        goto cleanup;
    }

    if (x25519_dh(dh, skE, pkR) != 0) {
        goto cleanup;
    }
    dh_len = 32U;

    if (auth) {
        if (crypto_scalarmult_curve25519_base(pkS, skS) != 0 ||
            x25519_dh(dh + 32U, skS, pkR) != 0) {
            goto cleanup;
        }
        dh_len = 64U;
    }

    /* kem_context order: enc || pkR || pkS (pkS only in auth mode). */
    memcpy(kem_context, enc_local, HPKE_ENCBYTES);
    memcpy(kem_context + HPKE_ENCBYTES, pkR, HPKE_PUBLICKEYBYTES);
    kem_context_len = HPKE_ENCBYTES + HPKE_PUBLICKEYBYTES;
    if (auth) {
        memcpy(kem_context + kem_context_len, pkS, HPKE_PUBLICKEYBYTES);
        kem_context_len += HPKE_PUBLICKEYBYTES;
    }

    if (labeled_extract(kem_suite_id, sizeof kem_suite_id,
                        NULL, 0U, "eae_prk", sizeof "eae_prk" - 1U,
                        dh, dh_len, eae_prk) != 0 ||
        labeled_expand(kem_suite_id, sizeof kem_suite_id,
                       eae_prk, "shared_secret",
                       sizeof "shared_secret" - 1U,
                       kem_context, kem_context_len,
                       shared_secret, HPKE_NH) != 0) {
        goto cleanup;
    }

    local.role = HPKE_ROLE_SENDER;
    if (key_schedule(&local,
                     (unsigned char) (auth ? HPKE_MODE_AUTH : HPKE_MODE_BASE),
                     shared_secret, info, info_len) != 0) {
        goto cleanup;
    }

    /* Publish only after every validation and derivation has succeeded. */
    published = (crypto_hpke_ctx *) malloc(sizeof *published);
    if (published == NULL) {
        goto cleanup;
    }
    *published = local;
    *out = published;
    memcpy(enc, enc_local, HPKE_ENCBYTES);
    ret = 0;

cleanup:
    sodium_memzero(ikm_random, sizeof ikm_random);
    sodium_memzero(dkp_prk, sizeof dkp_prk);
    sodium_memzero(skE, sizeof skE);
    sodium_memzero(dh, sizeof dh);
    sodium_memzero(eae_prk, sizeof eae_prk);
    sodium_memzero(shared_secret, sizeof shared_secret);
    sodium_memzero(&local, sizeof local);

    return ret;
}

int
crypto_hpke_receiver_setup(crypto_hpke_ctx **out,
                            const unsigned char enc[HPKE_ENCBYTES],
                            const unsigned char skR[HPKE_SECRETKEYBYTES],
                            const unsigned char *pkS,
                            const unsigned char *info, size_t info_len)
{
    struct crypto_hpke_ctx  local;
    crypto_hpke_ctx         *published = NULL;
    unsigned char           pkR_local[HPKE_PUBLICKEYBYTES];
    unsigned char           dh[64];
    unsigned char           eae_prk[HPKE_NH];
    unsigned char           shared_secret[HPKE_NH];
    unsigned char           kem_context[HPKE_ENCBYTES +
                                        HPKE_PUBLICKEYBYTES +
                                        HPKE_PUBLICKEYBYTES];
    size_t                  dh_len;
    size_t                  kem_context_len;
    int                     auth;
    int                     ret = -1;

    if (out == NULL) {
        return -1;
    }
    *out = NULL;

    if (sodium_init() < 0 || enc == NULL || skR == NULL) {
        return -1;
    }
    if ((info == NULL && info_len != 0U) ||
        !length_allowed(info_len, HPKE_INFO_MAX)) {
        return -1;
    }

    auth = (pkS != NULL);
    memset(&local, 0, sizeof local);

    if (crypto_scalarmult_curve25519_base(pkR_local, skR) != 0) {
        goto cleanup;
    }

    if (x25519_dh(dh, skR, enc) != 0) {
        goto cleanup;
    }
    dh_len = 32U;

    if (auth) {
        if (x25519_dh(dh + 32U, skR, pkS) != 0) {
            goto cleanup;
        }
        dh_len = 64U;
    }

    /* AuthDecap DH concatenation order: DH(skR,pkE) || DH(skR,pkS). */
    memcpy(kem_context, enc, HPKE_ENCBYTES);
    memcpy(kem_context + HPKE_ENCBYTES, pkR_local,
           HPKE_PUBLICKEYBYTES);
    kem_context_len = HPKE_ENCBYTES + HPKE_PUBLICKEYBYTES;
    if (auth) {
        memcpy(kem_context + kem_context_len, pkS,
               HPKE_PUBLICKEYBYTES);
        kem_context_len += HPKE_PUBLICKEYBYTES;
    }

    if (labeled_extract(kem_suite_id, sizeof kem_suite_id,
                        NULL, 0U, "eae_prk", sizeof "eae_prk" - 1U,
                        dh, dh_len, eae_prk) != 0 ||
        labeled_expand(kem_suite_id, sizeof kem_suite_id,
                       eae_prk, "shared_secret",
                       sizeof "shared_secret" - 1U,
                       kem_context, kem_context_len,
                       shared_secret, HPKE_NH) != 0) {
        goto cleanup;
    }

    local.role = HPKE_ROLE_RECEIVER;
    if (key_schedule(&local,
                     (unsigned char) (auth ? HPKE_MODE_AUTH : HPKE_MODE_BASE),
                     shared_secret, info, info_len) != 0) {
        goto cleanup;
    }

    published = (crypto_hpke_ctx *) malloc(sizeof *published);
    if (published == NULL) {
        goto cleanup;
    }
    *published = local;
    *out = published;
    ret = 0;

cleanup:
    sodium_memzero(dh, sizeof dh);
    sodium_memzero(eae_prk, sizeof eae_prk);
    sodium_memzero(shared_secret, sizeof shared_secret);
    sodium_memzero(&local, sizeof local);

    return ret;
}

int
crypto_hpke_seal(crypto_hpke_ctx *ctx,
                 unsigned char *c, unsigned long long *clen_p,
                 const unsigned char *m, unsigned long long mlen,
                 const unsigned char *ad, unsigned long long adlen)
{
    unsigned char       nonce[HPKE_NN];
    const unsigned char *m_eff;
    const unsigned char *ad_eff;
    unsigned long long  message_max;

    if (clen_p != NULL) {
        *clen_p = 0ULL;
    }
    if (ctx == NULL || ctx->role != HPKE_ROLE_SENDER) {
        return -1;
    }
    if (c == NULL || (m == NULL && mlen != 0ULL)) {
        return -1;
    }

    message_max =
        (unsigned long long) crypto_aead_chacha20poly1305_ietf_messagebytes_max();
    if (mlen > message_max || mlen > ULLONG_MAX - HPKE_NT) {
        return -1;
    }
    if (sequence_is_at_limit(ctx->seq)) {
        return -1;
    }

    m_eff = (m != NULL) ? m : empty_bytes;
    ad_eff = (ad != NULL || adlen == 0ULL) ?
             (ad != NULL ? ad : empty_bytes) : NULL;
    if (ad_eff == NULL) {
        return -1;
    }

    compute_nonce(nonce, ctx);
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            c, clen_p, m_eff, mlen, ad_eff, adlen,
            NULL, nonce, ctx->key) != 0) {
        if (clen_p != NULL) {
            *clen_p = 0ULL;
        }
        sodium_memzero(nonce, sizeof nonce);
        return -1;
    }

    sequence_increment(ctx->seq);
    sodium_memzero(nonce, sizeof nonce);
    return 0;
}

int
crypto_hpke_open(crypto_hpke_ctx *ctx,
                 unsigned char *m, unsigned long long *mlen_p,
                 const unsigned char *c, unsigned long long clen,
                 const unsigned char *ad, unsigned long long adlen)
{
    unsigned char       nonce[HPKE_NN];
    const unsigned char *ad_eff;
    unsigned long long  message_max;
    unsigned long long  plaintext_len;

    if (mlen_p != NULL) {
        *mlen_p = 0ULL;
    }
    if (ctx == NULL || ctx->role != HPKE_ROLE_RECEIVER) {
        return -1;
    }
    if (c == NULL || clen < HPKE_NT) {
        return -1;
    }

    plaintext_len = clen - HPKE_NT;
    message_max =
        (unsigned long long) crypto_aead_chacha20poly1305_ietf_messagebytes_max();
    if (plaintext_len > message_max) {
        return -1;
    }
    if (m == NULL && plaintext_len != 0ULL) {
        return -1;
    }
    if (sequence_is_at_limit(ctx->seq)) {
        return -1;
    }

    ad_eff = ad;
    if (ad_eff == NULL) {
        if (adlen != 0ULL) {
            return -1;
        }
        ad_eff = empty_bytes;
    }

    compute_nonce(nonce, ctx);
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            m, mlen_p, NULL, c, clen, ad_eff, adlen,
            nonce, ctx->key) != 0) {
        if (mlen_p != NULL) {
            *mlen_p = 0ULL;
        }
        sodium_memzero(nonce, sizeof nonce);
        return -1;
    }

    if (mlen_p != NULL) {
        *mlen_p = plaintext_len;
    }
    sequence_increment(ctx->seq);
    sodium_memzero(nonce, sizeof nonce);
    return 0;
}

int
crypto_hpke_export(const crypto_hpke_ctx *ctx,
                   unsigned char *out, size_t outlen,
                   const unsigned char *exporter_context,
                   size_t exporter_context_len)
{
    if (ctx == NULL) {
        return -1;
    }
    if ((out == NULL && outlen != 0U) ||
        outlen > crypto_hpke_EXPORT_BYTES_MAX) {
        return -1;
    }
    if ((exporter_context == NULL && exporter_context_len != 0U) ||
        !length_allowed(exporter_context_len, HPKE_EXPORT_CONTEXT_MAX)) {
        return -1;
    }

    return labeled_expand(hpke_suite_id, sizeof hpke_suite_id,
                          ctx->exporter_secret, "sec",
                          sizeof "sec" - 1U,
                          exporter_context, exporter_context_len,
                          out, outlen);
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
