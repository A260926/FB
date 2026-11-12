# Hashing to edwards25519 (RFC 9380)

This module adds the random-oracle suite
`edwards25519_XMD:SHA-512_ELL2_RO_` from [RFC 9380](https://www.rfc-editor.org/rfc/rfc9380.txt)
to the library. It maps a domain-separated, arbitrary-length message to a
point on edwards25519 so a local protocol can reuse the existing group
operations. Only this random-oracle (`RO`) suite is implemented; the
non-uniform (`NU`) suite and ristretto255 are deliberately not supported.

## Pipeline

The implementation runs entirely on the library's own SHA-512, field (`fe`)
and group (`ge`) code:

1. `expand_message_xmd` with SHA-512 (block size 128, output 64), built per
   RFC 9380 §5.3.1, produces 96 uniform bytes.
2. `hash_to_field` (§5.2) splits those into two 48-byte big-endian integers
   and reduces each modulo the field prime `p = 2^255 - 19` using
   `fe_fromwide`. The full 384-bit width is consumed; the integers are never
   reduced modulo the group order `l` (`sc_reduce` is not used) and high
   bytes are never truncated.
3. Each field element `u` is mapped with Elligator 2 on curve25519
   (Appendix G.2.1) and then through the birational map onto edwards25519
   (Appendix G.2.2), giving `Q0` and `Q1`.
4. The points are added (`R = Q0 + Q1`) and the cofactor is cleared with
   `h_eff = 8` (`P = 8 * R`).

The G.2.1 straight-line formulas never divide, so the `Z*u^2 == -1`
exception and `u = 0` need no error path. The only zero denominator that can
occur is in the G.2.2 rational map at `u = 0`; the standard `CMOV` exception
selects the identity point rather than dividing by zero or emitting an
invalid coordinate. The sign of the `sqrt(-486664)` constant is fixed even
(`sgn0 == 0`), so points carry the suite's required sign; nothing negates the
result as a whole (in contrast to `ge_frombytes_negate_vartime`).

## Point encoding

`ed25519_hash_to_curve` and `ed25519_h2c_final` write 32 bytes in the standard
RFC 8032 compressed Edwards encoding: the affine `y` coordinate in
little-endian, with the sign (parity) of `x` in the most significant bit of
the last byte. This is the same encoding used for Ed25519 public keys.

## API

```c
int ed25519_hash_to_curve(unsigned char *out,
                          const unsigned char *message, size_t message_len,
                          const unsigned char *dst, size_t dst_len);

int ed25519_h2c_init  (ed25519_h2c_ctx *ctx,
                       const unsigned char *dst, size_t dst_len);
int ed25519_h2c_update(ed25519_h2c_ctx *ctx,
                       const unsigned char *message, size_t message_len);
int ed25519_h2c_final (ed25519_h2c_ctx *ctx, unsigned char *out);
```

`out` must point to 32 writable bytes. All functions return `0` on success
and `-1` on a parameter or state error. On failure the output buffer is never
modified.

### Length and pointer rules

- A `NULL` data pointer is accepted only at length zero. An empty message
  (`message_len == 0`) and an empty DST (`dst_len == 0`) are both allowed.
- DSTs longer than 255 bytes are shortened per §5.3.3
  (`SHA-512("H2C-OVERSIZE-DST-" || DST)`); they are never silently truncated,
  so a 256-byte DST and its 255-byte prefix produce different points.
- The running message length and the SHA-512 64-bit bit-counter limit are
  checked in `ed25519_h2c_update` before any byte is absorbed. An
  out-of-range update fails without consuming input.

### Streaming state rules

- `ed25519_h2c_init` copies the (possibly shortened) DST into the context.
  The caller's DST buffer can be reused or overwritten afterwards; no
  external pointer is retained.
- `ed25519_h2c_update` feeds fragments straight into the SHA-512 state. It
  never assembles the full message and never stores the message pointer, so
  any chunking — including zero-length updates — yields exactly the same
  point as the one-shot call.
- After a successful `ed25519_h2c_final`, the context leaves the active state;
  a further `update` or `final` returns `-1`.
- Any parameter error on an active context (bad pointer, length overflow,
  `final` with a `NULL` output) poisons it: subsequent `update`/`final` calls
  keep returning `-1` and no partial result is published. Only a fresh
  `ed25519_h2c_init` makes the context usable again.

### Memory bounds

`ed25519_h2c_ctx` is a fixed-size, caller-allocated struct. Its size does not
grow with the message or the DST: it contains one `sha512_context`, a
255-byte private DST buffer with a one-byte length, the message counter and a
state byte. The module performs no heap allocation, uses no system randomness
and adds no dependency beyond the existing library sources. Two initialized
contexts can be updated alternately without interfering, because all state is
kept inside the struct.

## Compatibility

The new code is additive. Signature creation/verification, key exchange,
scalar addition and all existing key formats are unchanged; no existing
public interface was modified. The testing-only helpers
(`ed25519_h2c_test_*`) exist solely under `ED25519_H2C_TESTING` and are absent
from normal builds.
