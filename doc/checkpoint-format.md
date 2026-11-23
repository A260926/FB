# HEVC decoder checkpoint format

This document describes the binary format written by
`de265_checkpoint_write()` and read by `de265_checkpoint_read()`. The public
API is declared in `libde265/de265.h`; the implementation lives in
`libde265/checkpoint.cc`.

A checkpoint captures the decoder state directly after a complete picture, so
that decoding can continue with the next picture without re-decoding from the
preceding IRAP (IDR/CRA) picture.

## Applicability

A checkpoint can only be written when the decoder is quiescent:

* all NAL units of the last picture have been pushed,
* `de265_push_end_of_frame()` has been called for it,
* `de265_decode()` has processed those NAL units and the post-processing tasks
  (deblocking, SAO) they spawn, and
* no input bytes, NAL units, image units or worker tasks are pending.

`de265_decode()` must have been driven until the decoder reports
`DE265_ERROR_WAITING_FOR_INPUT_DATA` (or otherwise has an empty NAL queue and no
image units). Pictures that are still waiting in the output queue may be left
there; this is a valid checkpoint state. Use the same input boundaries and
`de265_push_end_of_frame()` calls for the uninterrupted reference decode and the
checkpointed decode so that both see identical picture boundaries.

Currently supported:

* single layer, progressive (frame) coding,
* 8-bit Main profile, 4:2:0 (I420),
* fixed parameter sets and picture size,
* the default image allocator (`de265_get_default_image_allocation_functions()`),
* no missing or corrupt reference pictures,
* temporal layer dropping disabled (`de265_set_limit_TID()` not used, framerate
  ratio 100%), and
* NALs pushed with `user_data == NULL` (PTS is preserved).

For any other state `de265_checkpoint_size()` returns 0 and
`de265_checkpoint_write()` returns `DE265_ERROR_CHECKPOINT_UNAVAILABLE` without
modifying the buffer or the decoder.

## Conventions

* All integers are fixed-width little-endian: `u8`, `u16`, `u32`, `u64` unsigned;
  `i32`/`i64` are two's-complement.
* `blob` = `u32 length` followed by `length` bytes.
* A field that stores a DPB slot index refers to the slot order written in the
  DPB section below.
* Nothing in the file is a pointer, a virtual address, a mutex, or a worker
  thread. The format is position-independent and can be read in another process.

## File layout

```
+--------------------------------------+
| header                               |
+--------------------------------------+
| body                                 |
+--------------------------------------+
```

### Header

| field       | type   | value                                  |
|-------------|--------|----------------------------------------|
| magic       | 8 bytes| ASCII `"D265CKPT"`                     |
| version     | u16    | format version, currently `1`          |
| flags       | u16    | reserved, must be `0`                  |
| total_size  | u64    | total file size in bytes (header+body) |

The reader rejects a file whose magic is wrong, whose version is not supported,
whose `flags` are nonzero, or whose `total_size` does not exactly match the
provided buffer size (this catches both truncation and trailing garbage).

The body is a sequence of fields in the order below. The reader fails if it
does not consume the body exactly.

### Decoder state

| field                     | type | notes                                  |
|---------------------------|------|----------------------------------------|
| current_image_poc_lsb     | i32  |                                        |
| first_decoded_picture     | u8   | 0/1                                    |
| NoRaslOutputFlag          | u8   | 0/1                                    |
| HandleCraAsBlaFlag        | u8   | 0/1                                    |
| FirstAfterEndOfSequenceNAL| u8   | 0/1                                    |
| PicOrderCntMsb            | i32  |                                        |
| prevPicOrderCntLsb        | i32  |                                        |
| prevPicOrderCntMsb        | i32  |                                        |
| nal_unit_type             | u8   | last decoded NAL unit type             |
| IdrPicFlag                | u8   | 0/1                                    |
| RapPicFlag                | u8   | 0/1                                    |
| NumPocStCurrBefore        | i32  | 0..16                                  |
| NumPocStCurrAfter         | i32  | 0..16                                  |
| NumPocStFoll              | i32  | 0..16                                  |
| NumPocLtCurr              | i32  | 0..16                                  |
| NumPocLtFoll              | i32  | 0..16                                  |
| PocStCurrBefore[16]       | 16×i32 | reference-set POCs                  |
| PocStCurrAfter[16]        | 16×i32 |                                     |
| PocStFoll[16]             | 16×i32 |                                     |
| PocLtCurr[16]             | 16×i32 |                                     |
| PocLtFoll[16]             | 16×i32 |                                     |
| RefPicSetStCurrBefore[16] | 16×i32 | DPB slot indices, -1 = none        |
| RefPicSetStCurrAfter[16]  | 16×i32 |                                     |
| RefPicSetStFoll[16]       | 16×i32 |                                     |
| RefPicSetLtCurr[16]       | 16×i32 |                                     |
| RefPicSetLtFoll[16]       | 16×i32 |                                     |
| PocLsbLt[16]              | 16×i32 | long-term POC LSBs                  |
| UsedByCurrPicLt[16]       | 16×i32 |                                     |
| DeltaPocMsbCycleLt[16]    | 16×u32 |                                     |
| CurrDeltaPocMsbPresentFlag[16] | 16×u8 | 0/1                              |
| FollDeltaPocMsbPresentFlag[16] | 16×u8 | 0/1                              |
| limit_HighestTid          | u8   | must be 6 (dropping disabled)          |
| goal_HighestTid           | u8   |                                        |
| current_HighestTid        | u8   |                                        |
| framerate_ratio           | u8   | must be 100                            |
| layer_framerate_ratio     | u8   |                                        |
| current_sps_id_plus1      | u8   | 0 = none, else SPS id + 1              |
| current_pps_id_plus1      | u8   | 0 = none, else PPS id + 1              |
| current_vps_id_plus1      | u8   | 0 = none, else VPS id + 1              |
| param_sei_check_hash      | u8   | 0/1                                    |
| param_conceal_stream_errors | u8 | 0/1                                    |
| param_suppress_faulty_pictures | u8 | 0/1                                 |
| param_disable_deblocking  | u8   | 0/1                                    |
| param_disable_sao         | u8   | 0/1                                    |
| max_image_size_pixels     | u32  | security limits                        |
| max_NAL_size_bytes        | u32  |                                        |
| max_SEI_messages          | u32  |                                        |
| dpb_max_images            | u8   | DPB capacity (max)                     |
| dpb_norm_images           | u8   | DPB capacity (normal)                  |

### Parameter sets

| field        | type | notes                                             |
|--------------|------|---------------------------------------------------|
| count        | u32  | number of parameter-set records                   |
| records      | …    | `count` × { type u8, id u8, payload blob }        |

`type` is `0` = VPS, `1` = SPS, `2` = PPS. `payload` is the raw RBSP of the
parameter set *after* the two-byte NAL header and with emulation-prevention
bytes removed, i.e. exactly the bytes the decoder parses internally. Records
are written as VPS, then SPS, then PPS, each in ascending id order. On restore,
SPS are parsed before PPS so a PPS can be resolved against its SPS. The payload
length is bounded (1 MiB per set) so a corrupt file cannot trigger a huge
allocation.

### Decoded picture buffer

| field        | type | notes                                             |
|--------------|------|---------------------------------------------------|
| num_slots    | u32  | DPB slot count (≤ 255)                            |
| slots        | …    | `num_slots` × slot record                         |

Each slot record begins with `live u8`:

* `live == 0`: an empty DPB slot. No further data follows. (Empty slots are
  kept so that the slot indices stored in the reference lists and the picture
  metadata remain valid.)
* `live == 1`: a picture record follows.

#### Picture record

| field                     | type   | notes                                  |
|---------------------------|--------|----------------------------------------|
| sps_id                    | u8     |                                        |
| pps_id                    | u8     |                                        |
| has_vps                   | u8     | 0/1                                    |
| vps_id                    | u8     | present iff has_vps                    |
| width, height             | u32,u32| luma size                              |
| width_confwin, height_confwin | u32,u32 | conformance window (luma)         |
| chroma_width_confwin, chroma_height_confwin | u32,u32 | chroma window     |
| luma_stride, chroma_stride| u32,u32| in samples                             |
| PicOrderCntVal            | i32    |                                        |
| picture_order_cnt_lsb     | i32    |                                        |
| PicState                  | u8     | 0 unused, 1 short-term, 2 long-term    |
| PicOutputFlag             | u8     | 0/1                                    |
| integrity                 | u8     |                                        |
| removed_at_picture_id     | u32    |                                        |
| picture_id                | u32    | restored verbatim; later IDs are higher|
| nal_unit_type             | u8     |                                        |
| nuh_layer_id              | u8     | must be 0                              |
| nuh_temporal_id           | u8     |                                        |
| pts                       | i64    | preserved                              |
| planes[3]                 | …      | three plane records (Y, Cb, Cr)        |
| cb_info_count             | u32    |                                        |
| cb_info                   | n×byte | `n = count * sizeof(CB_ref_info)`      |
| pb_info_count             | u32    |                                        |
| pb_info                   | n×byte | `n = count * sizeof(PBMotion)`         |
| ctb_info_count            | u32    |                                        |
| ctb_info                  | n×byte | `n = count * sizeof(CTB_info)`         |
| slice_count               | u32    |                                        |
| slices                    | …      | `slice_count` × slice record           |

Plane record:

| field  | type | notes                                            |
|--------|------|--------------------------------------------------|
| stride | u32  | in samples; must equal the restored stride       |
| height | u32  | plane height                                     |
| width  | u32  | plane width                                      |
| bytes  | u64  | `stride * height`                                |
| data   | bytes| allocated plane contents (including row padding) |

The stored dimensions must reproduce the geometry the default allocator
produces for the referenced SPS; otherwise the checkpoint is rejected as
inconsistent.

Slice record (only the fields temporal motion vector prediction reads from a
reference picture are needed; all are restored):

| field                          | type | notes                            |
|--------------------------------|------|----------------------------------|
| slice_type                     | u8   | 0 B, 1 P, 2 I                     |
| num_ref_idx_l0_active          | u16  |                                   |
| num_ref_idx_l1_active          | u16  |                                   |
| slice_temporal_mvp_enabled_flag| u8   | 0/1                               |
| collocated_from_l0_flag        | u8   | 0/1                               |
| collocated_ref_idx             | u8   |                                   |
| SliceAddrRS                    | u32  |                                   |
| per list l in {0,1}, i in 0..15: |    |                                   |
|   LongTermRefPic[l][i]         | u8   | 0/1                               |
|   RefPicList_POC[l][i]         | i32  |                                   |
|   RefPicList[l][i]             | i32  | DPB slot index, -1 = none         |

Every CTB's `ctb_info.SliceHeaderIndex` must index the restored slice list;
otherwise the checkpoint is rejected.

### Output queues

| field              | type | notes                                        |
|--------------------|------|----------------------------------------------|
| reorder_count      | u32  | pictures in the reorder buffer               |
| reorder_slots      | n×u32| DPB slot indices, in reorder order           |
| output_count       | u32  | pictures in the output queue                 |
| output_slots       | n×u32| DPB slot indices, in output order (head first) |

Restored pictures that were in the output queue are still there, so they are
returned exactly once by `de265_get_next_picture()`. Pictures the caller had
already consumed are not in the queue and are not returned again.

## Determinism

For the same quiescent decoder state, `de265_checkpoint_write()` produces
byte-identical output. This holds because the serializer writes only fields that
are fully determined by the decode history, in a fixed order, and the pixel and
per-block metadata arrays are zero-initialized by the allocator before use.

## Restoring

`de265_checkpoint_read()` returns a new, independently owned decoder:

* it references neither the input buffer nor the source decoder,
* it has no worker threads; call `de265_start_worker_threads()` afterwards if
  desired (0 or 2 workers both work),
* restored picture IDs are below the IDs handed out afterwards, so the decoder's
  reference-picture bookkeeping sees the same relationships as before,
* the restored decoder can be `de265_reset()` and used for another stream, and
  it shares no mutable DPB or slice objects with the source.

The first picture decoded after restoring may be a P or B picture that depends
on the restored reference pictures.

## Error handling

`de265_checkpoint_read()` returns `NULL` and fills `*err` on any of:

* `DE265_ERROR_INVALID_ARGUMENT` — NULL buffer,
* `DE265_ERROR_CHECKPOINT_FORMAT` — wrong magic, unsupported version, nonzero
  flags, size mismatch (truncation or trailing bytes), malformed field, an
  unsupported parameter set, a reference to a nonexistent parameter set or DPB
  slot, a picture geometry that does not match its SPS, a slice-header index
  outside the restored slice list, or a body that is not consumed exactly.
