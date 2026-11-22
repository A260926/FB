# LATM/LOAS otherData timing markers

## Overview

`aacEncSetLatmOtherData()` attaches a short bit string to an AAC-LC stream as
the `otherData` field of each emitted LATM/LOAS `AudioMuxElement`. The data is
transported **outside** the AAC access unit — it is written once per element,
after all subframe payloads and before the element's byte alignment — so it
never enters the raw AAC bitstream, the ancillary-data (`ancData`) path, or the
quantizer's coded spectrum.

Typical use is carrying short timing/synchronization markers that must stay
aligned with the audio element but must not change the coded audio itself.

## API

```c
AACENC_ERROR aacEncSetLatmOtherData(HANDLE_AACENCODER h,
                                    const UCHAR *data,
                                    UINT bitCount);
```

* `data` holds the marker bytes, read **most-significant-bit first**.
* `bitCount` is the number of valid bits, from `0` to `2048`. The unused low
  bits of the final byte, if any, are ignored.
* The encoder copies the bytes. After the call returns, the caller may free or
  overwrite the input buffer; the encoder keeps its own bounded copy.
* Calling with `bitCount == 0` (and `data == NULL`) removes any current marker.
* The function only stores the value; it does not restart analysis, flush, or
  reinitialize anything.

### Return values

| Return | Condition |
| --- | --- |
| `AACENC_OK` | The marker was accepted. |
| `AACENC_INVALID_HANDLE` | `h` is NULL. |
| `AACENC_INVALID_CONFIG` | `bitCount > 2048`, a non-zero `bitCount` was given with `data == NULL`, or the encoder has not completed initialization. |
| `AACENC_UNSUPPORTED_PARAMETER` | The transport is not `TT_MP4_LATM_MCP1` or `TT_MP4_LOAS`, or the codec configuration is outside the supported envelope. |

A rejected call leaves the previously active marker and all encoder state
untouched.

## Latching semantics

A marker is **pending** until the next `AudioMuxElement` actually starts, then
it becomes **active** for that whole element:

* Setting the marker while PCM is still accumulating (no complete element is
  open yet) makes the very next emitted element use it.
* Replacing it while an element is being assembled only affects the *next*
  element — never the element currently in progress. The binding is therefore
  driven by element boundaries, not by the number of `aacEncEncode()` calls,
  the amount of buffered PCM, or any output timing.
* With 1- or 2-subframe aggregation, one active marker applies to every
  subframe of the element and is written exactly once at the element's end.
* Updating the value repeatedly without an intervening element keeps only the
  latest value; no queue is formed.
* All side state is per encoder instance, so independent encoders never
  interfere.

## StreamMuxConfig behavior

The active marker's presence and length are described in the StreamMuxConfig
(SMC):

* `otherDataPresent` is set when a marker is active.
* The length is encoded in bits (not bytes):
  * **AudioMuxVersion 0** — `otherDataLenEsc` plus one or more 8-bit chunks,
    so values such as 1, 255, 256 and 2048 are all distinguishable.
  * **AudioMuxVersion 1** — a `latmGetValue()` variable-length integer.
* A **length change** forces a fresh SMC on the next element even if the
  periodic header counter would otherwise reuse the previous one.
* A **content-only change** at the same length does **not** force an SMC; the
  existing SMC already describes the length, so only the bit string changes.
* The forced SMC does not reset the periodic header counter — normal periodic
  emission continues from where it was.

## Bit budgeting

The marker bits, their SMC length field, any forced SMC and the resulting
alignment are all counted in the transport header demand **before** AAC
quantization (`transportEnc_GetStaticBits`). Nothing is inserted into the
output buffer after the fact.

Because the per-element CBR budget is fixed, the additional transport overhead
is taken from the room available to the AAC core: the quantizer's per-frame
minimum is relaxed (and, at a saturated operating point with no bit-reservoir
capacity, its floor is dropped) so the core shrinks by the marker cost rather
than producing an over-long frame or draining the bit reservoir. The legacy
`transportEnc_LatmAddOtherDataBits()` path and its "reserved bits minus 9"
behavior are unchanged.

## Flushing and state

At end of input, flushing with `numInSamples == -1` works as usual. Elements
emitted during the flush still carry the active marker; no extra frames are
synthesized for the side band, and the encoder delay/bit-reservoir state is
not reset by marker updates. Removing the marker (`bitCount == 0`) simply makes
later elements omit `otherData`.

## Supported configuration

The feature is validated for AAC-LC, mono or stereo, 1024 samples/frame,
CBR 192 kbit/s, sample rates 32 / 44.1 / 48 kHz, with `TT_MP4_LATM_MCP1` or
`TT_MP4_LOAS`, AudioMuxVersion 0 or 1, and 1 or 2 transport subframes.
RAW, ADIF and ADTS outputs are unchanged and reject the call.

## Example

See `examples/latm_other_data.c` for a complete encoder program that attaches
a 96-bit marker, emits LOAS elements, and then removes the marker.
