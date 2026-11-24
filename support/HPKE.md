# HPKE contract implementation

This repository adds the narrow RFC 9180 ciphersuite used by the offline
recipient-public-key encryption contract:

- KEM: DHKEM(X25519, HKDF-SHA256), ID `0x0020`
- KDF: HKDF-SHA256, ID `0x0001`
- AEAD: ChaCha20-Poly1305 (IETF nonce layout), ID `0x0003`
- Modes: `mode_base` (`0x00`) and `mode_auth` (`0x02`)

PSK/AuthPSK modes, export-only AEAD, hybrid/post-quantum KEMs, and other
ciphersuites are not provided.

## API

The contract declarations are in `support/hpke_api.h`; the installed public
header is `sodium/crypto_hpke.h`.

- `crypto_hpke_sender_setup`
- `crypto_hpke_receiver_setup`
- `crypto_hpke_seal`
- `crypto_hpke_open`
- `crypto_hpke_export`
- `crypto_hpke_free`

For sender setup, non-NULL `skS` selects RFC 9180 AuthEncap; NULL selects
base Encap. For receiver setup, non-NULL `pkS` selects AuthDecap; NULL
selects base Decap.

`ikmE` is RFC 9180 `DeriveKeyPair()` input keying material. It is not used
directly as an X25519 scalar. Pass `ikmE == NULL` and `ikmE_len == 0` to
generate 32 random IKM bytes through `randombytes`.

All pointer/length inputs are binary-safe. In particular, `info` and exporter
context may contain NUL bytes; their supplied lengths control processing.

A context is published through `*out` only after all key generation, DH
validation, transcript binding, and key-schedule derivation succeed. Setup
failures set `*out = NULL`. Release a successful context with
`crypto_hpke_free`, which zeroes the key material before `free`;
`crypto_hpke_free(NULL)` is a no-op.

## Buffers and overlap

- `enc`, X25519 public keys, private keys, and derived keys are 32 bytes.
- `crypto_hpke_seal` writes `mlen + 16` bytes, so `c` must have at least that
  capacity. The exact in-place form `c == m` is supported if `c` has the
  extra 16-byte tag bytes. Other overlaps are unsupported.
- `crypto_hpke_open` requires a ciphertext of at least 16 bytes. `m` must
  have capacity for `clen - 16` bytes. The exact in-place form `m == c` is
  supported. Other overlaps are unsupported.
- Failed opens set `*mlen = 0` and do not return plaintext.
- Exporter output is limited to `255 * 32 = 8160` bytes.

## State and nonces

Each context keeps the RFC 9180 96-bit big-endian sequence number. Nonces are
`base_nonce XOR I2OSP(seq, 12)`. The sequence advances only after a
successful seal/open. Authentication failure therefore leaves the sequence
unchanged and the same valid ciphertext can be retried. At the RFC limit
(`seq == 2^96 - 1`), further seal/open operations fail rather than wrapping.

Export does not advance or depend on the sequence number. It can be called
repeatedly on either role and produces the same bytes for matching contexts.

There is no public sequence-number setter, getter, or rollback API. The
contract test uses a source-relative private structure definition to place
the counter near the RFC boundary.

## Cryptographic separation

This is RFC 9180 HPKE key scheduling with KEM suite ID
`"KEM" || I2OSP(0x0020, 2)` and HPKE suite ID
`"HPKE" || I2OSP(0x0020, 2) || I2OSP(0x0001, 2) || I2OSP(0x0003, 2)`.
Both use the RFC `HPKE-v1` labels. It does not rename or reuse
`crypto_kx` BLAKE2b session keys.

## Verification

Run:

```sh
bash support/run.sh target
```

This builds the static library with the checked-in `Makefile.in`, links
`test/default/hpke_contract.c` through `support/harness.c`, verifies the RFC
9180 Appendix A.2 base/auth vectors and behavior groups, then runs the
existing X25519, HKDF-SHA256, and ChaCha20-Poly1305 tests.
