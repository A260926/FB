# Stream encoder checkpoint persistence

This document describes the on-disk format produced by
`FLAC__stream_encoder_checkpoint_write()` and consumed by
`FLAC__stream_encoder_checkpoint_resume()`, both declared in
`include/FLAC/stream_encoder_checkpoint.h`.

A checkpoint captures the exact state of a **native** FLAC stream encoder
between two `FLAC__stream_encoder_process*()` calls, before
`FLAC__stream_encoder_finish()`.  Resuming appends the remaining audio to the
*already written* FLAC stream, producing a file byte-for-byte identical to an
uninterrupted encode of the same input with the same build and settings.

## Properties

* Saving is a pure read: it does not advance the encoder, does not invoke the
  write or seek callbacks, and does not flush the final partial block.  The
  tell callback may be called to record the current byte position.  The same
  live encoder may be saved repeatedly; identical state yields identical
  bytes.
* The saved size depends only on the configuration, the number of pending
  (uncommitted) PCM samples, and the number of seek points.  It is independent
  of how many samples have already been encoded.
* No pointers, addresses, file names, callback addresses, or other in-memory
  details are persisted.

## Supported configurations

`FLAC__stream_encoder_checkpoint_size()` returns 0 (and
`..._checkpoint_write()` returns false) for any encoder that does not match
the following.  All other configurations continue to encode normally through
the classic interface but cannot be checkpointed.

* Single-threaded (`num_threads == 1`), native FLAC (not Ogg).
* `verify == false`, `do_md5 == true`, `streamable_subset == true`.
* Compression level 5 defaults with no overrides: max LPC order 8, apodization
  `tukey(5e-1)`, no QLP precision search, no escape coding, no exhaustive
  model search, residual partition orders 0..5, no min-bitrate limiting, and
  no instruction-set or subframe-type overrides.
* 1 or 2 channels; 16 or 24 bits per sample; 44100 or 48000 Hz.
* Explicit blocksize of 256, 1024, or 4096.
* Metadata restricted to the implicit default STREAMINFO plus at most one
  SEEKTABLE with at most 64 points.
* The encoder must be in the `FLAC__STREAM_ENCODER_OK` state, i.e. after a
  successful `init` and after `process`/`process_interleaved` returned true,
  and before `finish` (or after a resumed encoder was initialized).  The
  number of buffered samples may be anything from 0 up to `blocksize`,
  including exactly a full block plus the one-sample look-ahead that the
  encoder keeps internally.

## Endianness and field encoding

All multi-byte integers are unsigned little endian, fixed width.  Offsets are
counted from the beginning of the FLAC stream (byte 0 is the `fLaC` marker).

## Layout

| Offset | Size | Field |
|-------:|-----:|-------|
| 0  | 8  | magic, the ASCII bytes `FLACCKPT` |
| 8  | 4  | format version, currently `1` |
| 12 | 4  | total checkpoint size in bytes (must equal the supplied `size`) |
| 16 | 4  | flags; bit 0 (`0x1`) means MD5 is enabled, other bits must be 0 |
| 20 | 4  | channels |
| 24 | 4  | bits per sample |
| 28 | 4  | sample rate |
| 32 | 4  | blocksize |
| 36 | 4  | STREAMINFO min blocksize scratch value |
| 40 | 4  | STREAMINFO max blocksize scratch value |
| 44 | 4  | STREAMINFO min framesize scratch value (low 32 bits of the u64 below) |
| 48 | 4  | STREAMINFO max framesize scratch value (low 32 bits of the u64 below) |
| 52 | 8  | total samples estimate that was configured |
| 60 | 8  | bytes written so far (the output position) |
| 68 | 8  | samples written so far |
| 76 | 4  | frames written high-water mark |
| 80 | 4  | index of the first seek point not yet filled in |
| 84 | 4  | current buffered sample count (0..blocksize) |
| 88 | 4  | current frame number (frames fully encoded) |
| 92 | 8  | STREAMINFO metadata block offset |
| 100 | 8 | SEEKTABLE metadata block offset (0 if none) |
| 108 | 8 | audio frames offset |
| 116 | 8 | STREAMINFO total_samples scratch value (equals samples written) |
| 124 | 4 | STREAMINFO min framesize scratch value |
| 128 | 4 | STREAMINFO max framesize scratch value |
| 132 | 16 | STREAMINFO MD5 scratch value |
| 148 | 4 | MD5 byte counter low word |
| 152 | 4 | MD5 byte counter high word |
| 156 | 4 | MD5 internal block size, always 64 |
| 160 | 4 | reserved, must be 0 |
| 164 | 16 | MD5 internal state `buf[4]`, one u32 each, little endian |
| 180 | 64 | MD5 internal input buffer (`in[16]`), raw bytes; bytes at and after `bytes_low & 63` are zero |
| 244 | 4 | number of seek points `N` (0..64) |
| 248 | 20*N | seek points, each: sample number (u64), stream offset (u64), frame samples (u32) |
| 248+20N | 8 | pending PCM byte count (= `P * channels * 4`) |
| 256+20N | 8 | pending sample count for channel 0 |
| 264+20N | 8 | pending sample count for channel 1 (0 for mono) |
| 272+20N | 8 | pending MD5 byte count (= `P * channels * bytes_per_sample`) |
| 280+20N | 4 | pending bitwriter bytes, always 0 |
| 284+20N | 4 | reserved, must be 0 |
| 288+20N | 4*channels*P | uncommitted PCM, channel-major, one native 32-bit little-endian sample per entry |
| 288+20N+4*channels*P | 4 | CRC-32 (IEEE 802.3, reflected) of all preceding bytes, including the PCM |

where `P` is the current buffered sample count and `channels` is 1 or 2.
The total size is therefore `292 + 20*N + 4*channels*P`.  The value returned
by `FLAC__stream_encoder_checkpoint_size()` is exactly this number.

## Validation performed on resume

Resume returns NULL, without invoking the write or seek callbacks and
without writing anything, if any of the following holds:

* the buffer is truncated, shorter than the fixed header, or longer than the
  recorded total size;
* the magic or the version is not recognized;
* any flag, reserved field, MD5 block size, or scratch-field alias
  (min/max framesize duplicated in the u32/u64 fields) is inconsistent;
* the CRC-32 does not match;
* any fixed field is outside the supported configuration (channels, bits,
  rate, blocksize, buffered sample count, seek point count, ...);
* the buffered sample counts, byte counts, or MD5 byte counts disagree with
  each other or with the buffer length;
* the encoded counters disagree: `samples_written` must equal
  `current_frame_number * blocksize`, the STREAMINFO scratch total must equal
  `samples_written`, `frames_written` must be at least `current_frame_number`,
  and `first_seekpoint_to_check` must not exceed the seek point count;
* the seek points are not sorted/legal;
* `expected_offset` differs from the recorded bytes-written value, or from
  the value reported by the caller's tell callback.

The caller must open the original output and position it at the saved byte
offset before calling resume.  Resume may call the tell callback; it never
calls the write or seek callbacks.

## Resume semantics

The returned encoder is independent: it copies the pending PCM, the MD5
context, the running counters, and a private copy of the SEEKTABLE out of the
checkpoint buffer.  Both the source encoder and the checkpoint buffer may be
destroyed immediately afterwards.  The resumed encoder then:

* does not write the FLAC marker, STREAMINFO, or any metadata again;
* continues feeding frames with the correct frame numbering;
* keeps the original STREAMINFO and seek table offsets so that `finish()`
  rewrites exactly the same STREAMINFO and seek point locations as an
  uninterrupted encode;
* carries the same MD5 state so the final MD5 matches;
* may itself be checkpointed again with the same functions.

Saving a live encoder and then continuing it on the one hand, or resuming a
second encoder from the same bytes on the other, produces two independent
instances that share no mutable workspace.
