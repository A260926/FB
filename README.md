h264-reader
===========

Reader for H264 bitstream syntax, written in Rust.

[![crates.io version](https://img.shields.io/crates/v/h264-reader.svg)](https://crates.io/crates/h264-reader)
[![Documentation](https://docs.rs/h264-reader/badge.svg)](https://docs.rs/h264-reader)

Aims to provide access to stream metadata; does not actually decode the video.

The implementation attempts to minimise copying of source data, for efficiency, at the cost of a more complicated API

## Picture order count

The [`poc`](src/poc.rs) module provides [`poc::PocReader`], an access-unit-level
reader that derives the H.264 picture order count (section 8.2.1) for each
primary coded picture.

`PocReader::push_access_unit` takes one access unit at a time, as a slice of
complete NAL units (Annex B start codes already removed; the NAL header and
emulation-prevention bytes are still present). It returns:

 * `Ok(None)` for an access unit containing only non-VCL NAL units (SPS/PPS,
   AUD, SEI and similar), and
 * `Ok(Some(PictureOrder))` exactly once for an access unit containing a
   primary coded picture.

`PictureOrder` records `frame_num`, the referenced SPS/PPS ids, the optional
`TopFieldOrderCnt`/`BottomFieldOrderCnt` (a field that does not exist stays
`None` rather than being reported as zero), and the minimum of the field
counts that are present. All counts are signed 64-bit integers.

Scope and limitations:

 * All three `pic_order_cnt_type` values (0, 1 and 2) are supported, for coded
   frames, separately coded top/bottom fields and complementary field pairs.
 * Only picture order counts are derived. No timestamps are generated, no pixel
   data is decoded, and no reference picture store is maintained.
 * SVC/MVC extension NAL units, data-partitioned and redundant coded pictures
   are rejected with an explicit error rather than decoded.
 * SPS/PPS parameter sets are stored by the reader and take effect in NAL
   order. A re-sent parameter set with identical content does not reset POC
   state; a changed SPS only establishes new POC history when an IDR picture
   references it. A failed access unit commits neither parameter sets nor POC
   history.
 * `PocReader::reset` clears the POC/frame_num history (the next primary coded
   picture must then be an IDR) while retaining the declared parameter sets.

### Access unit boundaries

Access unit grouping is the **caller's responsibility**, not the reader's. The
NAL units handed to `push_access_unit` must already belong to a single access
unit in decoding order. The [`AnnexBReader`](src/annexb.rs) only splits the byte
stream into NAL units; the [`poc_dump` example](examples/poc_dump.rs) shows how
to group those NAL units into access units (a new access unit begins at each VCL
NAL that follows another VCL NAL) before feeding them to `PocReader`.

## Supported syntax

The following list shows the current state of support per H264 syntax element:

 * Bytestream formats
   * [x] _Annex B_ format (e.g. in MPEG-TS)
   * [x] _AVCC_ format (e.g. in MP4)
 * Network Abstraction Layer Units (NAL Units)
   * [x] `slice_layer_without_partitioning_rbsp()` — `slice_header()` and `slice_data()` (baseline profile only)
   * [ ] `slice_data_partition_a_layer_rbsp()`
   * [ ] `slice_data_partition_b_layer_rbsp()`
   * [ ] `slice_data_partition_c_layer_rbsp()`
   * [ ] `sei_rbsp()` _Supplementary Enhancement Information_ headers - the following payloads are supported:
     * [x] `buffering_period()`
     * [x] `pic_timing()`
     * [ ] `pan_scan_rect()`
     * [ ] `filler_payload()`
     * [x] `user_data_registered_itu_t_t35()`
     * [x] `user_data_unregistered()`
     * [x] `recovery_point()`
     * [ ] `dec_ref_pic_marking_repetition()`
     * [ ] `spare_pic()`
     * [ ] `scene_info()`
     * [ ] `sub_seq_info()`
     * [ ] `sub_seq_layer_characteristics()`
     * [ ] `sub_seq_characteristics()`
     * [ ] `full_frame_freeze()`
     * [ ] `full_frame_freeze_release()`
     * [ ] `full_frame_snapshot()`
     * [ ] `progressive_refinement_segment_start()`
     * [ ] `progressive_refinement_segment_end()`
     * [ ] `motion_constrained_slice_group_set()`
     * [ ] `film_grain_characteristics()`
     * [ ] `deblocking_filter_display_preference()`
     * [ ] `stereo_video_info()`
     * [ ] `post_filter_hint()`
     * [ ] `tone_mapping_info()`
     * [ ] _Annex G_ headers
     * [ ] _Annex H_ headers
     * [ ] _Annex I_ headers
     * [ ] _Annex J_ headers
     * [ ] `frame_packing_arrangement()`
     * [ ] `display_orientation()`
     * [ ] `mastering_display_colour_volume()`
     * [ ] `colour_remapping_info()`
     * [ ] `alternative_transfer_characteristics()`
     * [ ] `alternative_depth_info()`
   * [x] `seq_parameter_set_rbsp()`
   * [x] `pic_parameter_set_rbsp()`
   * [x] `access_unit_delimiter_rbsp()`
   * [ ] `end_of_stream_rbsp()`
   * [ ] `filler_data_rbsp()`
   * [x] `seq_parameter_set_extension_rbsp()`
   * [x] `prefix_nal_unit_rbsp()` — MVC (empty body) and SVC (`prefix_nal_unit_svc()`)
   * [x] `subset_seq_parameter_set_rbsp()`
   * [ ] `depth_parameter_set_rbsp()`
   * [x] `slice_layer_extension_rbsp()` — MVC `slice_header()` only (including `ref_pic_list_mvc_modification()`), not `slice_data()`

## Design goals

### Avoid copies

Parsing components accept partial data to avoid coping data
into intermediate buffers.  This is intended to support common cases like,

 - data embedded in MPEG-TS packets, where h264 data is interspersed with MPEG-TS header data
 - data being read from the network, where the data available at any instant may be incomplete

An alternative to accepting partial data would be to take a number of peices of partial data

### Lazy parsing

The implementation should be written to defer parsing data structures until an accessor method is called.
This can mean saving parsing costs for callers that don't care about all the data.  It can be difficult to
apply this principal universally, so in some areas we don't bother and just 'eager parse' (particularly
structures defined bit-by-bit rather than byte-by-byte).
