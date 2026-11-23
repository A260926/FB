# Pooled-sample undo for L-SMASH

This tree adds a per-track undo facility to L-SMASH's normal self-contained
avc1/mp4a writer: samples that have been appended to a track but **not yet
written out to the file** can be revoked, and new samples can be appended
again from the same decode position. The file is never reopened and no
history of appended samples is kept at the application level.

## API

```c
int lsmash_set_pool_undo( lsmash_root_t *root, uint32_t track_ID, int enabled );
int lsmash_discard_pooled_samples( lsmash_root_t *root, uint32_t track_ID,
                                   uint32_t *discarded );
```

* `lsmash_set_pool_undo` enables (`enabled == 1`) or disables (`enabled == 0`)
  the mode. Any other value is rejected. The default is disabled.
  It may be called only after the track and its sample descriptions are
  configured and **before the track accepts any sample**; calling it after a
  sample was appended returns a negative value and keeps the current mode.
  The mode supports only non-fragmented, non-external-media (self-contained)
  avc1/mp4a tracks; any other format or file mode returns a negative value.
  When disabled, the write path and its results are exactly the original ones.
* `lsmash_discard_pooled_samples` revokes every sample still sitting in the
  track's chunk pool and stores the number of revoked samples in `discarded`.
  Discarding an empty pool succeeds and reports `0`, so consecutive discards
  never subtract twice. Invalid root/track/output pointer, or a track without
  the mode enabled, returns a negative value and leaves `*discarded`
  untouched.

## Commit boundaries

A sample is **committed** exactly when its payload is written to the output
stream; everything still pooled after that point stays revocable. The
boundary is defined by actual writes, not by `lsmash_flush_pooled_samples`
alone:

* **Explicit flush** — `lsmash_flush_pooled_samples` commits the rest of the
  pool; `lsmash_finish_movie` then closes as usual.
* **Duration threshold** — a new sample whose DTS would stretch the pooled
  chunk past `max_chunk_duration` commits the pooled chunk.
* **Size threshold** — a new sample that would push the pool past
  `max_chunk_size` commits the pooled chunk.
* **Sample description switch** — a new sample with a different sample
  description index commits the pooled chunk.
* **Inter-track arbitration** — when another track's cached chunk is too far
  behind the current sample's DTS (`max_async_tolerance`), that track's pool
  is committed.

In every case the sample that *triggers* a commit belongs to the **new** pool
and remains revocable; only the previous pool is written out. Bytes already
written are never rewritten, and `lsmash_discard_pooled_samples` performs no
I/O at all (no writer or seek callbacks).

After a discard, the media state is exactly as if the revoked samples had
never been appended: stsz/stts/ctts/stss/sdtp entries, chunk tables, RAP and
roll-recovery grouping, and the muxer caches (including the last DTS/CTS) are
restored. If the first revoked sample had fixed the duration of the previous
committed sample, that derivation is rolled back too and is re-derived later
by the next appended sample or by `last_sample_delta`. Samples may then be
re-appended from the first revoked DTS with different CTS and reference
properties, and the finished file matches one written without the revoked
batch.

## Ownership and memory

* The undo state is owned by the track (allocated on enable, released on
  disable and when the track is destroyed). Callers never free it.
* Appended samples keep their usual ownership rules: `lsmash_append_sample`
  takes over the sample on success; the caller frees it only on failure.
* Extra memory used by the undo state grows only with the number of currently
  pooled samples and the number of not-yet-completed roll recovery groups.
  It does **not** copy the written sample table, does not cache the media
  payload, and does not re-read the output to rebuild state; it stays
  constant as committed chunks accumulate.
* I/O failures during normal writes are reported through the existing error
  paths; bytes already written by a failed write are not rolled back.

## Files

* `core/pool_undo.c`, `core/pool_undo.h` — implementation and internal hooks.
* `tests/pool_undo.c` — task tests (tables / boundaries / recovery).
* `examples/pool_undo.c` — revoke a trailing batch and replace it.
