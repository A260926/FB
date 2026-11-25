# Layered SMF Format 2 playback

`adl_setSMF2Layered()` enables an optional scheduler for Standard MIDI File
format 2:

```c
int adl_setSMF2Layered(ADL_MIDIPlayer *device, int enabled);
```

Pass `1` to layer the SMF2 tracks on one shared time line and OPL voice pool,
or `0` to explicitly keep the legacy behaviour. The function returns `0` on
success and `-1` for a null player, a value other than `0` or `1`, or a player
on which music has already been loaded successfully. Failed load attempts do
not count as a successful load. The setting remains `0` by default.

Enabling the mode does not change SMF0, SMF1, or any other supported format.
When layered loading is disabled, all existing code paths remain in use.

## Supported SMF2 subset

This implementation accepts standard SMF2 files with:

* PPQN division from `1` through `9600`;
* one through eight tracks;
* ordinary MIDI channel events and SysEx;
* usual non-tempo meta events such as text, marker, time signature, and key
  signature events.

It rejects:

* SMPTE divisions;
* loop markers, including the library's textual `loopstart` / `loopend`
  markers and controller-based loop markers;
* device/port switching meta events (`FF 09` and the obsolete port event
  `FF 21`).

Rejected layered SMF2 data makes `adl_openData()` or `adl_openFile()` fail.
The layered configuration itself remains enabled and can be used for another
load attempt.

## Independent tempo maps and time line

Every track starts at its own tick zero and uses `500000` microseconds per
quarter note until that track's first Set Tempo (`FF 51`) event. A tempo event
only changes the conversion of its own track from its tick onward. If several
tempo events occur at the same tick in one track, the last event in the track's
original event order is used.

All tracks are converted to one seconds timeline. Live events are submitted in
this order:

1. absolute time;
2. original track number;
3. original in-track order.

A finished track does not stop the other tracks. `adl_totalTimeLength()` is the
largest independent track end time plus the synthesizer's existing one-second
post-song wait. `adl_positionTell()`, `adl_positionSeek()`, and rewind use this
same seconds timeline.

Events are assigned to the first output frame that is not earlier than their
calculated time, i.e. an exact integer frame or the ceiling of a fractional
frame. The due frame is retained until the event is handled; small or varying
pull sizes therefore do not round the same interval repeatedly and accumulate
drift. The output is independent of whether callers request one large block or
a sequence of `1`, `7`, and `113` frame blocks.

`adl_setTempo()` remains a multiplier. Changing it during playback keeps the
already delivered frame position and continues from the current musical-time
position; it never rewrites the parsed tempo maps.

## Ports and voices

Track `N` uses the fixed MIDI port name `SMF2/N`, through the library's normal
multi-port channel mapping. Its original MIDI channel `0..15` is preserved.
Consequently, key state, program, bank, volume, pan, pitch bend, and pedals use
the existing independent port/channel state. The existing multi-port allocator,
physical OPL channel pool, and voice stealing rules remain in effect; channel
numbers are not rewritten to force separation.

There is still one `MIDIplay`/sequencer environment and one OPL synthesis pool
for an `ADL_MIDIPlayer`. Tracks are not rendered by separate players and mixed
afterwards, and no temporary SMF0/SMF1 file is created or reopened.

## Seek and rewind

Seek first performs the existing panic action. Historical note-on events are
ignored while events from each track are revisited to restore controllers,
programs, bank state, pitch bend, pedals, and related state. Every track then
resumes at its own next event using its own tempo map.

Rewind re-establishes every track's tick-zero position. Both seek and rewind
clear the legacy pull state (`delay`, `carry`, and the outstanding sample-skip
count) so the next pull cannot emit a stale fragment from the previous output
position.

Separate `ADL_MIDIPlayer` instances have independent schedulers, port maps,
clocks, and output buffers. Reloading one instance replaces its layered song
state without affecting other instances.

## Source and verification

The scheduler lives in:

* `src/adlmidi_smf2.hpp`
* `src/adlmidi_smf2.cpp`

The required content tests are in `tests/smf2_layered.cpp` and register the
`clocks`, `ports`, and `transport` groups with `task_support/check.h`.

A small offline writer/player is provided in `examples/smf2_layered.cpp`. It
writes raw stereo signed 16-bit little-endian PCM, without a WAV header.
