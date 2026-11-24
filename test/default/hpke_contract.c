#include "hpke_api.h"
#include "hpke_vectors.h"
#include "contract.h"

#include <sodium/core.h>
#include <sodium/crypto_scalarmult_curve25519.h>

#include "../../src/libsodium/crypto_hpke_local.h"

#include <stdint.h>
#include <string.h>

static size_t
unhex(unsigned char *out, size_t cap, const char *s)
{
    size_t len;
    size_t i;

    if (s == NULL || out == NULL) {
        return (size_t) -1;
    }
    len = strlen(s);
    if ((len & 1U) != 0U || len / 2U > cap) {
        return (size_t) -1;
    }
    for (i = 0U; i < len; i += 2U) {
        int hi = -1;
        int lo = -1;
        char a = s[i];
        char b = s[i + 1U];

        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        if (hi < 0 || lo < 0) {
            return (size_t) -1;
        }
        out[i / 2U] = (unsigned char) ((hi << 4) | lo);
    }
    return len / 2U;
}

static void
vectors_group(void)
{
    size_t v;

    for (v = 0U; v < sizeof hpke_vectors / sizeof hpke_vectors[0]; v++) {
        const hpke_vector *tv = &hpke_vectors[v];
        unsigned char ikmE[32], skR[32], pkR[32], expected_enc[32];
        unsigned char skS[32], pkS[32], expected_pkS[32];
        unsigned char info[64];
        size_t ikmE_len, info_len;
        const unsigned char *skS_arg = NULL;
        const unsigned char *pkS_arg = NULL;
        crypto_hpke_ctx *sender = NULL;
        crypto_hpke_ctx *receiver = NULL;
        unsigned char enc[32];
        size_t i;

        UNIT_CHECK(sodium_init() >= 0);
        ikmE_len = unhex(ikmE, sizeof ikmE, tv->ikmE);
        UNIT_CHECK(ikmE_len == 32U);
        UNIT_CHECK(unhex(skR, sizeof skR, tv->skR) == 32U);
        UNIT_CHECK(unhex(pkR, sizeof pkR, tv->pkR) == 32U);
        UNIT_CHECK(unhex(expected_enc, sizeof expected_enc, tv->enc) == 32U);
        info_len = unhex(info, sizeof info, tv->info);
        UNIT_CHECK(info_len != (size_t) -1 && info_len <= sizeof info);

        {
            unsigned char derived_pkR[32];
            UNIT_CHECK(crypto_scalarmult_curve25519_base(derived_pkR, skR) == 0);
            UNIT_CHECK(memcmp(derived_pkR, pkR, 32U) == 0);
        }

        if (tv->mode == 2) {
            UNIT_CHECK(unhex(skS, sizeof skS, tv->skS) == 32U);
            UNIT_CHECK(unhex(expected_pkS, sizeof expected_pkS, tv->pkS) == 32U);
            UNIT_CHECK(crypto_scalarmult_curve25519_base(pkS, skS) == 0);
            UNIT_CHECK(memcmp(pkS, expected_pkS, 32U) == 0);
            skS_arg = skS;
            pkS_arg = pkS;
        }

        UNIT_CHECK(crypto_hpke_sender_setup(
                       &sender, enc, pkR, skS_arg, info, info_len,
                       ikmE, ikmE_len) == 0);
        UNIT_CHECK(sender != NULL);
        UNIT_CHECK(memcmp(enc, expected_enc, 32U) == 0);
        UNIT_CHECK(crypto_hpke_receiver_setup(
                       &receiver, enc, skR, pkS_arg, info, info_len) == 0);
        UNIT_CHECK(receiver != NULL);

        for (i = 0U; i < 3U; i++) {
            unsigned char aad[32], pt[64], want_ct[96];
            unsigned char ct[96], got_pt[64];
            size_t aad_len, pt_len, want_ct_len;
            unsigned long long ct_len = 0;
            unsigned long long got_pt_len = 999U;

            aad_len = unhex(aad, sizeof aad, tv->aad[i]);
            pt_len = unhex(pt, sizeof pt, tv->pt[i]);
            want_ct_len = unhex(want_ct, sizeof want_ct, tv->ct[i]);
            UNIT_CHECK(aad_len != (size_t) -1);
            UNIT_CHECK(pt_len != (size_t) -1);
            UNIT_CHECK(want_ct_len != (size_t) -1);
            UNIT_CHECK(want_ct_len == pt_len + 16U);

            UNIT_CHECK(crypto_hpke_seal(sender, ct, &ct_len,
                                        pt, (unsigned long long) pt_len,
                                        aad, (unsigned long long) aad_len) == 0);
            UNIT_CHECK(ct_len == (unsigned long long) want_ct_len);
            UNIT_CHECK(memcmp(ct, want_ct, want_ct_len) == 0);

            UNIT_CHECK(crypto_hpke_open(receiver, got_pt, &got_pt_len,
                                        want_ct,
                                        (unsigned long long) want_ct_len,
                                        aad,
                                        (unsigned long long) aad_len) == 0);
            UNIT_CHECK(got_pt_len == (unsigned long long) pt_len);
            UNIT_CHECK(memcmp(got_pt, pt, pt_len) == 0);
        }

        for (i = 0U; i < 3U; i++) {
            unsigned char exporter_context[64];
            unsigned char expected[32], sender_out[32], receiver_out[32];
            size_t context_len, expected_len;

            context_len = unhex(exporter_context, sizeof exporter_context,
                                tv->exporter_context[i]);
            expected_len = unhex(expected, sizeof expected,
                                 tv->exported_value[i]);
            UNIT_CHECK(context_len != (size_t) -1);
            UNIT_CHECK(expected_len == tv->L[i]);
            UNIT_CHECK(expected_len <= sizeof sender_out);

            UNIT_CHECK(crypto_hpke_export(sender, sender_out,
                                          expected_len,
                                          exporter_context,
                                          context_len) == 0);
            UNIT_CHECK(crypto_hpke_export(receiver, receiver_out,
                                          expected_len,
                                          exporter_context,
                                          context_len) == 0);
            UNIT_CHECK(memcmp(sender_out, expected, expected_len) == 0);
            UNIT_CHECK(memcmp(receiver_out, expected, expected_len) == 0);
            UNIT_CHECK(memcmp(sender_out, receiver_out, expected_len) == 0);
        }

        crypto_hpke_free(sender);
        crypto_hpke_free(receiver);
    }
}

static void
binding_group(void)
{
    const hpke_vector *base = &hpke_vectors[0];
    const hpke_vector *authv = &hpke_vectors[1];
    unsigned char ikmE[32], skR[32], pkR[32];
    unsigned char askm[32], askR[32], apkR[32], askS[32], apkS[32];
    unsigned char auth_info[64];
    size_t auth_info_len;
    crypto_hpke_ctx *s_info_a = NULL, *s_info_b = NULL;
    crypto_hpke_ctx *r_info_b = NULL, *r_info_a = NULL;
    unsigned char enc_a[32], enc_b[32];
    static const unsigned char info_a[] = "binding-info-A";
    static const unsigned char info_b[] = "binding-info-B-different";
    static const unsigned char aad[] = "contract-AAD";
    static const unsigned char msg[] = "binding message bytes";
    unsigned char ct_a[96], ct_b[96], out[96];
    unsigned long long ct_a_len = 0, ct_b_len = 0, out_len = 0;
    unsigned char nul_info_a[4] = { 'i', 'd', 0, 'A' };
    unsigned char nul_info_b[4] = { 'i', 'd', 0, 'B' };
    crypto_hpke_ctx *s_nul_a = NULL, *s_nul_b = NULL;
    unsigned char nul_enc[32], nul_ct_a[64], nul_ct_b[64];
    unsigned long long nul_ct_len = 0;
    crypto_hpke_ctx *r_nul_b = NULL;
    unsigned char wrong_sk[32], wrong_pk[32];
    crypto_hpke_ctx *as = NULL, *ar_wrong = NULL, *ar_base = NULL;
    crypto_hpke_ctx *ar_ok = NULL;
    unsigned char aenc[32], act[96];
    unsigned long long act_len = 0;
    crypto_hpke_ctx *wrong_sender_r = NULL;
    unsigned char wrong_skR[32];

    UNIT_CHECK(unhex(ikmE, 32, base->ikmE) == 32U);
    UNIT_CHECK(unhex(skR, 32, base->skR) == 32U);
    UNIT_CHECK(unhex(pkR, 32, base->pkR) == 32U);

    UNIT_CHECK(crypto_hpke_sender_setup(
                   &s_info_a, enc_a, pkR, NULL,
                   info_a, sizeof info_a - 1U, ikmE, 32U) == 0);
    UNIT_CHECK(crypto_hpke_sender_setup(
                   &s_info_b, enc_b, pkR, NULL,
                   info_b, sizeof info_b - 1U, ikmE, 32U) == 0);
    UNIT_CHECK(memcmp(enc_a, enc_b, 32U) == 0);
    UNIT_CHECK(crypto_hpke_seal(s_info_a, ct_a, &ct_a_len,
                               msg, sizeof msg - 1U,
                               aad, sizeof aad - 1U) == 0);
    UNIT_CHECK(crypto_hpke_seal(s_info_b, ct_b, &ct_b_len,
                               msg, sizeof msg - 1U,
                               aad, sizeof aad - 1U) == 0);
    UNIT_CHECK(ct_a_len == ct_b_len);
    UNIT_CHECK(memcmp(ct_a, ct_b, ct_a_len) != 0);

    UNIT_CHECK(crypto_hpke_receiver_setup(
                   &r_info_b, enc_a, skR, NULL,
                   info_b, sizeof info_b - 1U) == 0);
    out_len = 999U;
    UNIT_CHECK(crypto_hpke_open(r_info_b, out, &out_len,
                                ct_a, ct_a_len,
                                aad, sizeof aad - 1U) == -1);
    UNIT_CHECK(out_len == 0U);
    crypto_hpke_free(r_info_b);

    UNIT_CHECK(crypto_hpke_receiver_setup(
                   &r_info_a, enc_a, skR, NULL,
                   info_a, sizeof info_a - 1U) == 0);
    out_len = 999U;
    UNIT_CHECK(crypto_hpke_open(r_info_a, out, &out_len,
                                ct_a, ct_a_len,
                                (const unsigned char *) "wrong-AAD", 9U) == -1);
    UNIT_CHECK(out_len == 0U);
    UNIT_CHECK(crypto_hpke_open(r_info_a, out, &out_len,
                                ct_a, ct_a_len,
                                aad, sizeof aad - 1U) == 0);
    UNIT_CHECK(out_len == sizeof msg - 1U);
    UNIT_CHECK(memcmp(out, msg, sizeof msg - 1U) == 0);

    /* Embedded NULs and explicit lengths must participate in binding. */
    UNIT_CHECK(crypto_hpke_sender_setup(
                   &s_nul_a, nul_enc, pkR, NULL,
                   nul_info_a, sizeof nul_info_a, ikmE, 32U) == 0);
    UNIT_CHECK(crypto_hpke_sender_setup(
                   &s_nul_b, nul_enc, pkR, NULL,
                   nul_info_b, sizeof nul_info_b, ikmE, 32U) == 0);
    UNIT_CHECK(crypto_hpke_seal(s_nul_a, nul_ct_a, &nul_ct_len,
                               msg, 4U, NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_seal(s_nul_b, nul_ct_b, &nul_ct_len,
                               msg, 4U, NULL, 0U) == 0);
    UNIT_CHECK(memcmp(nul_ct_a, nul_ct_b, nul_ct_len) != 0);
    UNIT_CHECK(crypto_hpke_receiver_setup(
                   &r_nul_b, nul_enc, skR, NULL,
                   nul_info_b, sizeof nul_info_b) == 0);
    out_len = 999U;
    UNIT_CHECK(crypto_hpke_open(r_nul_b, out, &out_len,
                                nul_ct_a, nul_ct_len, NULL, 0U) == -1);
    UNIT_CHECK(out_len == 0U);

    /* auth mode: wrong or omitted sender identity must not decrypt. */
    UNIT_CHECK(unhex(askm, 32, authv->ikmE) == 32U);
    UNIT_CHECK(unhex(askR, 32, authv->skR) == 32U);
    UNIT_CHECK(unhex(apkR, 32, authv->pkR) == 32U);
    UNIT_CHECK(unhex(askS, 32, authv->skS) == 32U);
    UNIT_CHECK(unhex(apkS, 32, authv->pkS) == 32U);
    auth_info_len = unhex(auth_info, sizeof auth_info, authv->info);
    UNIT_CHECK(auth_info_len != (size_t) -1);
    memcpy(wrong_sk, askS, 32U);
    wrong_sk[0] ^= 0x08U;
    UNIT_CHECK(crypto_scalarmult_curve25519_base(wrong_pk, wrong_sk) == 0);
    UNIT_CHECK(memcmp(wrong_pk, apkS, 32U) != 0);

    UNIT_CHECK(crypto_hpke_sender_setup(
                   &as, aenc, apkR, askS,
                   auth_info,
                   auth_info_len, askm, 32U) == 0);
    UNIT_CHECK(crypto_hpke_seal(as, act, &act_len,
                               msg, sizeof msg - 1U,
                               aad, sizeof aad - 1U) == 0);
    UNIT_CHECK(crypto_hpke_receiver_setup(
                   &ar_wrong, aenc, askR, wrong_pk,
                   auth_info,
                   auth_info_len) == 0);
    out_len = 999U;
    UNIT_CHECK(crypto_hpke_open(ar_wrong, out, &out_len,
                                act, act_len,
                                aad, sizeof aad - 1U) == -1);
    UNIT_CHECK(out_len == 0U);
    UNIT_CHECK(crypto_hpke_receiver_setup(
                   &ar_base, aenc, askR, NULL,
                   auth_info,
                   auth_info_len) == 0);
    UNIT_CHECK(crypto_hpke_open(ar_base, out, &out_len,
                                act, act_len,
                                aad, sizeof aad - 1U) == -1);
    UNIT_CHECK(crypto_hpke_receiver_setup(
                   &ar_ok, aenc, askR, apkS,
                   auth_info,
                   auth_info_len) == 0);
    UNIT_CHECK(crypto_hpke_open(ar_ok, out, &out_len,
                                act, act_len,
                                aad, sizeof aad - 1U) == 0);

    /* Receiver identity: decapsulating with another skR must fail. */
    memcpy(wrong_skR, skR, 32U);
    wrong_skR[0] ^= 0x08U;
    UNIT_CHECK(crypto_hpke_receiver_setup(
                   &wrong_sender_r, enc_a, wrong_skR, NULL,
                   info_a, sizeof info_a - 1U) == 0);
    out_len = 999U;
    UNIT_CHECK(crypto_hpke_open(wrong_sender_r, out, &out_len,
                                ct_a, ct_a_len,
                                aad, sizeof aad - 1U) == -1);
    UNIT_CHECK(out_len == 0U);

    crypto_hpke_free(s_info_a);
    crypto_hpke_free(s_info_b);
    crypto_hpke_free(r_info_a);
    crypto_hpke_free(s_nul_a);
    crypto_hpke_free(s_nul_b);
    crypto_hpke_free(r_nul_b);
    crypto_hpke_free(as);
    crypto_hpke_free(ar_wrong);
    crypto_hpke_free(ar_base);
    crypto_hpke_free(ar_ok);
    crypto_hpke_free(wrong_sender_r);
}

static void
contexts_group(void)
{
    const hpke_vector *tv = &hpke_vectors[0];
    unsigned char ikmE[32], skR[32], pkR[32];
    static const unsigned char zero32[32] = { 0 };
    static const unsigned char low_order[32] = {
        0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae,
        0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
        0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd,
        0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00
    };
    crypto_hpke_ctx *random_s = NULL, *random_r = NULL;
    crypto_hpke_ctx *s = NULL, *r = NULL;
    crypto_hpke_ctx *bad = NULL, *kept = NULL;
    unsigned char enc[32], bad_enc[32];
    unsigned char ct[96], pt[96];
    unsigned long long ct_len = 0, pt_len = 0;
    unsigned char exported[8161];
    unsigned char export_before[32], export_after[32];
    crypto_hpke_ctx *a_s = NULL, *a_r = NULL, *b_s = NULL, *b_r = NULL;
    unsigned char a_enc[32], b_enc[32];
    unsigned char c0[64], c1[64], c2[64], c3[64];
    unsigned long long l0 = 0, l1 = 0, l2 = 0, l3 = 0;
    unsigned char p0[32], p1[32], p2[32], p3[32];
    unsigned long long pl = 0;
    static const unsigned char seq_msg[] = "sequence-boundary";
    unsigned char seq_ct[64], seq_pt[64];
    unsigned long long seq_ct_len = 0, seq_pt_len = 0;
    unsigned long long limit_ct_len = 0;

    UNIT_CHECK(unhex(ikmE, 32, tv->ikmE) == 32U);
    UNIT_CHECK(unhex(skR, 32, tv->skR) == 32U);
    UNIT_CHECK(unhex(pkR, 32, tv->pkR) == 32U);

    crypto_hpke_free(NULL);

    /* Invalid lengths and keys fail with *out == NULL. */
    UNIT_CHECK(crypto_hpke_sender_setup(
                   &bad, bad_enc, pkR, NULL,
                   (const unsigned char *) "x", (size_t) -1,
                   ikmE, 32U) == -1);
    UNIT_CHECK(bad == NULL);
    UNIT_CHECK(crypto_hpke_sender_setup(
                   &bad, bad_enc, pkR, NULL, NULL, 0U,
                   NULL, 32U) == -1);
    UNIT_CHECK(bad == NULL);
    UNIT_CHECK(crypto_hpke_sender_setup(
                   &bad, bad_enc, zero32, NULL, NULL, 0U,
                   ikmE, 32U) == -1);
    UNIT_CHECK(bad == NULL);
    UNIT_CHECK(crypto_hpke_sender_setup(
                   &bad, bad_enc, low_order, NULL, NULL, 0U,
                   ikmE, 32U) == -1);
    UNIT_CHECK(bad == NULL);

    UNIT_CHECK(crypto_hpke_sender_setup(
                   &kept, enc, pkR, NULL, NULL, 0U, ikmE, 32U) == 0);
    UNIT_CHECK(kept != NULL);
    bad = kept;
    UNIT_CHECK(crypto_hpke_receiver_setup(
                   &bad, zero32, skR, NULL, NULL, 0U) == -1);
    UNIT_CHECK(bad == NULL);
    bad = kept;
    UNIT_CHECK(crypto_hpke_receiver_setup(
                   &bad, enc, skR, low_order, NULL, 0U) == -1);
    UNIT_CHECK(bad == NULL);
    crypto_hpke_free(kept);

    /* NULL/0 IKM selects randombytes and still interoperates. */
    UNIT_CHECK(crypto_hpke_sender_setup(
                   &random_s, enc, pkR, NULL,
                   (const unsigned char *) "random-context", 14U,
                   NULL, 0U) == 0);
    UNIT_CHECK(crypto_hpke_receiver_setup(
                   &random_r, enc, skR, NULL,
                   (const unsigned char *) "random-context", 14U) == 0);

    ct_len = 999U;
    UNIT_CHECK(crypto_hpke_seal(random_s, ct, &ct_len,
                               NULL, 0U, NULL, 0U) == 0);
    UNIT_CHECK(ct_len == 16U);
    pt_len = 999U;
    UNIT_CHECK(crypto_hpke_open(random_r, pt, &pt_len,
                                ct, ct_len, NULL, 0U) == 0);
    UNIT_CHECK(pt_len == 0U);

    ct_len = 0;
    UNIT_CHECK(crypto_hpke_seal(random_s, ct, &ct_len,
                               (const unsigned char *) "z", 1U,
                               NULL, 0U) == 0);
    pt_len = 999U;
    UNIT_CHECK(crypto_hpke_open(random_r, pt, &pt_len,
                                ct, ct_len, NULL, 0U) == 0);
    UNIT_CHECK(pt_len == 1U);
    UNIT_CHECK(pt[0] == 'z');

    {
        unsigned char in_place[64];
        unsigned long long in_len = 0;
        memcpy(in_place, "in-place", 8U);
        UNIT_CHECK(crypto_hpke_seal(random_s, in_place, &in_len,
                                    in_place, 8U, NULL, 0U) == 0);
        UNIT_CHECK(in_len == 24U);
        pt_len = 999U;
        UNIT_CHECK(crypto_hpke_open(random_r, in_place, &pt_len,
                                    in_place, in_len, NULL, 0U) == 0);
        UNIT_CHECK(pt_len == 8U);
        UNIT_CHECK(memcmp(in_place, "in-place", 8U) == 0);
    }
    crypto_hpke_free(random_s);
    crypto_hpke_free(random_r);

    /* Tampered tag fails, does not advance seq, and leaves retry possible. */
    UNIT_CHECK(crypto_hpke_sender_setup(
                   &s, enc, pkR, NULL,
                   (const unsigned char *) "contexts", 8U,
                   ikmE, 32U) == 0);
    UNIT_CHECK(crypto_hpke_receiver_setup(
                   &r, enc, skR, NULL,
                   (const unsigned char *) "contexts", 8U) == 0);
    UNIT_CHECK(crypto_hpke_export(s, export_before, 32U,
                                  (const unsigned char *) "before", 6U) == 0);
    UNIT_CHECK(crypto_hpke_seal(s, ct, &ct_len,
                               (const unsigned char *) "retry-me", 8U,
                               (const unsigned char *) "ad", 2U) == 0);
    {
        unsigned char bad_ct[96];
        unsigned char bad_pt[96];
        unsigned long long bad_len = 999U;

        memcpy(bad_ct, ct, ct_len);
        bad_ct[ct_len - 1U] ^= 0x80U;
        UNIT_CHECK(crypto_hpke_open(r, bad_pt, &bad_len,
                                    bad_ct, ct_len,
                                    (const unsigned char *) "ad", 2U) == -1);
        UNIT_CHECK(bad_len == 0U);
    }
    pt_len = 999U;
    UNIT_CHECK(crypto_hpke_open(r, pt, &pt_len,
                                ct, ct_len,
                                (const unsigned char *) "ad", 2U) == 0);
    UNIT_CHECK(pt_len == 8U);
    UNIT_CHECK(memcmp(pt, "retry-me", 8U) == 0);

    /* Role misuse returns -1 and does not consume either sequence. */
    pt_len = 777U;
    UNIT_CHECK(crypto_hpke_seal(r, pt, &pt_len,
                                (const unsigned char *) "x", 1U,
                                NULL, 0U) == -1);
    UNIT_CHECK(pt_len == 0U);
    ct_len = 777U;
    UNIT_CHECK(crypto_hpke_open(s, pt, &ct_len,
                                ct, sizeof ct,
                                NULL, 0U) == -1);
    UNIT_CHECK(ct_len == 0U);
    UNIT_CHECK(crypto_hpke_seal(s, ct, &ct_len,
                               (const unsigned char *) "after-roles", 11U,
                               NULL, 0U) == 0);
    pt_len = 999U;
    UNIT_CHECK(crypto_hpke_open(r, pt, &pt_len,
                                ct, ct_len, NULL, 0U) == 0);
    UNIT_CHECK(pt_len == 11U);

    /* Export is sequence-independent and enforces HKDF's 255 blocks. */
    UNIT_CHECK(crypto_hpke_export(s, export_after, 32U,
                                  (const unsigned char *) "before", 6U) == 0);
    UNIT_CHECK(memcmp(export_before, export_after, 32U) == 0);
    UNIT_CHECK(crypto_hpke_export(s, exported, 8160U,
                                  (const unsigned char *) "length", 6U) == 0);
    UNIT_CHECK(crypto_hpke_export(r, exported + 1, 8160U,
                                  (const unsigned char *) "length", 6U) == 0);
    UNIT_CHECK(crypto_hpke_export(s, exported, 8161U,
                                  (const unsigned char *) "length", 6U) == -1);
    UNIT_CHECK(crypto_hpke_export(s, exported, (size_t) -1,
                                  NULL, 0U) == -1);
    UNIT_CHECK(crypto_hpke_export(s, NULL, 1U, NULL, 0U) == -1);
    UNIT_CHECK(crypto_hpke_export(s, exported, 32U,
                                  (const unsigned char *) "x",
                                  (size_t) -1) == -1);

    /* Independent contexts have independent sequence/key state. */
    {
        unsigned char ikmA[32], ikmB[32];
        memset(ikmA, 0x41, sizeof ikmA);
        memset(ikmB, 0x42, sizeof ikmB);
        UNIT_CHECK(crypto_hpke_sender_setup(&a_s, a_enc, pkR, NULL,
                                            (const unsigned char *) "A", 1U,
                                            ikmA, 32U) == 0);
        UNIT_CHECK(crypto_hpke_receiver_setup(&a_r, a_enc, skR, NULL,
                                              (const unsigned char *) "A", 1U) == 0);
        UNIT_CHECK(crypto_hpke_sender_setup(&b_s, b_enc, pkR, NULL,
                                            (const unsigned char *) "B", 1U,
                                            ikmB, 32U) == 0);
        UNIT_CHECK(crypto_hpke_receiver_setup(&b_r, b_enc, skR, NULL,
                                              (const unsigned char *) "B", 1U) == 0);

        UNIT_CHECK(crypto_hpke_seal(a_s, c0, &l0,
                                    (const unsigned char *)"A0", 2U, NULL, 0U) == 0);
        UNIT_CHECK(crypto_hpke_seal(b_s, c1, &l1,
                                    (const unsigned char *)"B0", 2U, NULL, 0U) == 0);
        UNIT_CHECK(crypto_hpke_seal(b_s, c2, &l2,
                                    (const unsigned char *)"B1", 2U, NULL, 0U) == 0);
        UNIT_CHECK(crypto_hpke_seal(a_s, c3, &l3,
                                    (const unsigned char *)"A1", 2U, NULL, 0U) == 0);

        UNIT_CHECK(crypto_hpke_open(a_r, p0, &pl, c0, l0, NULL, 0U) == 0);
        UNIT_CHECK(pl == 2U && memcmp(p0, "A0", 2U) == 0);
        UNIT_CHECK(crypto_hpke_open(b_r, p1, &pl, c1, l1, NULL, 0U) == 0);
        UNIT_CHECK(pl == 2U && memcmp(p1, "B0", 2U) == 0);
        UNIT_CHECK(crypto_hpke_open(b_r, p2, &pl, c2, l2, NULL, 0U) == 0);
        UNIT_CHECK(pl == 2U && memcmp(p2, "B1", 2U) == 0);
        UNIT_CHECK(crypto_hpke_open(a_r, p3, &pl, c3, l3, NULL, 0U) == 0);
        UNIT_CHECK(pl == 2U && memcmp(p3, "A1", 2U) == 0);
    }

    /* Push the private 96-bit counter to RFC 9180's upper boundary. */
    memset(s->seq, 0xff, sizeof s->seq);
    s->seq[11] = 0xfdU; /* 2^96 - 3: two usable increments remain */
    memset(r->seq, 0xff, sizeof r->seq);
    r->seq[11] = 0xfdU;

    UNIT_CHECK(crypto_hpke_seal(s, seq_ct, &seq_ct_len,
                                seq_msg, 1U, NULL, 0U) == 0);
    UNIT_CHECK(s->seq[11] == 0xfeU);
    seq_pt_len = 999U;
    UNIT_CHECK(crypto_hpke_open(r, seq_pt, &seq_pt_len,
                                seq_ct, seq_ct_len, NULL, 0U) == 0);
    UNIT_CHECK(r->seq[11] == 0xfeU);

    seq_ct_len = 0;
    UNIT_CHECK(crypto_hpke_seal(s, seq_ct, &seq_ct_len,
                                seq_msg, sizeof seq_msg, NULL, 0U) == 0);
    {
        size_t j;
        for (j = 0U; j < 12U; j++) UNIT_CHECK(s->seq[j] == 0xffU);
    }
    seq_pt_len = 999U;
    UNIT_CHECK(crypto_hpke_open(r, seq_pt, &seq_pt_len,
                                seq_ct, seq_ct_len, NULL, 0U) == 0);
    {
        size_t j;
        for (j = 0U; j < 12U; j++) UNIT_CHECK(r->seq[j] == 0xffU);
    }
    limit_ct_len = seq_ct_len;

    UNIT_CHECK(crypto_hpke_seal(s, seq_ct, &seq_ct_len,
                                seq_msg, 1U, NULL, 0U) == -1);
    seq_pt_len = 999U;
    UNIT_CHECK(crypto_hpke_open(r, seq_pt, &seq_pt_len,
                                seq_ct, limit_ct_len, NULL, 0U) == -1);
    UNIT_CHECK(seq_pt_len == 0U);
    {
        size_t j;
        for (j = 0U; j < 12U; j++) {
            UNIT_CHECK(s->seq[j] == 0xffU);
            UNIT_CHECK(r->seq[j] == 0xffU);
        }
    }

    crypto_hpke_free(s);
    crypto_hpke_free(r);
    crypto_hpke_free(a_s);
    crypto_hpke_free(a_r);
    crypto_hpke_free(b_s);
    crypto_hpke_free(b_r);
}

void
register_contract(void)
{
    contract_add("vectors", vectors_group);
    contract_add("binding", binding_group);
    contract_add("contexts", contexts_group);
}
