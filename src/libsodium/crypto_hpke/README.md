# HPKE (RFC 9180) support

This directory contains a self-contained implementation of Hybrid
Public Key Encryption as defined in [RFC 9180](https://www.rfc-editor.org/rfc/rfc9180),
built exclusively on top of the library's existing X25519, HKDF-SHA256
and ChaCha20-Poly1305 primitives.

It is **not** `crypto_kx`: the `crypto_kx` BLAKE2b session key is a
different construction.  Nothing produced by `crypto_kx` is reused or
renamed here; every key below follows the labeled-extract/expand
schedules of RFC 9180.

## Supported scope

Exactly one ciphersuite is implemented:

| Component | Value | Identifier |
|-----------|-------|------------|
| KEM  | DHKEM(X25519, HKDF-SHA256) | `0x0020` |
| KDF  | HKDF-SHA256               | `0x0001` |
| AEAD | ChaCha20Poly1305          | `0x0003` |

Only the base mode (`0x00`) and the authenticated mode (`0x02`) are
provided.  PSK modes, PSK/auth-PSK combinations, network transport,
certificate management and the export-only AEAD ID are out of scope.

The KEM-side labeled functions use `suite_id = "KEM" || 0x0020`; the
key-schedule and exporter labeled functions use
`suite_id = "HPKE" || 0x0020 || 0x0001 || 0x0003`.  Both domain
separators are kept distinct, as required by Sections 4.1 and 5.1.

## Public API

All declarations live in `include/sodium/crypto_hpke.h`; the opaque
context type is `crypto_hpke_ctx`.

* `crypto_hpke_sender_setup()` — `SetupBaseS` / `SetupAuthS`.
  * `skS == NULL` selects base mode; a 32-byte sender static private
    key selects authenticated mode.
  * `ikmE` is fed to the RFC 9180 X25519 `DeriveKeyPair()` function
    (labeled HKDF, then X25519 scalar clamping); it is never used
    directly as an X25519 private key.  `ikmE == NULL` with
    `ikmE_len == 0` first draws 32 random bytes, matching
    `GenerateKeyPair = DeriveKeyPair(random(Nsk))`.
  * In auth mode both DH values (`DH(skE, pkR)` then `DH(skS, pkR)`)
    and the ordered `kem_context = enc || pkR || pkS` are bound.
* `crypto_hpke_receiver_setup()` — `SetupBaseR` / `SetupAuthR`, with
  `pkS == NULL` selecting base mode analogously.
* `crypto_hpke_seal()` — sender only;
  `crypto_hpke_open()` — receiver only.  Using the wrong role returns
  `-1`.
* `crypto_hpke_export()` — identical output on both roles, never
  advances the sequence number, bounded to 255 * 32 bytes.
* `crypto_hpke_free()` — scrubs key material and frees the context;
  accepts `NULL`.

Setup functions allocate the context only after every cryptographic
step has succeeded.  On failure they return `-1` and set `*out` to
`NULL`, so a caller can never observe a partially derived context.
Invalid public keys, all-zero X25519 DH outputs and unrepresentable
lengths all fail before publication.

## Context semantics

* `info`, `aad` and `exporter_context` are length-prefixed raw byte
  strings; embedded NUL bytes are significant.
* The per-message nonce is `base_nonce XOR I2OSP(seq, 12)` over a
  96-bit sequence number that starts at zero and advances only after a
  successful seal/open.  At the message limit the next call fails; the
  counter never wraps.
* AEAD authentication failure returns `-1` with an output length of
  zero, does not advance the sequence number, and leaves the context
  usable: the next valid ciphertext still opens.
* Zero-length messages and zero-length AD are accepted.
* Seal/open buffers may be the same pointer for in-place operation
  (the ciphertext is exactly `mlen + 16` bytes); other partial overlap
  is not supported.
* There is no public interface to inspect or modify the sequence
  number.  A private, non-installed test hook
  (`include/sodium/private/hpke_testonly.h`) exists solely so the test
  suite can reach the message limit without sealing 2^64 messages.

## Interoperability

`test/default/hpke_contract.c` checks the public Appendix A.2
ChaCha20Poly1305 vectors for both base and authenticated mode,
including `enc`, the sequence of ciphertexts and the exported values,
so correctness is not established only by the library talking to
itself.
