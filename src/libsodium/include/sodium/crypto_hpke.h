#ifndef crypto_hpke_H
#define crypto_hpke_H

#include <stddef.h>

#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RFC 9180 HPKE, limited to:
 *   DHKEM(X25519, HKDF-SHA256) (KEM ID 0x0020),
 *   HKDF-SHA256                (KDF ID 0x0001), and
 *   ChaCha20-Poly1305          (AEAD ID 0x0003).
 *
 * Only mode_base (0x00) and mode_auth (0x02) are implemented. PSK modes,
 * export-only AEAD, and other ciphersuites are intentionally absent.
 *
 * All key, nonce, encapsulated-key, and tag sizes below are fixed by the
 * selected ciphersuite. Byte strings passed as pointer/length pairs are
 * binary-safe and may contain NUL bytes.
 */

#define crypto_hpke_BYTES 32U
#define crypto_hpke_ENCBYTES 32U
#define crypto_hpke_PUBLICKEYBYTES 32U
#define crypto_hpke_SECRETKEYBYTES 32U
#define crypto_hpke_KEYBYTES 32U
#define crypto_hpke_NONCEBYTES 12U
#define crypto_hpke_ABYTES 16U
#define crypto_hpke_EXPORT_BYTES_MAX (255U * 32U)

#if !defined(CHECK_HPKE_API_H)
typedef struct crypto_hpke_ctx crypto_hpke_ctx;
#endif

/*
 * Deterministically derive an ephemeral X25519 key pair from ikmE using
 * RFC 9180 DHKEM DeriveKeyPair(). ikmE is input keying material, not a raw
 * X25519 scalar. Pass ikmE == NULL and ikmE_len == 0 to generate 32 random
 * bytes of IKM.
 *
 * skS == NULL selects mode_base; a non-NULL 32-byte skS selects mode_auth.
 * On success, *out is a sender context and enc[0..31] is the encapsulated
 * ephemeral public key. On failure, *out is set to NULL and enc is not a
 * complete HPKE encapsulation.
 */
SODIUM_EXPORT
int crypto_hpke_sender_setup(crypto_hpke_ctx **out,
                             unsigned char enc[crypto_hpke_ENCBYTES],
                             const unsigned char pkR[crypto_hpke_PUBLICKEYBYTES],
                             const unsigned char *skS,
                             const unsigned char *info, size_t info_len,
                             const unsigned char *ikmE, size_t ikmE_len);

/*
 * Decapsulate enc using skR and build a receiver context. pkS == NULL
 * selects mode_base; a non-NULL 32-byte sender public key selects mode_auth.
 * On failure, *out is set to NULL.
 */
SODIUM_EXPORT
int crypto_hpke_receiver_setup(crypto_hpke_ctx **out,
                               const unsigned char enc[crypto_hpke_ENCBYTES],
                               const unsigned char skR[crypto_hpke_SECRETKEYBYTES],
                               const unsigned char *pkS,
                               const unsigned char *info, size_t info_len);

/*
 * Encrypt for a sender context. c must have room for mlen plus
 * crypto_hpke_ABYTES bytes. *clen receives mlen + 16. The exact in-place
 * form c == m is supported when c has the extra 16-byte tag capacity;
 * other overlapping ranges are not supported.
 *
 * The sequence number advances only after successful AEAD encryption.
 */
SODIUM_EXPORT
int crypto_hpke_seal(crypto_hpke_ctx *ctx,
                     unsigned char *c, unsigned long long *clen,
                     const unsigned char *m, unsigned long long mlen,
                     const unsigned char *ad, unsigned long long adlen);

/*
 * Decrypt with a receiver context. m must have room for at least
 * clen - crypto_hpke_ABYTES bytes when clen >= 16. The exact in-place form
 * m == c is supported; other overlapping ranges are not supported.
 *
 * On authentication or format failure *mlen is zero and no plaintext is
 * returned. Failed opens do not advance the sequence number.
 */
SODIUM_EXPORT
int crypto_hpke_open(crypto_hpke_ctx *ctx,
                     unsigned char *m, unsigned long long *mlen,
                     const unsigned char *c, unsigned long long clen,
                     const unsigned char *ad, unsigned long long adlen);

/*
 * Derive outlen exporter bytes. outlen must not exceed
 * crypto_hpke_EXPORT_BYTES_MAX. Export does not advance the sequence number
 * and is valid on either sender or receiver contexts.
 */
SODIUM_EXPORT
int crypto_hpke_export(const crypto_hpke_ctx *ctx,
                       unsigned char *out, size_t outlen,
                       const unsigned char *exporter_context,
                       size_t exporter_context_len);

/* Zero context key material and release it. crypto_hpke_free(NULL) is a no-op. */
SODIUM_EXPORT
void crypto_hpke_free(crypto_hpke_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif
