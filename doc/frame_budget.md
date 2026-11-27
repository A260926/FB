# Per-frame VBR/ABR bit budget

Two public encoder controls add a hard, deterministic bit allowance to each
encoded audio frame:

```c
#define SPEEX_SET_VBR_FRAME_BITS 48
#define SPEEX_GET_VBR_FRAME_BITS 49
```

The argument is an `spx_int32_t *`, and the default value is 0 (disabled).
The controls are supported by the floating-point build for the narrowband,
wideband and ultra-wideband modes and are single-channel only.

## What the budget counts

The budget is the number of **audio bits that one encode call appends to the
`SpeexBits` structure**:

* the narrowband CELP frame, including the narrowband sub-mode header, and
* the mode header of every higher layer in a WB/UWB frame.

It does **not** count:

* bits the caller packed before the call (a non-byte-aligned prefix does not
  change the allowance),
* byte-alignment padding,
* the stream terminator written by `speex_bits_write()`, or
* in-band control messages.

## Accepted values

| mode | minimum | maximum |
|------|---------|---------|
| NB   | 43      | 4096    |
| WB   | 47      | 4096    |
| UWB  | 51      | 4096    |

Anything else returns -1 and leaves the current value untouched.

A positive value is accepted only while VBR or ABR is enabled and submode
encoding is on; otherwise the request is rejected and the current configuration
is kept.  The budget may be changed between frames.

Once a positive budget is active, the last remaining VBR/ABR switch cannot be
turned off, and submode encoding cannot be turned off.  Those requests are
rejected (the setting is kept).  DTX may still be enabled and disabled freely.

Setting the budget back to 0 restores the legacy behaviour exactly: the old
`SPEEX_SET_VBR_MAX_BITRATE` layered split and all encoder output are unchanged.

## Interaction with `SPEEX_SET_VBR_MAX_BITRATE`

When a legacy maximum bit-rate is positive it is folded into whole-frame bits
using the layer's clock (`floor(max_bitrate * frame_size / sampling_rate)`) and
the smaller of the two limits is used.  The old fixed low/high-band split is
not applied in this case.  The explicit bit budget itself does not depend on
the sampling-rate setting; a sampling-rate override only changes how the
legacy bit-rate limit converts.

If the effective allowance cannot hold even the smallest frame, `encode`
returns -1 **before** touching the PCM, the encoder state or the `SpeexBits`
stream.

## Allocation rule

The allocator is deterministic and allocates low frequencies first:

1. Every outer layer that has not yet been encoded reserves 4 bits for an empty
   high-band header.
2. The inner layer encodes within the remaining allowance.
3. The bits it does not use are returned to the outer layers.
4. Each outer layer then picks the highest high-band mode that fits both the
   remaining allowance and the mode its own analysis asked for; if none fits it
   writes a legal empty high band.

For the narrowband layer, the existing VBR/VAD/DTX analysis first picks the
desired mode.  If it does not fit, the mode with the most bits that does fit
(bounded by the desired mode) is chosen, with ties going to the smaller mode
number.  Mode 0 is only used when the DTX logic itself selected silence; an
undersized allowance never forces a silent frame.

The UWB low layer is treated as a complete WB sub-tree, so its full cost is
accounted for, not just the NB block.  QMF, LPC, DTX counters and CELP history
advance exactly once per frame.

ABR continues to adapt its quality with the legacy control logic, but the drift
feedback for a budgeted frame is computed from the bits of the mode actually
selected for that frame, converted at the corresponding sampling clock.  The
parent aggregates the whole sub-tree; fixed quotas and unused modes are not
counted.

The constraint is met without dropping frames, truncating encoded bytes,
modifying the PCM, or re-running the encoder over the same frame.
