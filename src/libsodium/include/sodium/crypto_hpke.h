
#ifndef crypto_hpke_H
#define crypto_hpke_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

#ifdef __cplusplus
# ifdef __GNUC__
#  pragma GCC diagnostic ignored "-Wlong-long"
# endif
extern "C" {
#endif

/*
 * HPKE (RFC 9180), restricted to the single ciphersuite
 *
 *   KEM:   DHKEM(X25519, HKDF-SHA256), kem_id  = 0x0020
 *   KDF:   HKDF-SHA256,                 kdf_id  = 0x0001
 *   AEAD:  ChaCha20Poly1305,            aead_id = 0x0003
 *
 * Only the base mode (0x00) and the authenticated mode (0x02) are
 * provided.  PSK modes, transport and certificate handling are out of
 * scope.  This is a standalone RFC 9180 implementation built on top of
 * the X25519, HKDF-SHA256 and ChaCha20-Poly1305 primitives; it is not
 * crypto_kx with renamed outputs.
 */

/* Encapsulated key / X25519 public key length (Nenc = Npk). */
#define crypto_hpke_ENCBYTES 32U

/* X25519 static key lengths (Nsk). */
#define crypto_hpke_PUBLICKEYBYTES 32U
#define crypto_hpke_SECRETKEYBYTES 32U

/* ChaCha20Poly1305 parameters (Nk, Nn, Nt). */
#define crypto_hpke_KEYBYTES 32U
#define crypto_hpke_NONCEBYTES 12U
#define crypto_hpke_AEADTAGBYTES 16U

/* HKDF-SHA256 exporter limit: 255 * Nh = 255 * 32 bytes. */
#define crypto_hpke_EXPORT_BYTES_MAX (255U * 32U)

/*
 * Opaque role-specific encryption context.  A context returned by a
 * sender setup can only seal; a context returned by a receiver setup
 * can only open.  Both roles can export.
 */
typedef struct crypto_hpke_ctx crypto_hpke_ctx;

/*
 * Sender setup (RFC 9180 SetupBaseS / SetupAuthS).
 *
 * enc[32] receives the encapsulated ephemeral public key.  pkR[32] is
 * the recipient public key.  info/info_len is length-prefixed raw
 * bytes and may contain NUL bytes; info may be NULL only when
 * info_len is 0.
 *
 * Mode selection: if skS is NULL, the base mode is used; if skS
 * points to a 32-byte X25519 sender static private key, the
 * authenticated mode is used and the corresponding public key is
 * bound into the KEM shared secret and kem_context.
 *
 * Ephemeral key generation: ikmE is input to the RFC 9180 X25519
 * DeriveKeyPair() function (labeled HKDF over the input); it is
 * never used directly as an X25519 scalar.  Passing ikmE == NULL
 * with ikmE_len == 0 generates 32 random bytes with randombytes()
 * first, matching GenerateKeyPair = DeriveKeyPair(random(Nsk)).
 *
 * On success 0 is returned and *out is a newly malloc()ed context.
 * Release it with crypto_hpke_free(), which scrubs all key material
 * before freeing the allocation.  On any failure -1 is returned,
 * *out is set to NULL and no partial context is published.
 */
SODIUM_EXPORT
int crypto_hpke_sender_setup(crypto_hpke_ctx **out, unsigned char enc[32],
                             const unsigned char pkR[32],
                             const unsigned char *skS,
                             const unsigned char *info, size_t info_len,
                             const unsigned char *ikmE, size_t ikmE_len);

/*
 * Receiver setup (RFC 9180 SetupBaseR / SetupAuthR).
 *
 * enc[32] is the encapsulated key produced by the sender and skR[32]
 * is the recipient X25519 private key.  pkS selects the mode in the
 * same way skS does on the sender side: NULL for base mode, a
 * 32-byte sender static public key for authenticated mode.
 *
 * Returns 0 on success with *out holding a malloc()ed context, or -1
 * with *out set to NULL on failure (invalid enc, all-zero DH output,
 * unrepresentable length, ...).
 */
SODIUM_EXPORT
int crypto_hpke_receiver_setup(crypto_hpke_ctx **out,
                               const unsigned char enc[32],
                               const unsigned char skR[32],
                               const unsigned char *pkS,
                               const unsigned char *info, size_t info_len);

/*
 * Seal a message on a sender context.
 *
 * c must hold at least mlen + crypto_hpke_AEADTAGBYTES bytes; on
 * success *clen is set to that length.  The c buffer may alias the m
 * buffer for in-place encryption; other partial overlaps are not
 * supported.  Zero-length messages and aad are allowed.  The 96-bit
 * nonce is base_nonce XOR the big-endian sequence number; the
 * sequence number advances only after a successful seal, and the call
 * fails (without wrapping) once the message limit is reached.  Using
 * a receiver context here fails with -1.  On failure *clen is 0 and
 * no plaintext is delivered.
 */
SODIUM_EXPORT
int crypto_hpke_seal(crypto_hpke_ctx *ctx, unsigned char *c,
                     unsigned long long *clen, const unsigned char *m,
                     unsigned long long mlen, const unsigned char *aad,
                     unsigned long long aadlen);

/*
 * Open a ciphertext on a receiver context.
 *
 * clen must be at least crypto_hpke_AEADTAGBYTES and m must hold at
 * least clen - crypto_hpke_AEADTAGBYTES bytes.  The m buffer may
 * alias the c buffer for in-place decryption; other partial overlaps
 * are not supported.  Authentication failure returns -1 with *mlen
 * set to 0, does not advance the sequence number and leaves the
 * context usable, so a later valid ciphertext still opens.  Using a
 * sender context here fails with -1.
 */
SODIUM_EXPORT
int crypto_hpke_open(crypto_hpke_ctx *ctx, unsigned char *m,
                     unsigned long long *mlen, const unsigned char *c,
                     unsigned long long clen, const unsigned char *aad,
                     unsigned long long aadlen);

/*
 * Derive exporter secret bytes (RFC 9180 Section 5.3).
 *
 * outlen must not exceed crypto_hpke_EXPORT_BYTES_MAX (the HKDF 255
 * block bound); exporter_context is length-prefixed raw bytes (NUL
 * allowed, NULL only with contextlen 0).  Sender and receiver
 * contexts derived from the same setup produce identical bytes.  The
 * export operation never touches the sequence number and is
 * unaffected by how many seals or opens happened beforehand.
 */
SODIUM_EXPORT
int crypto_hpke_export(const crypto_hpke_ctx *ctx, unsigned char *out,
                       size_t outlen, const unsigned char *exporter_context,
                       size_t contextlen);

/*
 * Scrub all key material and sequence state in the context and free
 * it.  Like free(), accepting NULL is defined and does nothing.
 */
SODIUM_EXPORT
void crypto_hpke_free(crypto_hpke_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif
