# Micromod instance-based player API

The original Micromod C replay (`micromod_initialise`, `micromod_get_audio`,
...) keeps all of its state in file-scope globals, so one process can only
drive one module at a time and every call implicitly targets that single
player. The functions declared below add an opaque, independently-stated
player while keeping the legacy API and its output unchanged.

## Synopsis

```c
#include "micromod.h"

unsigned flags = 0;                 /* or MICROMOD_SAMPLE_DMA */
micromod_player *player =
    micromod_player_create( module_data, module_length, 48000, flags );
if( !player ) { /* invalid/truncated data, bad rate or allocation failure */ }

short buffer[ 512 * 2 ];            /* interleaved stereo, caller-zeroed. */
memset( buffer, 0, sizeof( buffer ) );
micromod_player_get_audio( player, buffer, 512 );

micromod_player_free( player );
```

## Functions

| Function | Purpose |
| --- | --- |
| `micromod_player_create(data, length, rate, flags)` | Parse `data` and return a new independent player, or `NULL` on failure. |
| `micromod_player_free(player)` | Release a player (safe on `NULL`). |
| `micromod_player_reset(player, order)` | Restart at row 0 of `order`; an out-of-range order restarts at order 0. |
| `micromod_player_get_audio(player, output, frames)` | Render `frames` interleaved stereo frames. `NULL` output skips mixing. |
| `micromod_player_set_gain(player, gain)` | Set the playback gain (see `micromod_set_gain`). |
| `micromod_player_mute_channel(player, channel)` | Mute one channel; a negative channel un-mutes all. Returns the channel count. |
| `micromod_player_duration(player)` | Total song length in frames from order 0, without moving the instance. |

### Return and error rules

`micromod_player_create` returns `NULL` when:

* an unknown flag bit is set (only `MICROMOD_SAMPLE_DMA` is defined);
* the input is shorter than one complete module or the format is invalid;
* `rate` is below `8000`;
* `MICROMOD_SAMPLE_DMA` is requested for a module that is not four-channel;
* memory allocation fails.

`micromod_player_mute_channel` and `micromod_player_duration` return `long`;
the remaining functions return `void`.

## Input lifetime

The `data` pointer is only ever read. It must remain valid and unmodified
until **every** player built from it has been freed. Players do not take a
copy of the pattern/header bytes. In DMA mode each player additionally owns a
private, writable copy of the sample bytes, so inverse-sample writes never
reach the caller's buffer.

## Independent instances

All playback state lives inside the returned object. Two players created from
the same `data` are fully isolated: they may be driven interleaved from
several preview positions, may use different flags, and never exchange
cursors, gain, mute settings or writable samples. The legacy global API keeps
its own separate state and is not disturbed by instance players.

There is a single replay engine shared by both front-ends; the legacy API is a
thin wrapper over one static instance, so there is no duplicated playback
code and no global "current player" pointer.

## `flags = 0`: compatibility mode

With no flags the engine is bit-identical to the legacy replay:

* selecting an instrument swaps the playing sample immediately, as before;
* the `EFx` effect is ignored, as before;
* ordinary modules produce the same PCM as `micromod_get_audio`, including
  when audio is fetched in many small requests instead of one.

## `MICROMOD_SAMPLE_DMA`: segment hand-over and inverse sampling

This flag is only accepted for four-channel MOD files. It changes two things.

### 1. Three distinct sample states per channel

A channel keeps separate track of:

1. the **selected instrument** (`assigned`), changed by instrument numbers in
   rows;
2. the **active sample segment** currently being resampled, with its owning
   instrument and its fixed-point phase;
3. the **loop descriptor to reload at the next segment end**, refreshed
   whenever the selected instrument changes.

A normal note starts the selected instrument's first segment. Finetune,
volume, fixed-point step and the "sample then advance" resampling are
unchanged. The first segment ends at:

* `loop_start + loop_length` when the loop start is non-zero, or
* the full `sample_length` when the loop start is zero.

Without a valid loop the first segment is the whole sample, followed by
silence. Loop validity and the legacy loop-start correction follow the same
rules the parser already used in `micromod_initialise`.

A row that only writes an instrument number (or uses 3xx/5xx portamento
without restarting the note) updates the default volume and selection at once
but leaves the active segment and its phase untouched; only the pending loop
descriptor changes. When the active segment reaches its tail, the newest
pending descriptor is loaded. The descriptor records the owning instrument,
start and length, and is re-fetched on every subsequent segment end. The
fixed-point distance past a boundary is retained across a hand-over and may
cross several rounds without losing the fraction or inheriting the old
instrument's edges. If no loop is pending the channel stops; a later
instrument-only row cannot restart it - a new note is required. `9xx` offsets
past the first-segment tail use the same crossing rules. `EDx` updates the
three states only when the delayed note actually triggers, and `E9x`
retriggers from the selected instrument's first segment.

### 2. `EFx` inverse-sample ("funk") writes

`EFx` writes are owned by the player: channels sharing an instrument inside
one instance see each other's changes, while separate instances are isolated.
Each channel stores an inverse speed, an accumulator and a write cursor; all
three are cleared on reset. Selecting an instrument positions the cursor at
that instrument's loop start while keeping the speed and accumulator. `EFx`
sets the speed while the row is read; `EF0` stops, a non-zero speed persists
across rows, and an `EF`-only row never moves the cursor.

Rates for `x = 0..15` are
`0,5,6,7,8,10,11,13,16,19,22,26,32,43,64,128`.

At the start of every tick - including tick 0 and ticks extended by `EEx` -
the normal row/effect processing runs first, then each channel performs one
inverse step in channel order. A tick is processed once regardless of how the
audio request is split. With a non-zero speed the table value is added to the
accumulator; when it reaches 128 it is reset to zero with no retained
remainder. If the selected instrument has a valid loop, the write cursor
advances one byte (wrapping from the loop end back to its start) and that
byte is replaced by `-1 - old`. Instruments without a valid loop are not
written. The write always targets the **selected** instrument's loop, which
may differ from the instrument of the segment still playing on that channel.
Two channels inverting the same byte in one tick cancel out.

Muting a channel or passing `NULL` to `micromod_player_get_audio` only skips
mixing; clocks, segment hand-overs and inverse steps keep advancing.

## Reset and duration

`micromod_player_reset(player, order)` restarts from row 0 of the given
order, restores the player's original sample bytes and all playback state,
and retains the gain and mute configuration.

`micromod_player_duration` walks the sequence using an independent temporary
state and the existing song-end rule, returning the frame count from order 0.
It does not change the instance's cursors, writable samples, gain or mute
state and has no effect on any other instance.

## Rendering rules

* Output is the existing interleaved stereo accumulation interface; the
  caller zeroes the buffer before each call.
* Any sequence of batched requests gives identical PCM to one request for the
  same total frame count.
* A zero-frame call does nothing; a `NULL` buffer advances time silently.
* No memory is allocated and no locks are taken on the audio hot path. Only
  `create` allocates (the instance and, in DMA mode, its sample area), and
  `duration` uses a stack-held temporary state.
