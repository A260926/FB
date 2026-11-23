# Packet loss concealment for libgsm (`gsm_decode_plc`)

## Block entry

```c
int gsm_decode_plc(gsm s, const gsm_byte *data, unsigned int bytes,
                   unsigned int good_mask, gsm_signal *pcm,
                   unsigned int capacity);
```

`gsm_decode_plc()` decodes one *complete compressed block* and always
fills a whole number of audio samples, so a player that is missing GSM
06.10 frames can keep advancing its audio time axis.  It returns the
number of samples written, or `-1` on error.  On error nothing is
written to `pcm` and no decoder state changes, so a rejected call can
be retried safely.

This is a deterministic, self-contained concealment policy.  It does
not claim to implement any external PLC standard.  Only the integer
code path is supported; `FAST`, `USE_FLOAT_MUL` and `LTP_CUT` are not
extended.

## Block units

| Mode   | Block size | Samples | `good_mask` bits                                   |
|--------|-----------:|--------:|----------------------------------------------------|
| Normal | 33 bytes   | 160     | bit 0 marks the single frame good                  |
| WAV49  | 65 bytes   | 320     | bit 0 = first 160-sample half, bit 1 = second half |

- A set bit means that half frame is available; a clear bit means it
  is lost and must be concealed.
- If at least one half is good, `data` must point to a full block
  (`bytes` is 33 or 65) and the entry always produces the complete
  block (160 or 320 samples).
- If every half is lost (`good_mask == 0`), a full block may still be
  supplied for concealment, or `data` may be `NULL` with `bytes == 0`.
- Normal mode only honors bit 0; WAV49 honors bits 0 and 1.  Any other
  set bit makes the mask out of range and is rejected.

### Validation (all checked before any state or output changes)

- `good_mask` outside the valid bits,
- `bytes` or `capacity` that do not match the mode's block size,
- a `NULL` output buffer,
- a bad magic nibble in a good ordinary frame,
- a WAV49 decoder that is parked in the middle of a half block
  (i.e. its `frame_index` is non-zero).  The block entry only ever
  consumes whole blocks and leaves the decoder on a whole-block
  boundary.

### The shared WAV49 byte

A 65-byte WAV49 block packs two 33-byte-equivalent frames and shares
one byte (block byte 32) between them.  Its high nibble carries the
bits the second (odd) half needs to reconstruct its `LARc[0]` low
bits; the low nibble holds the first (even) half's tail bits.  The
block entry reads the second half's carry straight from that high
nibble, so a good second half is decoded correctly even when the first
half was lost — the bad first half is never decoded just to advance the
chaining state.

## What a good frame records

For every normally decoded 160-sample frame the decoder caches, in a
fixed-size structure attached to the opaque `gsm` state:

- the eight coded `LARc`,
- for each of the four subframes, the effective `Nc` (the coded lag
  after the library's 40..120 range check and `nrp` fallback), the
  coded LTP gain `bc`, and the 40-point RPE-dequantized excitation.

The cache holds exactly one most-recent-good frame; it never grows with
the stream length.  Concealed frames do not overwrite it.  When a good
frame arrives, that half frame's consecutive-loss counter is cleared.
WAV49 keeps two independent counters, one per half frame.

The legacy `gsm_decode()` maintains the same cache, so the old entry and
`gsm_decode_plc()` can be interleaved at frame boundaries and recovered
audio continues from the concealed prediction, short-term and
deemphasis state.  With no losses the block entry is sample-for-sample
identical to the legacy entry.

## Concealment strategy

Let `n` be the 1-based number of consecutive lost frames for the half
frame being concealed.  The Q15 attenuation `g` is:

| `n`            | 1      | 2      | 3      | 4      | 5     | 6 and later |
|----------------|-------:|-------:|-------:|-------:|------:|------------:|
| `g`            | 29491  | 26214  | 19661  | 13107  | 6554  | 0           |

For each subframe, using the last good frame's `Nc` and `bc` and the
*current* long-term history, the long-term prediction is computed
sample by sample with the library's `QLB` table and its usual rounding
and saturation:

```
p   = GSM_MULT_R(QLB[bc], history[k - Nc])
r_k = sat16( q15(last_excitation[k], g) + q15(p, g) )
q15(x, g) = floor((x*g + 16384) / 32768)
```

The residual `r_k` generated in this subframe immediately enters the
history and is used by the remaining samples, exactly as in the normal
long-term synthesis filter.  The resulting 160 residuals then go through
the ordinary short-term synthesis filter and deemphasis, reusing the
last good frame's `LARc`.

- When `g == 0` (the sixth and later losses) the new residual is zero,
  but the synthesis and deemphasis filters keep ringing; the PCM is not
  forced to zero.
- With no good frame in history, concealment starts from a zero
  residual and all-zero `LARc`, and the filters are still advanced.

## Files

- `inc/gsm.h` — public declaration of `gsm_decode_plc()` (the `gsm`
  type stays opaque).
- `inc/private.h` — fixed-size PLC bookkeeping added to the internal
  `gsm_state`.
- `src/plc_frame.c` — parameter unpacking, the shared good-frame decode
  and cache update, concealment, the block entry, and the helpers used
  by the legacy `gsm_decode()`.
- `src/gsm_decode.c` — now delegates to the shared engine so both
  entries maintain identical state.
- `tests/plc_blocks.c` — `Excitation`, `Packing` and `Recovery` tests
  with an independent wide-integer reference.
- `examples/plc_blocks.c` — runs a frame sequence with losses in both
  modes.
