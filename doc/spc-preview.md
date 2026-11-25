# SPC speculative preview (`Spc_Preview`)

`Spc_Preview` renders a short, editable preview of an SPC image. It reuses the
existing fast SPC700/DSP core (`Snes_Spc`), the SNES output filter
(`SPC_Filter`) and the 24-tap FIR resampler (`Fir_Resampler<24>`); no new
decoder, device access or external dependency is introduced.

The class lives in `gme/Spc_Preview.h` / `gme/Spc_Preview.cpp` and is compiled
into `gme_static`. It is non-copyable, default-constructible and destructible.

## When to use it

A normal `Spc_Emu` render is a one-way stream: once samples have been produced
the input that shaped them cannot be changed. A preview UI wants to

* render a few milliseconds of audio ahead of time, and
* let the user edit CPU input-port writes that have not been handed to the
  device yet,

without re-rendering (or re-decoding) the audio that was already delivered.

`Spc_Preview` keeps the delivered prefix immutable and rebuilds only the
not-yet-delivered tail when an edit lands inside it.

## Public interface

```cpp
struct Status {
    uint64_t delivered_frames;          // stereo frames returned by read()
    uint64_t committed_native_frame;    // 32 kHz frame reached for delivered audio
    uint64_t speculative_native_frame;  // 32 kHz frame reached for pending audio
    uint64_t synthesized_native_frames; // total native frames ever synthesized
    uint64_t rerenders;                 // how often the pending tail was rebuilt
    unsigned pending_frames;            // undelivered stereo frames buffered
    unsigned queued_events;             // accepted events not yet committed
};

blargg_err_t begin(const void* data, long size, long rate, unsigned lookahead);
blargg_err_t schedule_port(uint64_t native_frame, unsigned port, unsigned value);
blargg_err_t prefetch();
blargg_err_t read(short* out, long frames);
Status       status() const;
void         reset();
```

All calls are serial; there is no internal locking.

### `begin`

Copies the complete SPC image and starts track 0 with tempo 1.0, the default
gain (1.4), accurate filtering enabled and silence checking disabled; no
track-length fade is applied.

* `rate` must be 32000, 44100 or 48000.
* `lookahead` (output frames kept ready by `prefetch`) must be 512..4096.

The replacement engine is fully constructed before any live state is touched,
so a failed `begin()` leaves the previous playback intact.

### `schedule_port`

Queues a write of `value` (0..255) to CPU input port 0..3. The write is
performed through the real `Snes_Spc::write_port` immediately before native
32 kHz frame `native_frame` is generated, so the value is read by the emulated
CPU at its own instruction time. Values are never added to PCM samples or DSP
registers.

* Frames are timed in 32 kHz native frames, not output frames.
* Events for one frame are applied in ascending port order.
* Only the last value per `(frame, port)` is kept; re-scheduling the same value
  is a no-op.
* Accepted frames satisfy `committed_native_frame <= native_frame < 2^30`.
  Stale frames, invalid arguments and a queue already holding 256 distinct keys
  fail without changing any state.

### `prefetch`

Fills the pending buffer up to `lookahead` output frames using a speculative
engine. It never changes `delivered_frames` or `committed_native_frame`. When
the buffer is already full and no new event invalidated it, a second call does
no work and synthesizes nothing.

### `read`

Delivers 1..4096 complete stereo frames, rendering more if the pending buffer
is short, and advances the committed boundary. `frames == 0` is a complete
no-op (the buffer pointer may be null). Negative or oversized requests, and a
null buffer with a positive frame count, fail without advancing.

## Commitment, speculation and rollback

Two frame counters describe progress, both in 32 kHz native frames:

* **`committed_native_frame`** is the boundary reached while producing audio
  that has already been delivered. At resampled rates it includes the native
  look-ahead the FIR consumed to produce those output frames, so it is not
  `delivered_frames * 32000 / rate` and it never moves during speculation.
* **`speculative_native_frame`** is where the engine that fills the pending
  buffer currently stands; it is always `>= committed_native_frame`.

When an event is scheduled at a frame the speculative engine has already
crossed (`native_frame < speculative_native_frame`), the pending audio can no
longer be trusted:

1. the buffered PCM is discarded (`pending_frames = 0`),
2. the whole engine site — SPC700 CPU/RAM/timers, DSP BRR/envelope/echo
   state, the SNES filter history and the FIR fractional phase and input
   history — is restored to the committed site,
3. `speculative_native_frame` returns to `committed_native_frame` and
   `rerenders` is incremented.

The next `prefetch()`/`read()` re-synthesizes the tail with the new event.
Rendering never continues from the current chip and the buffer is never
patched in place, so the result is sample-identical to a synchronous render
that knew every accepted event ahead of time. Events entirely beyond the
speculative frontier leave the rendered prefix untouched.

`synthesized_native_frames` accumulates every positive native-frame count
handed to `Snes_Spc::play`, including re-renders and the replay used to move
the committed boundary; it never decreases on a rollback. After an
event-triggered rebuild, the new synthesis for one `prefetch` is bounded by
`ceil(lookahead * 32000 / rate) + 3200` native frames.

### Engine snapshots

The engine components hold internal pointers (the DSP points into the SPC700
RAM and its own echo/counter tables; the FIR points into a heap input buffer).
Cross-object copies would therefore dangle. Instead `Spc_Preview` owns one
live engine at a stable address and stores byte snapshots of it, always
restoring a snapshot back into that same object. The object and its FIR
allocation are never reallocated, so every pointer stored in a snapshot stays
valid on restore; the FIR heap buffer contents are saved alongside the engine
bytes. No playback history is retained and nothing is replayed from the file
header.

## Resource use

At most the following are retained: the current/committed/scratch engine
sites (each far below 512 KiB), `lookahead + 4096` frames of pending PCM, one
copy of the input SPC and up to 256 events. No random-access index and no
on-disk snapshot format is maintained, and the legacy `Spc_Emu` path and the
fast DSP are unchanged.
