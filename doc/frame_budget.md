# Per-frame bit budget (low-frequency-first allocation)

Speex's historical VBR codec splits a *bit-rate* maximum into a fixed low-band
and high-band quota. Whatever the low band does not use is lost, and the
ultra-wideband mode nests another such split. This document describes the
optional per-frame *bit budget* mode that instead allocates a deterministic
number of bits to every encoded frame and returns any unused balance to the
outer layers.

The mode is opt-in. With it disabled (`0`), every existing control and the
encoded output are unchanged, including `SPEEX_SET_VBR_MAX_BITRATE`'s fixed
low/high split.

## Controls

| Control | Type | Meaning |
| --- | --- | --- |
| `SPEEX_SET_VBR_FRAME_BITS` (48) | `spx_int32_t *` | Set the per-frame audio bit budget. `0` disables the mode. |
| `SPEEX_GET_VBR_FRAME_BITS` (49) | `spx_int32_t *` | Read the current setting (`0` when disabled). |

Both controls are accepted by the narrowband, wideband and ultra-wideband
encoders. The budget is in *audio payload bits per frame*, measured at the
encoder, and can be changed before every frame.

### What the budget counts

The budget counts the bits that one `speex_encode_int` / `speex_encode` call
actually appends to the `SpeexBits`:

* the narrowband block and every wideband/ultra-wideband mode header,
* all payload bits of all layers.

The budget does **not** count:

* bits the caller wrote into the `SpeexBits` before the call (the budget is
  relative to the current cursor, so a non-byte-aligned prefix is irrelevant),
* byte-alignment padding,
* the terminator inserted by `speex_bits_write`,
* in-band control messages.

### Accepted values

| Mode | Minimum | Maximum |
| --- | --- | --- |
| Narrowband | 43 | 4096 |
| Wideband | 47 | 4096 |
| Ultra-wideband | 51 | 4096 |

`0` always means "disabled". Any other value outside the range for the mode is
rejected with `-1` and the previous setting is kept.

### Preconditions

A positive budget requires:

* VBR or ABR enabled, and
* sub-mode encoding (`SPEEX_SET_SUBMODE_ENCODING`) enabled.

Setting a positive budget when these do not hold returns `-1`. Once the mode is
enabled:

* turning off the last effective VBR/ABR switch returns `-1` and keeps the old
  settings (turning off VBR while ABR remains is allowed),
* disabling sub-mode encoding returns `-1` and keeps the old settings.

DTX remains freely switchable.

### Interaction with `SPEEX_SET_VBR_MAX_BITRATE`

When the new mode is active and `SPEEX_SET_VBR_MAX_BITRATE` is positive, the
rate cap is converted into a whole-frame bit count

```
max_bits = floor(max_bitrate * frame_size / sampling_rate)
```

and the effective budget is `min(frame_bits, max_bits)`. The historical fixed
low/high split is **not** applied. `frame_size` is 160 for narrowband and 320
for wideband and ultra-wideband; `sampling_rate` is the encoder's current
value, so a `SPEEX_SET_SAMPLING_RATE` override affects the conversion. The new
`SPEEX_SET_VBR_FRAME_BITS` value itself does not depend on the sampling rate.

If the resulting effective budget is below the mode minimum, `speex_encode*`
returns `-1` **before** modifying the PCM input, the encoder state or the
`SpeexBits`.

## Allocation rules

Allocation is deterministic and low-frequency-first.

1. **Narrowband layer.** The ordinary VBR/VAD/DTX rules first decide the mode
   this frame *wants*. If that mode does not fit in the budget (less the empty
   high-band headers each outer layer still needs), the encoder picks the legal
   non-empty mode with the largest real bit cost that is both `<=` the desired
   mode's cost and `<=` the budget. Ties go to the smaller mode number. Mode
   numbers are **not** ordered by cost: in narrowband mode 8 costs 79 bits while
   mode 2 costs 119.
2. **Empty mode.** If the ordinary DTX decision selected the empty mode, it is
   kept; the budget rule never invents silence to make a frame fit.
3. **High-band layers.** After the low band has actually written its bits, the
   unused balance is handed to the next layer. Each sub-band encoder keeps its
   original relative-quality / energy-ratio / threshold decision about which
   mode it wants, then emits the highest high-band mode within that demand that
   fits the remaining balance. If nothing fits it emits a legal empty high band
   (mode 0). For ultra-wideband the low band is a complete wideband subtree, so
   both the inner wideband high band and the outer ultra-wideband high band are
   allocated this way.
4. Every QMF stage, LPC analysis, DTX counter and CELP history advances exactly
   once per frame; the encoder never runs a second, unconstrained encoder to
   discover a mode.

## ABR accounting

When ABR is active, the drift feedback is fed with the *actual* effective bits
of the mode that was committed for this frame, converted to a bit rate with the
corresponding sampling clock (`sampling_rate * bits / frame_size`). Fixed
per-band quotas and the rates of modes that were not used are not counted. For a
wideband or ultra-wideband encoder the outer layer reports the whole subtree's
actual frame bits.

## Failure and stream safety

The encoder budget is enforced without dropping frames, truncating encoded
bytes, modifying the PCM input, or re-running the encoder on the same frame.
The resulting stream is a normal Speex stream that the existing narrowband,
wideband and ultra-wideband decoders play continuously; caller prefix bits,
neighbouring frames and the closing terminator are preserved.

Turning the mode off restores the legacy rate-cap layering. A per-frame budget
pushed down to a sub-encoder by its parent never survives into the next frame.

## Example

See `doc/frame_budget_example.c` for a complete program. In short:

```c
spx_int32_t on = 1, budget = 300;
speex_encoder_ctl(enc, SPEEX_SET_VBR, &on);
speex_encoder_ctl(enc, SPEEX_SET_SUBMODE_ENCODING, &on);
speex_encoder_ctl(enc, SPEEX_SET_VBR_FRAME_BITS, &budget);
while (have_pcm)
{
   budget = next_frame_budget();          /* may change every frame */
   speex_encoder_ctl(enc, SPEEX_SET_VBR_FRAME_BITS, &budget);
   if (speex_encode_int(enc, pcm, &bits) < 0)
      handle_error();
}
```

Tests live in `tests/frame_budget.c` and are run by `devcheck/target.sh`.
