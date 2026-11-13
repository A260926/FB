
/*
 * RFC 9180 HPKE contract tests for the single supported ciphersuite
 * DHKEM(X25519, HKDF-SHA256) / HKDF-SHA256 / ChaCha20Poly1305
 * (0x0020 / 0x0001 / 0x0003), base and auth modes.
 *
 * Registered through support/contract.h; this file has no main().
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sodium.h"
#include "private/hpke_testonly.h"

#include "contract.h"
#include "hpke_vectors.h"

#define CTX_TAG 16U
#define SEQ_LAST (UINT64_MAX - 1U)

static int
hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int
hex_decode(unsigned char *out, size_t out_cap, const char *hex, size_t *out_len)
{
    size_t n = strlen(hex);
    size_t i;

    if ((n & 1U) != 0U || n / 2U > out_cap) {
        return -1;
    }
    for (i = 0U; i < n / 2U; i++) {
        int hi = hex_nibble(hex[2U * i]);
        int lo = hex_nibble(hex[2U * i + 1U]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (unsigned char) ((hi << 4) | lo);
    }
    if (out_len != NULL) {
        *out_len = n / 2U;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* vectors: RFC 9180 Appendix A.2 public base/auth data                */
/* ------------------------------------------------------------------ */

static void
group_vectors(void)
{
    unsigned int v;

    if (sodium_init() < 0) {
        UNIT_CHECK(0);
        return;
    }
    for (v = 0U; v < sizeof hpke_vectors / sizeof hpke_vectors[0]; v++) {
        const hpke_vector *tv = &hpke_vectors[v];
        unsigned char ikmE[32], skR[32], pkR[32], skS[32], pkS[32];
        unsigned char info[128], enc[32], enc2[32];
        unsigned char sender_enc[32];
        const unsigned char *skS_arg = NULL;
        const unsigned char *pkS_arg = NULL;
        crypto_hpke_ctx *sctx = NULL;
        crypto_hpke_ctx *rctx = NULL;
        crypto_hpke_ctx *sctx2 = NULL;
        size_t info_len;
        int ret;
        unsigned int i;

        UNIT_CHECK(hex_decode(ikmE, sizeof ikmE, tv->ikmE, NULL) == 0);
        UNIT_CHECK(hex_decode(skR, sizeof skR, tv->skR, NULL) == 0);
        UNIT_CHECK(hex_decode(pkR, sizeof pkR, tv->pkR, NULL) == 0);
        UNIT_CHECK(hex_decode(info, sizeof info, tv->info, &info_len) == 0);
        UNIT_CHECK(hex_decode(enc, sizeof enc, tv->enc, NULL) == 0);

        if (tv->mode == 2) {
            UNIT_CHECK(hex_decode(skS, sizeof skS, tv->skS, NULL) == 0);
            UNIT_CHECK(hex_decode(pkS, sizeof pkS, tv->pkS, NULL) == 0);
            skS_arg = skS;
            pkS_arg = pkS;
        }

        ret = crypto_hpke_sender_setup(&sctx, sender_enc, pkR, skS_arg,
                                       info, info_len, ikmE, sizeof ikmE);
        UNIT_CHECK(ret == 0);
        UNIT_CHECK(sctx != NULL);
        /* Deterministic DeriveKeyPair(ikmE): same ikmE, same enc. */
        UNIT_CHECK(crypto_hpke_sender_setup(&sctx2, enc2, pkR, skS_arg,
                                            info, info_len, ikmE,
                                            sizeof ikmE) == 0);
        UNIT_CHECK(memcmp(enc2, sender_enc, 32U) == 0);
        crypto_hpke_free(sctx2);
        /* The enc must match the RFC vector, not just a self-consistent
         * value produced by this library. */
        UNIT_CHECK(memcmp(sender_enc, enc, 32U) == 0);

        ret = crypto_hpke_receiver_setup(&rctx, sender_enc, skR, pkS_arg,
                                         info, info_len);
        UNIT_CHECK(ret == 0);
        UNIT_CHECK(rctx != NULL);

        for (i = 0U; i < 3U; i++) {
            unsigned char aad[128], pt[256], want_ct[512], ct[512], got[256];
            size_t aad_len, pt_len, want_ct_len;
            unsigned long long clen = 0ULL, mlen = 0ULL;

            UNIT_CHECK(hex_decode(aad, sizeof aad, tv->aad[i], &aad_len) == 0);
            UNIT_CHECK(hex_decode(pt, sizeof pt, tv->pt[i], &pt_len) == 0);
            UNIT_CHECK(hex_decode(want_ct, sizeof want_ct, tv->ct[i],
                                  &want_ct_len) == 0);
            UNIT_CHECK(want_ct_len == pt_len + CTX_TAG);

            UNIT_CHECK(crypto_hpke_seal(sctx, ct, &clen, pt,
                                        (unsigned long long) pt_len, aad,
                                        (unsigned long long) aad_len) == 0);
            UNIT_CHECK((size_t) clen == want_ct_len);
            UNIT_CHECK(memcmp(ct, want_ct, want_ct_len) == 0);

            UNIT_CHECK(crypto_hpke_open(rctx, got, &mlen, want_ct,
                                        (unsigned long long) want_ct_len,
                                        aad,
                                        (unsigned long long) aad_len) == 0);
            UNIT_CHECK((size_t) mlen == pt_len);
            UNIT_CHECK(memcmp(got, pt, pt_len) == 0);
        }

        for (i = 0U; i < 3U; i++) {
            unsigned char ectx[128], want[64], got_s[64], got_r[64];
            size_t ectx_len, want_len;

            UNIT_CHECK(hex_decode(ectx, sizeof ectx,
                                  tv->exporter_context[i], &ectx_len) == 0);
            UNIT_CHECK(hex_decode(want, sizeof want,
                                  tv->exported_value[i], &want_len) == 0);
            UNIT_CHECK(want_len == tv->L[i]);

            UNIT_CHECK(crypto_hpke_export(sctx, got_s, tv->L[i],
                                          ectx, ectx_len) == 0);
            UNIT_CHECK(crypto_hpke_export(rctx, got_r, tv->L[i],
                                          ectx, ectx_len) == 0);
            UNIT_CHECK(memcmp(got_s, want, want_len) == 0);
            UNIT_CHECK(memcmp(got_r, want, want_len) == 0);
        }

        crypto_hpke_free(sctx);
        crypto_hpke_free(rctx);
    }
}

/* ------------------------------------------------------------------ */
/* binding: info, AD, sender identity and recipient identity changes   */
/* ------------------------------------------------------------------ */

static void
group_binding(void)
{
    unsigned char skR1[32], pkR1[32], skR2[32], pkR2[32];
    unsigned char skS[32], pkS[32], other_skS[32], other_pkS[32];
    unsigned char enc[32];
    const unsigned char info_a[4] = { 'i', 'n', 'f', 'o' };
    const unsigned char info_b[4] = { 'i', 'n', 'f', 'O' };
    const unsigned char m[5] = { 'h', 'e', 'l', 'l', 'o' };
    const unsigned char aad1[3] = { 'a', 'a', 'd' };
    const unsigned char aad2[3] = { 'A', 'A', 'D' };
    unsigned char ct[64], out[64], exp_a[32], exp_b[32];
    unsigned long long clen = 0ULL, mlen = 0ULL;
    crypto_hpke_ctx *sa = NULL, *ra = NULL, *rb = NULL;
    crypto_hpke_ctx *sauth = NULL, *rauth = NULL, *rwrong = NULL;
    crypto_hpke_ctx *rbase = NULL;

    randombytes_buf(skR1, sizeof skR1);
    randombytes_buf(skR2, sizeof skR2);
    randombytes_buf(skS, sizeof skS);
    UNIT_CHECK(crypto_scalarmult_curve25519_base(pkR1, skR1) == 0);
    UNIT_CHECK(crypto_scalarmult_curve25519_base(pkR2, skR2) == 0);
    UNIT_CHECK(crypto_scalarmult_curve25519_base(pkS, skS) == 0);
    /* A guaranteed-valid but different static public key. */
    randombytes_buf(other_skS, sizeof other_skS);
    UNIT_CHECK(crypto_scalarmult_curve25519_base(other_pkS,
                                                 other_skS) == 0);
    UNIT_CHECK(memcmp(other_pkS, pkS, 32U) != 0);

    /* info binding: distinct info gives distinct exporter secrets. */
    UNIT_CHECK(crypto_hpke_sender_setup(&sa, enc, pkR1, NULL,
                                        info_a, sizeof info_a,
                                        NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_receiver_setup(&ra, enc, skR1, NULL,
                                          info_a, sizeof info_a) == 0);
    UNIT_CHECK(crypto_hpke_receiver_setup(&rb, enc, skR1, NULL,
                                          info_b, sizeof info_b) == 0);
    UNIT_CHECK(crypto_hpke_export(sa, exp_a, sizeof exp_a, NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_export(rb, exp_b, sizeof exp_b, NULL, 0U) == 0);
    UNIT_CHECK(memcmp(exp_a, exp_b, sizeof exp_a) != 0);

    /* AD binding: wrong aad fails, correct aad opens. */
    UNIT_CHECK(crypto_hpke_seal(sa, ct, &clen, m, sizeof m,
                                aad1, sizeof aad1) == 0);
    UNIT_CHECK(crypto_hpke_open(ra, out, &mlen, ct, clen,
                                aad2, sizeof aad2) == -1);
    UNIT_CHECK(mlen == 0ULL);
    UNIT_CHECK(crypto_hpke_open(ra, out, &mlen, ct, clen,
                                aad1, sizeof aad1) == 0);
    UNIT_CHECK(mlen == sizeof m && memcmp(out, m, sizeof m) == 0);

    /* Recipient identity: a context built for another recipient cannot
     * open the ciphertext. */
    {
        crypto_hpke_ctx *r2 = NULL;
        UNIT_CHECK(crypto_hpke_receiver_setup(&r2, enc, skR2, NULL,
                                              info_a,
                                              sizeof info_a) == 0);
        UNIT_CHECK(crypto_hpke_open(r2, out, &mlen, ct, clen,
                                    aad1, sizeof aad1) == -1);
        UNIT_CHECK(mlen == 0ULL);
        crypto_hpke_free(r2);
    }
    crypto_hpke_free(sa);
    crypto_hpke_free(ra);
    crypto_hpke_free(rb);

    /* Sender identity binding (auth mode): the recipient must pin the
     * sender static public key. */
    UNIT_CHECK(crypto_hpke_sender_setup(&sauth, enc, pkR1, skS,
                                        info_a, sizeof info_a,
                                        NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_receiver_setup(&rauth, enc, skR1, pkS,
                                          info_a, sizeof info_a) == 0);
    UNIT_CHECK(crypto_hpke_seal(sauth, ct, &clen, m, sizeof m,
                                aad1, sizeof aad1) == 0);
    UNIT_CHECK(crypto_hpke_open(rauth, out, &mlen, ct, clen,
                                aad1, sizeof aad1) == 0);
    UNIT_CHECK(mlen == sizeof m && memcmp(out, m, sizeof m) == 0);

    /* Wrong sender public key: setup itself may succeed, but the AEAD
     * tag can never validate. */
    UNIT_CHECK(crypto_hpke_receiver_setup(&rwrong, enc, skR1, other_pkS,
                                          info_a, sizeof info_a) == 0);
    UNIT_CHECK(crypto_hpke_open(rwrong, out, &mlen, ct, clen,
                                aad1, sizeof aad1) == -1);
    UNIT_CHECK(mlen == 0ULL);
    crypto_hpke_free(rwrong);

    /* A base-mode recipient cannot decrypt an auth-mode ciphertext. */
    UNIT_CHECK(crypto_hpke_receiver_setup(&rbase, enc, skR1, NULL,
                                          info_a, sizeof info_a) == 0);
    UNIT_CHECK(crypto_hpke_open(rbase, out, &mlen, ct, clen,
                                aad1, sizeof aad1) == -1);
    UNIT_CHECK(mlen == 0ULL);

    crypto_hpke_free(sauth);
    crypto_hpke_free(rauth);
    crypto_hpke_free(rbase);
}

/* ------------------------------------------------------------------ */
/* contexts: empty inputs, tag damage, roles, limits, independence     */
/* ------------------------------------------------------------------ */

static void
group_contexts(void)
{
    unsigned char skR[32], pkR[32], enc1[32], enc2[32];
    const unsigned char info_nul[3] = { 'a', 0U, 'b' };
    const unsigned char info_nul2[3] = { 'a', 0U, 'c' };
    unsigned char ct[64], pt[32], out[32];
    unsigned char big[255U * 32U + 1U];
    unsigned char big_r[255U * 32U];
    unsigned char e1[32], e2[32];
    unsigned long long clen = 0ULL, mlen = 0ULL;
    crypto_hpke_ctx *s1 = NULL, *r1 = NULL, *s2 = NULL, *r2 = NULL;
    crypto_hpke_ctx *sn = NULL, *rn = NULL;
    static const unsigned char zero32[32];

    randombytes_buf(skR, sizeof skR);
    UNIT_CHECK(crypto_scalarmult_curve25519_base(pkR, skR) == 0);

    /* Empty message and empty AD. */
    UNIT_CHECK(crypto_hpke_sender_setup(&s1, enc1, pkR, NULL,
                                        NULL, 0U, NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_receiver_setup(&r1, enc1, skR, NULL,
                                          NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_seal(s1, ct, &clen, NULL, 0ULL, NULL, 0ULL) == 0);
    UNIT_CHECK(clen == (unsigned long long) CTX_TAG);
    UNIT_CHECK(crypto_hpke_open(r1, out, &mlen, ct, clen, NULL, 0ULL) == 0);
    UNIT_CHECK(mlen == 0ULL);

    /* Corrupted tag: failure returns length 0, keeps the sequence
     * number untouched, and the original ciphertext still opens. */
    {
        unsigned char damaged[64];
        UNIT_CHECK(crypto_hpke_seal(s1, ct, &clen, pt, sizeof pt,
                                    NULL, 0U) == 0);
        memcpy(damaged, ct, (size_t) clen);
        damaged[clen - 1ULL] ^= 0x80U;
        UNIT_CHECK(crypto_hpke_open(r1, out, &mlen, damaged, clen,
                                    NULL, 0U) == -1);
        UNIT_CHECK(mlen == 0ULL);
        UNIT_CHECK(crypto_hpke_open(r1, out, &mlen, ct, clen,
                                    NULL, 0U) == 0);
        UNIT_CHECK(mlen == sizeof pt);
    }

    /* Role misuse: sender cannot open, receiver cannot seal. */
    UNIT_CHECK(crypto_hpke_open(s1, out, &mlen, ct, clen, NULL, 0U) == -1);
    UNIT_CHECK(mlen == 0ULL);
    UNIT_CHECK(crypto_hpke_seal(r1, ct, &clen, pt, sizeof pt,
                                NULL, 0U) == -1);
    UNIT_CHECK(clen == 0ULL);

    /* Exporter length limits (255 * Nh bound) and zero-length export. */
    UNIT_CHECK(crypto_hpke_export(s1, big, 255U * 32U, NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_export(r1, big_r, 255U * 32U, NULL, 0U) == 0);
    UNIT_CHECK(memcmp(big, big_r, 255U * 32U) == 0);
    UNIT_CHECK(crypto_hpke_export(s1, big, 255U * 32U + 1U,
                                  NULL, 0U) == -1);
    UNIT_CHECK(crypto_hpke_export(s1, NULL, 0U, NULL, 0U) == 0);

    /* Multiple independent contexts: distinct enc, independent state. */
    UNIT_CHECK(crypto_hpke_sender_setup(&s2, enc2, pkR, NULL,
                                        NULL, 0U, NULL, 0U) == 0);
    UNIT_CHECK(memcmp(enc1, enc2, 32U) != 0);
    UNIT_CHECK(crypto_hpke_receiver_setup(&r2, enc2, skR, NULL,
                                          NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_seal(s2, ct, &clen, pt, sizeof pt,
                                NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_open(r2, out, &mlen, ct, clen, NULL, 0U) == 0);
    UNIT_CHECK(mlen == sizeof pt);
    /* r1's sequence is unaffected by traffic on r2: it is sitting at
     * sequence number 2 and still opens its own second message. */
    UNIT_CHECK(crypto_hpke_seal(s1, ct, &clen, pt, sizeof pt,
                                NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_open(r1, out, &mlen, ct, clen, NULL, 0U) == 0);
    UNIT_CHECK(mlen == sizeof pt);

    /* NUL bytes inside info are significant, not C-string terminators. */
    UNIT_CHECK(crypto_hpke_sender_setup(&sn, enc1, pkR, NULL,
                                        info_nul, sizeof info_nul,
                                        NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_receiver_setup(&rn, enc1, skR, NULL,
                                          info_nul, sizeof info_nul) == 0);
    UNIT_CHECK(crypto_hpke_export(sn, e1, sizeof e1, NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_export(rn, e2, sizeof e2, NULL, 0U) == 0);
    UNIT_CHECK(memcmp(e1, e2, sizeof e1) == 0);
    {
        crypto_hpke_ctx *rn2 = NULL;
        unsigned char e3[32];
        UNIT_CHECK(crypto_hpke_receiver_setup(&rn2, enc1, skR, NULL,
                                              info_nul2,
                                              sizeof info_nul2) == 0);
        UNIT_CHECK(crypto_hpke_export(rn2, e3, sizeof e3, NULL, 0U) == 0);
        UNIT_CHECK(memcmp(e1, e3, sizeof e1) != 0);
        crypto_hpke_free(rn2);
    }

    /* In-place seal/open: output buffer aliases the input buffer. */
    {
        unsigned char buf_s[48], buf_r[48];
        memcpy(buf_s, pt, sizeof pt);
        UNIT_CHECK(crypto_hpke_seal(sn, buf_s, &clen, buf_s, sizeof pt,
                                    NULL, 0U) == 0);
        UNIT_CHECK(clen == (unsigned long long) (sizeof pt + CTX_TAG));
        memcpy(buf_r, buf_s, (size_t) clen);
        UNIT_CHECK(crypto_hpke_open(rn, buf_r, &mlen, buf_r, clen,
                                    NULL, 0U) == 0);
        UNIT_CHECK(mlen == sizeof pt);
        UNIT_CHECK(memcmp(buf_r, pt, sizeof pt) == 0);
    }

    /* NUL bytes inside exporter_context are significant too. */
    {
        const unsigned char ectx_a[4] = { 'x', 0U, 'y', 'z' };
        const unsigned char ectx_b[4] = { 'x', 0U, 'y', 'Z' };
        unsigned char a1[32], a2[32], b1[32];
        UNIT_CHECK(crypto_hpke_export(sn, a1, sizeof a1,
                                      ectx_a, sizeof ectx_a) == 0);
        UNIT_CHECK(crypto_hpke_export(rn, a2, sizeof a2,
                                      ectx_a, sizeof ectx_a) == 0);
        UNIT_CHECK(memcmp(a1, a2, sizeof a1) == 0);
        UNIT_CHECK(crypto_hpke_export(rn, b1, sizeof b1,
                                      ectx_b, sizeof ectx_b) == 0);
        UNIT_CHECK(memcmp(a1, b1, sizeof a1) != 0);
    }

    /* Sequence-number ceiling via the private test-only hook: no
     * wraparound, failure at the RFC message limit. */
    _crypto_hpke_testonly_set_sequence(s2, SEQ_LAST);
    _crypto_hpke_testonly_set_sequence(r2, SEQ_LAST);
    UNIT_CHECK(crypto_hpke_seal(s2, ct, &clen, pt, sizeof pt,
                                NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_open(r2, out, &mlen, ct, clen, NULL, 0U) == 0);
    UNIT_CHECK(mlen == sizeof pt);
    /* Both roles are now at 2^64 - 1 and must refuse the next message. */
    UNIT_CHECK(crypto_hpke_seal(s2, ct, &clen, pt, sizeof pt,
                                NULL, 0U) == -1);
    UNIT_CHECK(clen == 0ULL);
    UNIT_CHECK(crypto_hpke_open(r2, out, &mlen, ct, clen, NULL, 0U) == -1);
    UNIT_CHECK(mlen == 0ULL);

    /* KEM validation failures must surface before a context exists. */
    {
        crypto_hpke_ctx *bad = NULL;
        UNIT_CHECK(crypto_hpke_sender_setup(&bad, enc1, zero32, NULL,
                                            NULL, 0U, NULL, 0U) == -1);
        UNIT_CHECK(bad == NULL);
        UNIT_CHECK(crypto_hpke_receiver_setup(&bad, zero32, skR, NULL,
                                              NULL, 0U) == -1);
        UNIT_CHECK(bad == NULL);
        /* Auth mode: a small-order sender public key aborts decap. */
        UNIT_CHECK(crypto_hpke_receiver_setup(&bad, enc1, skR, zero32,
                                              NULL, 0U) == -1);
        UNIT_CHECK(bad == NULL);
        /* A NULL out pointer is rejected rather than dereferenced. */
        UNIT_CHECK(crypto_hpke_sender_setup(NULL, enc1, pkR, NULL,
                                            NULL, 0U, NULL, 0U) == -1);
    }

    /* DeriveKeyPair accepts non-32-byte, NUL-containing ikm and is
     * deterministic; the ikm is never interpreted as a raw scalar. */
    {
        static const unsigned char long_ikm[64] = {
            0U, 1U, 2U, 3U, 0U, 255U, 0U, 0U
        };
        unsigned char enc_a[32], enc_b[32];
        crypto_hpke_ctx *sa = NULL, *sb = NULL, *ra = NULL;
        unsigned char ea[32], eb[32];
        UNIT_CHECK(crypto_hpke_sender_setup(&sa, enc_a, pkR, NULL,
                                            NULL, 0U, long_ikm,
                                            sizeof long_ikm) == 0);
        UNIT_CHECK(crypto_hpke_sender_setup(&sb, enc_b, pkR, NULL,
                                            NULL, 0U, long_ikm,
                                            sizeof long_ikm) == 0);
        UNIT_CHECK(memcmp(enc_a, enc_b, 32U) == 0);
        UNIT_CHECK(crypto_hpke_receiver_setup(&ra, enc_a, skR, NULL,
                                              NULL, 0U) == 0);
        UNIT_CHECK(crypto_hpke_export(sa, ea, sizeof ea, NULL, 0U) == 0);
        UNIT_CHECK(crypto_hpke_export(ra, eb, sizeof eb, NULL, 0U) == 0);
        UNIT_CHECK(memcmp(ea, eb, sizeof ea) == 0);
        crypto_hpke_free(sa);
        crypto_hpke_free(sb);
        crypto_hpke_free(ra);
    }

    /* Export output is unaffected by the number of prior seal/open
     * calls and does not move the sequence number. */
    {
        crypto_hpke_ctx *sx = NULL, *rx = NULL;
        unsigned char before[32], after[32];
        UNIT_CHECK(crypto_hpke_sender_setup(&sx, enc1, pkR, NULL,
                                            NULL, 0U, NULL, 0U) == 0);
        UNIT_CHECK(crypto_hpke_receiver_setup(&rx, enc1, skR, NULL,
                                              NULL, 0U) == 0);
        UNIT_CHECK(crypto_hpke_export(sx, before, sizeof before,
                                      NULL, 0U) == 0);
        UNIT_CHECK(crypto_hpke_seal(sx, ct, &clen, pt, sizeof pt,
                                    NULL, 0U) == 0);
        UNIT_CHECK(crypto_hpke_open(rx, out, &mlen, ct, clen,
                                    NULL, 0U) == 0);
        UNIT_CHECK(crypto_hpke_export(sx, after, sizeof after,
                                      NULL, 0U) == 0);
        UNIT_CHECK(memcmp(before, after, sizeof before) == 0);
        /* One message each: another seal/open still uses sequence 1. */
        UNIT_CHECK(crypto_hpke_seal(sx, ct, &clen, pt, sizeof pt,
                                    NULL, 0U) == 0);
        UNIT_CHECK(crypto_hpke_open(rx, out, &mlen, ct, clen,
                                    NULL, 0U) == 0);
        crypto_hpke_free(sx);
        crypto_hpke_free(rx);
    }

    crypto_hpke_free(s1);
    crypto_hpke_free(r1);
    crypto_hpke_free(s2);
    crypto_hpke_free(r2);
    crypto_hpke_free(sn);
    crypto_hpke_free(rn);
    crypto_hpke_free(NULL); /* free(NULL) is defined and a no-op. */
}

void
register_contract(void)
{
    contract_add("vectors", group_vectors);
    contract_add("binding", group_binding);
    contract_add("contexts", group_contexts);
}
