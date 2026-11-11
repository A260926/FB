# Delay0r

Video delay. The plugin keeps a short history of the actual input frames and
outputs the frame closest to the requested target time.

Written by Martin Bayer and released under the GNU GPL.

## Parameters

1. **DelayTime** (double, default 0.0): the delay in seconds, interpreted over
   the range 0..1 second.

## Frame selection

For an update at time `t` the target time is `t - DelayTime`. The plugin
outputs the newest retained frame whose timestamp is not later than the
target. If no frame that old exists yet, the oldest retained frame is used
instead; with zero delay this naturally selects the current frame. Frames are
never synthesized, so uneven time intervals simply leave gaps in the history.

## Retention and parameter animation

To allow **DelayTime** to be animated upwards at any moment, the plugin
retains every actual frame of the most recent one second, plus the single
frame immediately preceding that second as a boundary support. Frames older
than this range are reclaimed as new frames arrive. Retention is therefore
independent of the currently configured delay: a short delay does not cause
in-range frames to be discarded early.

Every retained frame is a private copy owned by the plugin; the host may
reuse or overwrite its input buffer as soon as the update call returns.

## Host time behavior

The video time passed to `f0r_update` follows the same rules as the other
temporal filters in this package:

- a time strictly greater than the previous one appends a new frame;
- the exact same time replaces the current frame, so a redraw is stored once
  and does not occupy an extra history slot;
- a time smaller than the previous one starts a new playback segment and
  discards all previously retained frames;
- a non-finite time (NaN or infinity) is handled as a standalone frame: the
  input is passed through to the output, nothing is retained, and the next
  finite time starts a fresh history.

Input and output buffers may overlap (in-place updates), multiple instances
may be interleaved, and destruction releases every retained frame copy.
