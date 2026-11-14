# Jitter buffer playout regression checks

`checks/playout.c` exercises the SpeexDSP jitter buffer through the public
API only (`include/speex/speex_jitter.h`): out-of-order arrivals, mixed
frame lengths, same-timestamp fragments, playout gaps and packet ownership.
Run with:

    bash task_support/run.sh target      # build and run the checks
    bash task_support/variants.sh        # also run the three mutation builds

The original library must pass; each mutation must fail at a content
assertion (a `FAIL` line from `TASK_CHECK`), not at build time or by a
crash.

## Independent expectations

Nothing in the expected output is produced by the library under test:

- The checks keep their own media timeline on a 64-bit *unwrapped* axis
  plus a packet identity ledger (which interval each packet covers, which
  are still queued, and the arrival order of the puts, kept separate from
  media time). Timestamps are truncated to `spx_uint32_t` only at the
  moment they are handed to `jitter_buffer_put()`. Every script keeps all
  live timestamps within a window far smaller than 2^31 around the play
  cursor, so the unwrapped model and the wire format describe the same
  ordering. The library's time-comparison macros are never used, its
  internal state is never read, and no second instance is consulted.
- The expected result of each `jitter_buffer_get()` is derived from
  declarative interval rules over the ledger: the set of queued packets
  covering the play cursor, otherwise the earliest packet starting inside
  the consumption chunk, otherwise a gap. Scripts are built so that at
  most one candidate exists, except for same-timestamp fragments, which
  are checked as an identity set (any order of draining is legal).
  Sequence numbers tag packet identity only; nothing is sorted by them.
- Each get/get_another event records: return code, payload, sequence,
  user_data, timestamp, span, start_offset and the public play cursor
  (`jitter_buffer_get_pointer_timestamp()`). The same relative script runs
  at a plain origin (1000), an origin crossing INT32_MAX (0x7FFFFFF0) and
  an origin crossing UINT32_MAX (0xFFFFFFF0); after subtracting the
  origin, the complete event records must be identical, and each run must
  also match the model. Put entries carry no cursor (before the first get
  syncs it, the cursor is absolute 0, not origin-relative), so they log a
  sentinel instead.
- Scripts call `jitter_buffer_update_delay()` once while the timing
  history is empty (a no-op returning 0) to select manual delay mode, so
  later `jitter_buffer_tick()`/`jitter_buffer_remaining_span()` calls
  cannot move the cursor on their own.

## Cases and the behaviour they pin

- `wrap_timeline/plain_origin_model` — one playout of out-of-order,
  mixed-length frames; cursor lands on each returned packet's end;
  start_offset signs (-5 for an earlier-starting packet that still covers
  the chunk, +5/+2 for late starters); trailing `get_another` finds
  nothing and does not move the cursor.
- `wrap_timeline/shifted_origins_invariance` — the same relative script at
  the three origins above; de-shifted event records must be identical.
- `fragment_cursor/mixed_playout` — long packet first, `remaining_span`
  reported, same-timestamp fragments drained by `get_another` (identity
  set, cursor frozen), a fragment expiring exactly at the cursor swept by
  the next put, negative/positive start_offset packets.
- `fragment_cursor/playout_gap` — see below.
- `ownership/copy_mode_payload` — default copy mode: inputs rewritten
  after the put, fetched payloads are still the original bytes.
- `ownership/destroy_mode_ledger` — destroy-callback mode: zero-copy
  handoff to the caller for fetched objects, the put sweep reclaims a
  fragment that expired exactly at the cursor, reset/destroy reclaim the
  still-queued rest, and a per-object ledger proves every object ends with
  exactly one owner (the callback never frees; double reclamation would
  trip the ledger). After reset, playout resumes on a new time origin and
  no object from the old session may reappear.

## Mutation -> failing event map

- `unsigned_time` (time comparisons become plain unsigned): modular and
  unsigned ordering agree until the timeline crosses 2^32, so the plain
  and INT32_MAX origins still pass, but at the UINT32_MAX origin the
  reset sync picks the wrong "oldest" packet and the first GET returns a
  packet outside the model's candidate set
  (`wrap_timeline/shifted_origins_invariance`, `expect_has(&e, id)`).
- `fragment_anchor` (`get_another` keys on the play cursor): after a
  span-30 packet the cursor sits 30 units past the last returned
  timestamp, so the very first `get_another` either returns the next
  frame early or misses the queued fragments
  (`wrap_timeline/plain_origin_model`, then
  `fragment_cursor/mixed_playout`, `rc == e.rc`).
- `expiry_boundary` (put sweep keeps packets ending exactly at the
  cursor): the leftover fragment that ends at the cursor survives the
  sweep, so the trailing `get_another` returns it instead of MISSING
  (`fragment_cursor/mixed_playout`, `rc == e.rc`); the ownership ledger
  would also report the object as never reclaimed
  (`ownership/destroy_mode_ledger`).

## Why the gap reports MISSING, not INSERTION

`fragment_cursor/playout_gap` resets the buffer and then preloads both
packets before playout starts, so every put lands in reset state and is
stamped "no arrival", and no fetched packet can record an arrival timing.
With an empty timing history the buffer has no basis for growing the
playout delay (its computed adjustment is 0), so the hole must surface as
`JITTER_BUFFER_MISSING` whose span is one concealment unit, never as a
delay-inflating `JITTER_BUFFER_INSERTION`. Each return code is checked
individually in script order; nothing is polled past, and the model
refuses to predict a gap at all once arrival statistics may exist. The
packet after the gap then resumes normal playout.
