/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Checkpoint serialization.
 *
 * A checkpoint captures everything the decoder needs to continue after a
 * complete picture without decoding again from the preceding IRAP picture:
 *
 *  - the VPS/SPS/PPS (as raw RBSP payloads; re-parsed on restore),
 *  - the POC bookkeeping and reference-picture-set lists,
 *  - the decoded picture buffer: every live picture together with its three
 *    pixel planes and the per-block metadata that later pictures read, plus
 *    the slice headers temporal motion vector prediction consults,
 *  - the reorder buffer and output queue (order and PTS).
 *
 * Pointers, mutexes, worker threads, historic VCL NALs and the input
 * bytestream are deliberately not stored. The on-disk layout is documented in
 * doc/checkpoint-format.md.
 *
 * All integers are fixed-width little-endian. Serialization is deterministic:
 * saving the same quiescent decoder state twice yields byte-identical output.
 */

#include "checkpoint.h"

#include "decctx.h"
#include "dpb.h"
#include "image.h"
#include "slice.h"
#include "sps.h"
#include "pps.h"
#include "vps.h"
#include "bitstream.h"
#include "nal.h"

#include <cstring>
#include <limits>
#include <memory>
#include <vector>

/* --- little-endian byte writer ------------------------------------------ */

class byte_writer
{
 public:
  void u8(uint8_t v) { bytes.push_back(v); }
  void u16(uint16_t v) {
    bytes.push_back(uint8_t(v));
    bytes.push_back(uint8_t(v >> 8));
  }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; i++) bytes.push_back(uint8_t(v >> (8 * i)));
  }
  void u64(uint64_t v) {
    for (int i = 0; i < 8; i++) bytes.push_back(uint8_t(v >> (8 * i)));
  }
  void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
  void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }

  void raw(const void* p, size_t n) {
    if (n == 0) return;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    bytes.insert(bytes.end(), b, b + n);
  }

  void blob(const std::vector<uint8_t>& v) {
    u32(static_cast<uint32_t>(v.size()));
    bytes.insert(bytes.end(), v.begin(), v.end());
  }

  std::vector<uint8_t> bytes;
};

/* --- bounds-checked little-endian reader -------------------------------- */

class byte_reader
{
 public:
  byte_reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

  bool ok() const { return m_good; }
  size_t remaining() const { return m_good ? m_size - m_offset : 0; }

  bool need(size_t n) {
    if (!m_good || n > m_size - m_offset) { m_good = false; return false; }
    return true;
  }

  uint8_t u8() {
    if (!need(1)) return 0;
    return m_data[m_offset++];
  }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v = uint16_t(m_data[m_offset]) | (uint16_t(m_data[m_offset + 1]) << 8);
    m_offset += 2;
    return v;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= uint32_t(m_data[m_offset + i]) << (8 * i);
    m_offset += 4;
    return v;
  }
  uint64_t u64() {
    if (!need(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= uint64_t(m_data[m_offset + i]) << (8 * i);
    m_offset += 8;
    return v;
  }
  int32_t i32() { return static_cast<int32_t>(u32()); }
  int64_t i64() { return static_cast<int64_t>(u64()); }

  bool raw(void* out, size_t n) {
    if (!need(n)) return false;
    if (n) memcpy(out, m_data + m_offset, n);
    m_offset += n;
    return true;
  }

  /* Length-prefixed blob with an upper bound, so corrupt data cannot trigger
     an absurd allocation. */
  bool blob(std::vector<uint8_t>& out, size_t limit) {
    uint32_t n = u32();
    if (!m_good || n > limit || n > remaining()) { m_good = false; return false; }
    out.assign(m_data + m_offset, m_data + m_offset + n);
    m_offset += n;
    return true;
  }

 private:
  const uint8_t* m_data;
  size_t m_size;
  size_t m_offset = 0;
  bool m_good = true;
};

constexpr uint8_t MAGIC[8] = { 'D','2','6','5','C','K','P','T' };

constexpr uint32_t MAX_PARAMETER_SET_BYTES = 1u << 20;   // sanity bound per set
constexpr uint32_t MAX_PICTURES             = 255;      // DPB size is uint8_t-bounded
constexpr uint32_t MAX_SLICES_PER_PICTURE   = 4096;

/* ======================================================================== */

bool is_supported_sps(const seq_parameter_set* sps, const char** why)
{
  if (sps->chroma_format_idc != 1 || sps->separate_colour_plane_flag) {
    if (why) *why = "unsupported chroma format";
    return false;
  }
  if (sps->BitDepth_Y != 8 || sps->BitDepth_C != 8) {
    if (why) *why = "unsupported bit depth";
    return false;
  }
  if (sps->sps_max_sub_layers != 1) {
    if (why) *why = "multiple temporal sub-layers";
    return false;
  }
  if (sps->vui_parameters_present_flag && sps->vui.field_seq_flag) {
    if (why) *why = "interlaced (field-coded) sequence";
    return false;
  }
  return true;
}


/* ======================================================================== */
/* Availability                                                             */
/* ======================================================================== */

bool checkpoint_state::is_available(const decoder_context* ctx, const char** why)
{
  auto fail = [why](const char* msg) -> bool {
    if (why) *why = msg;
    return false;
  };

  if (ctx == nullptr) {
    return fail("no decoder");
  }

  /* Quiescent: no input bytes, no queued NALs, no partially decoded picture,
     no pending worker tasks. */
  if (ctx->nal_parser.bytes_in_input_queue() != 0 ||
      ctx->nal_parser.number_of_NAL_units_pending() != 0) {
    return fail("input or NALs pending");
  }
  if (!ctx->image_units.empty()) {
    return fail("picture still being decoded");
  }
  if (!ctx->thread_pool_.tasks.empty() || ctx->thread_pool_.num_threads_working != 0) {
    return fail("worker tasks pending");
  }

  /* Only after a complete picture. */
  if (ctx->first_decoded_picture) {
    return fail("no complete picture decoded yet");
  }

  /* Supported parameter sets only. */
  for (int i = 0; i < DE265_MAX_SPS_SETS; i++) {
    if (ctx->sps[i] && ctx->sps[i]->sps_read) {
      if (!is_supported_sps(ctx->sps[i].get(), why)) {
        return false;
      }
    }
  }

  /* Restoring relies on the default image allocator. */
  const de265_image_allocation& def = de265_image::default_image_allocation;
  const de265_image_allocation& cur = ctx->param_image_allocation_functions;
  if (cur.get_buffer != def.get_buffer || cur.release_buffer != def.release_buffer) {
    return fail("custom image allocator");
  }

  /* No temporal layer dropping. */
  if (ctx->limit_HighestTid != 6 || ctx->framerate_ratio != 100) {
    return fail("temporal layer dropping enabled");
  }

  /* Every live DPB picture must be a complete, checkpointable picture. */
  int live_pictures = 0;
  for (size_t i = 0; i < ctx->dpb.dpb.size(); i++) {
    const de265_image* img = ctx->dpb.dpb[i];
    if (img == nullptr || img->pixels[0] == nullptr) {
      continue;
    }
    live_pictures++;

    if (img->chroma_format != de265_chroma_420 ||
        img->BitDepth_Y != 8 || img->BitDepth_C != 8) {
      return fail("unsupported picture format");
    }
    if (!img->has_sps() || !img->has_pps()) {
      return fail("picture without parameter sets");
    }
    if (!is_supported_sps(&img->get_sps(), why)) {
      return false;
    }
    if (img->user_data != nullptr) {
      return fail("input user_data is not NULL");
    }
    if (img->slices.empty()) {
      return fail("picture without slice headers");
    }
    if (img->nThreadsRunning != 0 || img->nThreadsBlocked != 0) {
      return fail("picture still being decoded");
    }
    for (int c = 0; c < 3; c++) {
      if (img->plane_user_data[c] != nullptr) {
        return fail("custom plane user data");
      }
    }
  }

  if (live_pictures == 0) {
    return fail("no complete picture in the DPB");
  }

  return true;
}

/* ======================================================================== */
/* Serialization                                                            */
/* ======================================================================== */

void checkpoint_state::write_slice_header(const slice_segment_header* shdr,
                                          byte_writer& w)
{
  w.u8(shdr->slice_type);
  w.u16(shdr->num_ref_idx_l0_active);
  w.u16(shdr->num_ref_idx_l1_active);
  w.u8(shdr->slice_temporal_mvp_enabled_flag ? 1 : 0);
  w.u8(shdr->collocated_from_l0_flag ? 1 : 0);
  w.u8(shdr->collocated_ref_idx);
  w.u32(shdr->SliceAddrRS);

  for (int l = 0; l < 2; l++) {
    for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
      w.u8(shdr->LongTermRefPic[l][i] ? 1 : 0);
      w.i32(shdr->RefPicList_POC[l][i]);
      w.i32(shdr->RefPicList[l][i]);
    }
  }
}

void checkpoint_state::write_parameter_sets(const decoder_context* ctx,
                                            byte_writer& w)
{
  uint32_t count = 0;
  for (int id = 0; id < DE265_MAX_VPS_SETS; id++) {
    if (ctx->vps[id] && !ctx->raw_vps[id].empty()) count++;
  }
  for (int id = 0; id < DE265_MAX_SPS_SETS; id++) {
    if (ctx->sps[id] && !ctx->raw_sps[id].empty()) count++;
  }
  for (int id = 0; id < DE265_MAX_PPS_SETS; id++) {
    if (ctx->pps[id] && !ctx->raw_pps[id].empty()) count++;
  }

  w.u32(count);

  /* Type order: 0=VPS, 1=SPS, 2=PPS. SPS precede PPS so that a PPS can be
     parsed against its already-registered SPS on restore. */
  for (int id = 0; id < DE265_MAX_VPS_SETS; id++) {
    if (ctx->vps[id] && !ctx->raw_vps[id].empty()) {
      w.u8(0);
      w.u8(uint8_t(id));
      w.blob(ctx->raw_vps[id]);
    }
  }
  for (int id = 0; id < DE265_MAX_SPS_SETS; id++) {
    if (ctx->sps[id] && !ctx->raw_sps[id].empty()) {
      w.u8(1);
      w.u8(uint8_t(id));
      w.blob(ctx->raw_sps[id]);
    }
  }
  for (int id = 0; id < DE265_MAX_PPS_SETS; id++) {
    if (ctx->pps[id] && !ctx->raw_pps[id].empty()) {
      w.u8(2);
      w.u8(uint8_t(id));
      w.blob(ctx->raw_pps[id]);
    }
  }
}

void checkpoint_state::write_picture(const decoder_context* /*ctx*/,
                                     const de265_image* img, uint32_t canonical_id,
                                     byte_writer& w)
{
  const seq_parameter_set& sps = img->get_sps();
  const pic_parameter_set& pps = img->get_pps();

  w.u8(sps.seq_parameter_set_id);
  w.u8(pps.pic_parameter_set_id);
  w.u8(img->has_vps() ? 1 : 0);
  if (img->has_vps()) {
    w.u8(img->get_vps().video_parameter_set_id);
  }

  w.u32(static_cast<uint32_t>(img->width));
  w.u32(static_cast<uint32_t>(img->height));
  w.u32(static_cast<uint32_t>(img->width_confwin));
  w.u32(static_cast<uint32_t>(img->height_confwin));
  w.u32(static_cast<uint32_t>(img->chroma_width_confwin));
  w.u32(static_cast<uint32_t>(img->chroma_height_confwin));
  w.u32(static_cast<uint32_t>(img->stride));
  w.u32(static_cast<uint32_t>(img->chroma_stride));

  w.i32(img->PicOrderCntVal);
  w.i32(img->picture_order_cnt_lsb);
  w.u8(static_cast<uint8_t>(img->PicState));
  w.u8(img->PicOutputFlag ? 1 : 0);
  w.u8(img->integrity.load());
  /* Picture IDs are a process-global counter, so they differ between runs and
     would make two checkpoints of the same logical state differ. Store a
     canonical, history-independent ID (slot index + 1) and collapse the
     "already removed" marker to 0 (UINT32_MAX = "not removed" is kept). The
     only property the decoder relies on is ordering: restored IDs are below
     every future ID, and a removal marker below all IDs means "removed". */
  w.u32(img->removed_at_picture_id == std::numeric_limits<uint32_t>::max()
            ? std::numeric_limits<uint32_t>::max() : 0u);
  w.u32(canonical_id);
  w.u8(img->nal_hdr.nal_unit_type);
  w.u8(img->nal_hdr.nuh_layer_id);
  w.u8(img->nal_hdr.nuh_temporal_id);
  w.i64(img->pts);

  /* Pixel planes. Store the full allocated plane (stride * plane height),
     including the row padding, so restoring is a straightforward memcpy. The
     visible window is derived from the stored dimensions. */
  for (int c = 0; c < 3; c++) {
    int cw = img->get_width(c);
    int ch = img->get_height(c);
    ptrdiff_t stride = img->get_image_stride(c);
    w.u32(static_cast<uint32_t>(stride));
    w.u32(static_cast<uint32_t>(ch));
    w.u32(static_cast<uint32_t>(cw));
    const size_t n = static_cast<size_t>(stride) * static_cast<size_t>(ch);
    w.u64(n);
    w.raw(img->pixels[c], n);
  }

  /* Per-block metadata that later pictures read from this one: prediction mode
     and partition geometry (cb_info), motion vectors (pb_info), and the
     slice-header index per CTB (ctb_info). Their sizes are derived from the
     SPS; a checkpoint record carries the counts so the reader can verify
     them. These arrays are zero-initialized by alloc_image() and only the
     entries the encoder touched are nonzero, so the stored bytes are stable.
     (intraPredMode, intraPredModeC, tu_info and deblk_info are only used while
     the picture itself is decoded and are not needed by later pictures.) */
  w.u32(static_cast<uint32_t>(img->cb_info.data_size));
  w.raw(img->cb_info.data, sizeof(CB_ref_info) * img->cb_info.data_size);
  w.u32(static_cast<uint32_t>(img->pb_info.data_size));
  w.raw(img->pb_info.data, sizeof(PBMotion) * img->pb_info.data_size);
  w.u32(static_cast<uint32_t>(img->ctb_info.data_size));
  w.raw(img->ctb_info.data, sizeof(CTB_info) * img->ctb_info.data_size);

  /* Slice headers: reference pictures keep these for temporal motion vector
     prediction, which consults the collocated picture's slice. */
  w.u32(static_cast<uint32_t>(img->slices.size()));
  for (const slice_segment_header* shdr : img->slices) {
    write_slice_header(shdr, w);
  }
}

bool checkpoint_state::serialize(const decoder_context* ctx, std::vector<uint8_t>& out)
{
  byte_writer body;

  /* --- decoder state --- */
  body.i32(ctx->current_image_poc_lsb);
  body.u8(ctx->first_decoded_picture ? 1 : 0);
  body.u8(ctx->NoRaslOutputFlag ? 1 : 0);
  body.u8(ctx->HandleCraAsBlaFlag ? 1 : 0);
  body.u8(ctx->FirstAfterEndOfSequenceNAL ? 1 : 0);
  body.i32(ctx->PicOrderCntMsb);
  body.i32(ctx->prevPicOrderCntLsb);
  body.i32(ctx->prevPicOrderCntMsb);

  body.u8(ctx->nal_unit_type);
  body.u8(ctx->IdrPicFlag ? 1 : 0);
  body.u8(ctx->RapPicFlag ? 1 : 0);

  body.i32(ctx->NumPocStCurrBefore);
  body.i32(ctx->NumPocStCurrAfter);
  body.i32(ctx->NumPocStFoll);
  body.i32(ctx->NumPocLtCurr);
  body.i32(ctx->NumPocLtFoll);
  for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
    body.i32(ctx->PocStCurrBefore[i]);
    body.i32(ctx->PocStCurrAfter[i]);
    body.i32(ctx->PocStFoll[i]);
    body.i32(ctx->PocLtCurr[i]);
    body.i32(ctx->PocLtFoll[i]);
  }
  for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
    body.i32(ctx->RefPicSetStCurrBefore[i]);
    body.i32(ctx->RefPicSetStCurrAfter[i]);
    body.i32(ctx->RefPicSetStFoll[i]);
    body.i32(ctx->RefPicSetLtCurr[i]);
    body.i32(ctx->RefPicSetLtFoll[i]);
  }
  for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
    body.i32(ctx->PocLsbLt[i]);
    body.i32(ctx->UsedByCurrPicLt[i]);
    body.u32(ctx->DeltaPocMsbCycleLt[i]);
  }
  for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
    body.u8(ctx->CurrDeltaPocMsbPresentFlag[i] ? 1 : 0);
    body.u8(ctx->FollDeltaPocMsbPresentFlag[i] ? 1 : 0);
  }

  /* Frame-dropping configuration. Only the "dropping disabled" state is
     checkpointable; store the values so the restored instance is identical. */
  body.u8(static_cast<uint8_t>(ctx->limit_HighestTid));
  body.u8(static_cast<uint8_t>(ctx->goal_HighestTid));
  body.u8(static_cast<uint8_t>(ctx->current_HighestTid));
  body.u8(static_cast<uint8_t>(ctx->framerate_ratio));
  body.u8(static_cast<uint8_t>(ctx->layer_framerate_ratio));

  /* Current parameter sets (so get_highest_TID() etc. behave as before). */
  {
    int cur_sps_id = -1, cur_pps_id = -1, cur_vps_id = -1;
    for (int id = 0; id < DE265_MAX_SPS_SETS; id++) {
      if (ctx->current_sps && ctx->current_sps == ctx->sps[id]) cur_sps_id = id;
    }
    for (int id = 0; id < DE265_MAX_PPS_SETS; id++) {
      if (ctx->current_pps && ctx->current_pps == ctx->pps[id]) cur_pps_id = id;
    }
    for (int id = 0; id < DE265_MAX_VPS_SETS; id++) {
      if (ctx->current_vps && ctx->current_vps == ctx->vps[id]) cur_vps_id = id;
    }
    body.u8(static_cast<uint8_t>(cur_sps_id + 1));
    body.u8(static_cast<uint8_t>(cur_pps_id + 1));
    body.u8(static_cast<uint8_t>(cur_vps_id + 1));
  }

  /* Decoder parameters that affect future output. */
  body.u8(ctx->param_sei_check_hash ? 1 : 0);
  body.u8(ctx->param_conceal_stream_errors ? 1 : 0);
  body.u8(ctx->param_suppress_faulty_pictures ? 1 : 0);
  body.u8(ctx->param_disable_deblocking ? 1 : 0);
  body.u8(ctx->param_disable_sao ? 1 : 0);
  body.u32(ctx->param_security_limits.max_image_size_pixels);
  body.u32(ctx->param_security_limits.max_NAL_size_bytes);
  body.u32(ctx->param_security_limits.max_SEI_messages);

  /* DPB capacity configuration. */
  body.u8(ctx->dpb.max_images_in_DPB);
  body.u8(ctx->dpb.norm_images_in_DPB);

  /* --- parameter sets --- */
  write_parameter_sets(ctx, body);

  /* --- DPB slots --- */
  body.u32(static_cast<uint32_t>(ctx->dpb.dpb.size()));
  for (size_t i = 0; i < ctx->dpb.dpb.size(); i++) {
    const de265_image* img = ctx->dpb.dpb[i];
    bool live = (img != nullptr && img->pixels[0] != nullptr);
    body.u8(live ? 1 : 0);
    if (live) {
      write_picture(ctx, img, static_cast<uint32_t>(i) + 1, body);
    }
  }

  /* --- reorder buffer and output queue, as slot indices --- */
  auto slot_of = [ctx](const de265_image* img) -> uint32_t {
    for (size_t i = 0; i < ctx->dpb.dpb.size(); i++) {
      if (ctx->dpb.dpb[i] == img) return static_cast<uint32_t>(i);
    }
    return 0;   /* unreachable: is_available() guarantees membership */
  };

  body.u32(static_cast<uint32_t>(ctx->dpb.reorder_output_queue.size()));
  for (const de265_image* img : ctx->dpb.reorder_output_queue) {
    body.u32(slot_of(img));
  }
  body.u32(static_cast<uint32_t>(ctx->dpb.image_output_queue.size()));
  for (const de265_image* img : ctx->dpb.image_output_queue) {
    body.u32(slot_of(img));
  }

  /* --- assemble file: magic, version, total length, body --- */
  byte_writer file;
  file.raw(MAGIC, sizeof(MAGIC));
  file.u16(format_version);
  file.u16(0);                                  // flags / reserved
  const uint64_t total = sizeof(MAGIC) + 2 + 2 + 8 + body.bytes.size();
  file.u64(total);
  file.raw(body.bytes.data(), body.bytes.size());

  out.swap(file.bytes);
  return true;
}

/* ======================================================================== */
/* Deserialization                                                          */
/* ======================================================================== */

bool checkpoint_state::parse_parameter_set(decoder_context* ctx, uint8_t type,
                                           uint8_t id,
                                           const std::vector<uint8_t>& payload)
{
  if (payload.empty()) { return false; }

  bitreader br(const_cast<uint8_t*>(payload.data()), static_cast<int>(payload.size()));
  de265_error err = DE265_OK;
  switch (type) {
    case 0: err = ctx->read_vps_NAL(br, payload.data(), static_cast<int>(payload.size())); break;
    case 1: err = ctx->read_sps_NAL(br, payload.data(), static_cast<int>(payload.size())); break;
    case 2: err = ctx->read_pps_NAL(br, payload.data(), static_cast<int>(payload.size())); break;
    default: return false;
  }
  if (err != DE265_OK) { return false; }

  switch (type) {
    case 0: return ctx->vps[id] != nullptr;
    case 1: return ctx->sps[id] != nullptr;
    case 2: return ctx->pps[id] != nullptr;
  }
  return false;
}

bool checkpoint_state::read_parameter_sets(decoder_context* ctx, byte_reader& r)
{
  uint32_t count = r.u32();
  if (!r.ok() || count > DE265_MAX_VPS_SETS + DE265_MAX_SPS_SETS + DE265_MAX_PPS_SETS) {
    return false;
  }

  for (uint32_t i = 0; i < count; i++) {
    uint8_t type = r.u8();
    uint8_t id = r.u8();
    std::vector<uint8_t> payload;
    if (!r.ok() || !r.blob(payload, MAX_PARAMETER_SET_BYTES)) {
      return false;
    }
    if (!parse_parameter_set(ctx, type, id, payload)) {
      return false;
    }  }

  /* At least one SPS and one PPS must exist for pictures to reference. */
  bool have_sps = false, have_pps = false;
  for (int i = 0; i < DE265_MAX_SPS_SETS; i++) have_sps |= (ctx->sps[i] != nullptr);
  for (int i = 0; i < DE265_MAX_PPS_SETS; i++) have_pps |= (ctx->pps[i] != nullptr);
  return have_sps && have_pps;
}

bool checkpoint_state::read_queues(decoder_context* ctx, byte_reader& r,
                                   uint32_t num_slots)
{
  auto slot_picture = [ctx, num_slots](uint32_t slot) -> de265_image* {
    if (slot >= num_slots || slot >= ctx->dpb.dpb.size()) return nullptr;
    de265_image* img = ctx->dpb.dpb[slot];
    if (img == nullptr || img->pixels[0] == nullptr) return nullptr;
    return img;
  };

  uint32_t n_reorder = r.u32();
  if (!r.ok() || n_reorder > num_slots) return false;
  std::vector<de265_image*> reorder;
  reorder.reserve(n_reorder);
  for (uint32_t i = 0; i < n_reorder; i++) {
    uint32_t slot = r.u32();
    de265_image* img = slot_picture(slot);
    if (img == nullptr) return false;
    reorder.push_back(img);
  }

  uint32_t n_output = r.u32();
  if (!r.ok() || n_output > num_slots) return false;
  std::vector<de265_image*> output;
  output.reserve(n_output);
  for (uint32_t i = 0; i < n_output; i++) {
    uint32_t slot = r.u32();
    de265_image* img = slot_picture(slot);
    if (img == nullptr) return false;
    output.push_back(img);
  }

  ctx->dpb.reorder_output_queue = std::move(reorder);
  ctx->dpb.image_output_queue.clear();
  for (de265_image* img : output) {
    ctx->dpb.image_output_queue.push_back(img);
  }
  return true;
}

bool checkpoint_state::read_slice_header(decoder_context* ctx, byte_reader& r,
                                         uint32_t num_slots, de265_image* img)
{
  (void)ctx;

  slice_segment_header* shdr = new slice_segment_header;

  shdr->slice_type = r.u8();
  shdr->num_ref_idx_l0_active = static_cast<uint8_t>(r.u16());
  shdr->num_ref_idx_l1_active = static_cast<uint8_t>(r.u16());
  shdr->slice_temporal_mvp_enabled_flag = r.u8() != 0;
  shdr->collocated_from_l0_flag = r.u8() != 0;
  shdr->collocated_ref_idx = r.u8();
  shdr->SliceAddrRS = r.u32();

  for (int l = 0; l < 2; l++) {
    for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
      shdr->LongTermRefPic[l][i] = r.u8() != 0;
      shdr->RefPicList_POC[l][i] = r.i32();
      shdr->RefPicList[l][i] = r.i32();
    }
  }

  if (!r.ok() ||
      shdr->slice_type > SLICE_TYPE_I ||
      shdr->num_ref_idx_l0_active > MAX_NUM_REF_PICS ||
      shdr->num_ref_idx_l1_active > MAX_NUM_REF_PICS) {
    delete shdr;
    return false;
  }

  /* Reference-list entries must stay within the DPB slot range (or be -1). */
  for (int l = 0; l < 2; l++) {
    for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
      int idx = shdr->RefPicList[l][i];
      if (idx < -1 || idx >= static_cast<int>(num_slots)) {
        delete shdr;
        return false;
      }
    }
  }

  img->add_slice_segment_header(shdr);
  return true;
}

bool checkpoint_state::read_picture(decoder_context* ctx, byte_reader& r,
                                    uint32_t num_slots, de265_image*& out)
{
  out = nullptr;

  uint8_t sps_id = r.u8();
  uint8_t pps_id = r.u8();
  uint8_t has_vps = r.u8();
  uint8_t vps_id = 0;
  if (!r.ok()) return false;
  if (has_vps) {
    vps_id = r.u8();
  }
  if (!r.ok()) return false;

  if (sps_id >= DE265_MAX_SPS_SETS || pps_id >= DE265_MAX_PPS_SETS ||
      has_vps > 1 || vps_id >= DE265_MAX_VPS_SETS) {
    return false;
  }
  if (!ctx->sps[sps_id] || !ctx->pps[pps_id]) {
    return false;   // inconsistent reference
  }
  if (has_vps && !ctx->vps[vps_id]) {
    return false;
  }

  uint32_t width = r.u32();
  uint32_t height = r.u32();
  uint32_t w_conf = r.u32();
  uint32_t h_conf = r.u32();
  uint32_t cw_conf = r.u32();
  uint32_t ch_conf = r.u32();
  uint32_t luma_stride = r.u32();
  uint32_t chroma_stride = r.u32();

  int32_t  poc = r.i32();
  int32_t  poc_lsb = r.i32();
  uint8_t  pic_state = r.u8();
  uint8_t  output_flag = r.u8();
  uint8_t  integrity = r.u8();
  uint32_t removed_at = r.u32();
  uint32_t id = r.u32();
  uint8_t  nal_type = r.u8();
  uint8_t  nal_layer = r.u8();
  uint8_t  nal_tid = r.u8();
  int64_t  pts = r.i64();
  if (!r.ok()) return false;

  if (width == 0 || height == 0 || pic_state > UsedForLongTermReference ||
      output_flag > 1 || nal_layer != 0) {
    return false;
  }

  const seq_parameter_set& sps = *ctx->sps[sps_id];

  if (sps.pic_width_in_luma_samples != width ||
      sps.pic_height_in_luma_samples != height) {
    return false;
  }

  de265_image* img = new de265_image;
  de265_error err = img->alloc_image(static_cast<int>(width), static_cast<int>(height),
                                     de265_chroma_420, ctx->sps[sps_id], true,
                                     ctx, static_cast<de265_PTS>(pts), nullptr, true);
  if (err != DE265_OK) {
    delete img;
    return false;
  }

  img->set_headers(has_vps ? ctx->vps[vps_id] : std::shared_ptr<video_parameter_set>(),
                   ctx->sps[sps_id], ctx->pps[pps_id]);

  /* The allocator must have produced the recorded geometry. */
  if (static_cast<uint32_t>(img->width) != width ||
      static_cast<uint32_t>(img->height) != height ||
      static_cast<uint32_t>(img->width_confwin) != w_conf ||
      static_cast<uint32_t>(img->height_confwin) != h_conf ||
      static_cast<uint32_t>(img->chroma_width_confwin) != cw_conf ||
      static_cast<uint32_t>(img->chroma_height_confwin) != ch_conf ||
      static_cast<uint32_t>(img->stride) != luma_stride ||
      static_cast<uint32_t>(img->chroma_stride) != chroma_stride) {
    delete img;
    return false;
  }

  img->PicOrderCntVal = poc;
  img->picture_order_cnt_lsb = poc_lsb;
  img->PicState = static_cast<PictureState>(pic_state);
  img->PicOutputFlag = (output_flag != 0);
  img->integrity = integrity;
  img->removed_at_picture_id = removed_at;
  img->ID = id;
  img->nal_hdr.nal_unit_type = nal_type;
  img->nal_hdr.nuh_layer_id = nal_layer;
  img->nal_hdr.nuh_temporal_id = nal_tid;
  img->user_data = nullptr;

  /* Pixel planes. */
  for (int c = 0; c < 3; c++) {
    uint32_t stride_stored = r.u32();
    uint32_t plane_h = r.u32();
    uint32_t plane_w = r.u32();
    uint64_t n = r.u64();
    if (!r.ok()) { delete img; return false; }

    int cw = img->get_width(c);
    int ch = img->get_height(c);
    ptrdiff_t stride = img->get_image_stride(c);

    if (stride_stored != static_cast<uint32_t>(stride) ||
        plane_h != static_cast<uint32_t>(ch) ||
        plane_w != static_cast<uint32_t>(cw) ||
        n != static_cast<uint64_t>(stride_stored) * plane_h ||
        !r.raw(img->pixels[c], static_cast<size_t>(n))) {
      delete img;
      return false;
    }
  }

  /* Metadata. */
  struct { uint32_t count; size_t elem; void* data; } fields[] = {
    { static_cast<uint32_t>(img->cb_info.data_size),    sizeof(CB_ref_info), img->cb_info.data },
    { static_cast<uint32_t>(img->pb_info.data_size),    sizeof(PBMotion),    img->pb_info.data },
    { static_cast<uint32_t>(img->ctb_info.data_size),   sizeof(CTB_info),    img->ctb_info.data },
  };
  for (auto& f : fields) {
    uint32_t n = r.u32();
    if (!r.ok() || n != f.count) { delete img; return false; }
    if (!r.raw(f.data, f.elem * n)) { delete img; return false; }
  }

  /* Slice headers. */
  uint32_t num_slices = r.u32();
  if (!r.ok() || num_slices == 0 || num_slices > MAX_SLICES_PER_PICTURE) {
    delete img;
    return false;
  }
  for (uint32_t i = 0; i < num_slices; i++) {
    if (!read_slice_header(ctx, r, num_slots, img)) {
      delete img;
      return false;
    }
  }

  /* Validate every CTB's slice-header index against the restored headers. */
  for (int i = 0; i < img->ctb_info.data_size; i++) {
    if (img->ctb_info[i].SliceHeaderIndex >= img->slices.size()) {
      delete img;
      return false;
    }
  }

  /* The picture is complete; mark all CTBs as decoded. */
  img->mark_all_CTB_progress(CTB_PROGRESS_PREFILTER);

  out = img;
  return true;
}

bool checkpoint_state::read_dpb(decoder_context* ctx, byte_reader& r)
{
  uint32_t num_slots = r.u32();
  if (!r.ok() || num_slots > MAX_PICTURES) {
    return false;
  }

  ctx->dpb.dpb.clear();
  ctx->dpb.dpb.reserve(num_slots);

  uint32_t max_id = 0;
  for (uint32_t i = 0; i < num_slots; i++) {
    uint8_t live = r.u8();
    if (!r.ok() || live > 1) {
      return false;
    }

    if (!live) {
      ctx->dpb.dpb.push_back(new de265_image);
      continue;
    }

    de265_image* img = nullptr;
    if (!read_picture(ctx, r, num_slots, img)) {
      return false;
    }
    if (img->get_ID() >= max_id) {
      max_id = img->get_ID() + 1;
    }
    if (img->removed_at_picture_id != std::numeric_limits<uint32_t>::max() &&
        img->removed_at_picture_id >= max_id) {
      max_id = img->removed_at_picture_id + 1;
    }
    ctx->dpb.dpb.push_back(img);
  }

  /* Pictures allocated from now on must get IDs larger than every restored
     ID and every live removal marker, so that the "removed_at_picture_id >
     current ID" comparisons used during reference-set construction keep the
     same result. The stored removed_at_picture_id of a live picture is always
     UINT32_MAX (never removed) or 0 (already removed); 0 is below every
     restored ID, so only the picture IDs themselves matter for the bound. */
  de265_image::reserve_ID(max_id);

  return true;
}

decoder_context* checkpoint_state::deserialize(const void* buffer, size_t buffer_size,
                                               de265_error* err)
{
  auto fail = [err](de265_error e) -> decoder_context* {
    if (err) *err = e;
    return nullptr;
  };

  if (buffer == nullptr) {
    return fail(DE265_ERROR_INVALID_ARGUMENT);
  }
  if (buffer_size < sizeof(MAGIC) + 2 + 2 + 8) {
    return fail(DE265_ERROR_CHECKPOINT_FORMAT);
  }

  byte_reader r(static_cast<const uint8_t*>(buffer), buffer_size);

  uint8_t magic[8];
  if (!r.raw(magic, sizeof(magic)) || memcmp(magic, MAGIC, sizeof(MAGIC)) != 0) {
    return fail(DE265_ERROR_CHECKPOINT_FORMAT);
  }
  uint16_t version = r.u16();
  uint16_t flags = r.u16();
  uint64_t total = r.u64();
  if (!r.ok()) {
    return fail(DE265_ERROR_CHECKPOINT_FORMAT);
  }
  if (version != format_version) {
    return fail(DE265_ERROR_CHECKPOINT_FORMAT);
  }
  if (flags != 0) {
    return fail(DE265_ERROR_CHECKPOINT_FORMAT);
  }
  if (total != buffer_size) {
    /* Either truncated or carrying trailing garbage. */
    return fail(DE265_ERROR_CHECKPOINT_FORMAT);
  }

  /* Allocate an initialized context through the public API so that the
     library initialization count is balanced with de265_free_decoder(). */
  decoder_context* ctx = reinterpret_cast<decoder_context*>(de265_new_decoder());
  if (ctx == nullptr) {
    return fail(DE265_ERROR_OUT_OF_MEMORY);
  }

  bool ok = false;
  do {
    /* --- decoder state --- */
    ctx->current_image_poc_lsb = r.i32();
    uint8_t first_pic = r.u8();
    uint8_t no_rasl = r.u8();
    uint8_t handle_cra = r.u8();
    uint8_t first_eos = r.u8();
    ctx->PicOrderCntMsb = r.i32();
    ctx->prevPicOrderCntLsb = r.i32();
    ctx->prevPicOrderCntMsb = r.i32();

    ctx->nal_unit_type = r.u8();
    uint8_t idr = r.u8();
    uint8_t rap = r.u8();

    ctx->NumPocStCurrBefore = r.i32();
    ctx->NumPocStCurrAfter = r.i32();
    ctx->NumPocStFoll = r.i32();
    ctx->NumPocLtCurr = r.i32();
    ctx->NumPocLtFoll = r.i32();
    if (!r.ok()) break;

    auto in_ref_range = [](int v) { return v >= 0 && v <= MAX_NUM_REF_PICS; };
    if (!in_ref_range(ctx->NumPocStCurrBefore) || !in_ref_range(ctx->NumPocStCurrAfter) ||
        !in_ref_range(ctx->NumPocStFoll) || !in_ref_range(ctx->NumPocLtCurr) ||
        !in_ref_range(ctx->NumPocLtFoll)) {
      break;
    }
    if (first_pic > 1 || no_rasl > 1 || handle_cra > 1 || first_eos > 1 ||
        idr > 1 || rap > 1) {
      break;
    }

    for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
      ctx->PocStCurrBefore[i] = r.i32();
      ctx->PocStCurrAfter[i] = r.i32();
      ctx->PocStFoll[i] = r.i32();
      ctx->PocLtCurr[i] = r.i32();
      ctx->PocLtFoll[i] = r.i32();
    }
    for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
      ctx->RefPicSetStCurrBefore[i] = r.i32();
      ctx->RefPicSetStCurrAfter[i] = r.i32();
      ctx->RefPicSetStFoll[i] = r.i32();
      ctx->RefPicSetLtCurr[i] = r.i32();
      ctx->RefPicSetLtFoll[i] = r.i32();
    }
    for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
      ctx->PocLsbLt[i] = r.i32();
      ctx->UsedByCurrPicLt[i] = r.i32();
      ctx->DeltaPocMsbCycleLt[i] = r.u32();
    }
    for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
      uint8_t a = r.u8();
      uint8_t b = r.u8();
      if (a > 1 || b > 1) { r.need(0); }
      ctx->CurrDeltaPocMsbPresentFlag[i] = a;
      ctx->FollDeltaPocMsbPresentFlag[i] = b;
    }
    if (!r.ok()) break;

    ctx->limit_HighestTid = r.u8();
    ctx->goal_HighestTid = r.u8();
    ctx->current_HighestTid = r.u8();
    ctx->framerate_ratio = r.u8();
    ctx->layer_framerate_ratio = r.u8();
    if (!r.ok()) break;

    uint8_t cur_sps_plus1 = r.u8();
    uint8_t cur_pps_plus1 = r.u8();
    uint8_t cur_vps_plus1 = r.u8();
    if (!r.ok()) break;

    ctx->param_sei_check_hash = r.u8() != 0;
    ctx->param_conceal_stream_errors = r.u8() != 0;
    ctx->param_suppress_faulty_pictures = r.u8() != 0;
    ctx->param_disable_deblocking = r.u8() != 0;
    ctx->param_disable_sao = r.u8() != 0;
    ctx->param_security_limits.version = 1;
    ctx->param_security_limits.max_image_size_pixels = r.u32();
    ctx->param_security_limits.max_NAL_size_bytes = r.u32();
    ctx->param_security_limits.max_SEI_messages = r.u32();
    if (!r.ok()) break;

    ctx->dpb.max_images_in_DPB = r.u8();
    ctx->dpb.norm_images_in_DPB = r.u8();
    if (!r.ok()) break;
    if (ctx->dpb.max_images_in_DPB == 0 || ctx->dpb.norm_images_in_DPB == 0 ||
        ctx->dpb.norm_images_in_DPB > ctx->dpb.max_images_in_DPB) {
      break;
    }

    /* Restore the scalar flags now that all bytes have been read. */
    ctx->first_decoded_picture = (first_pic != 0);
    ctx->NoRaslOutputFlag = (no_rasl != 0);
    ctx->HandleCraAsBlaFlag = (handle_cra != 0);
    ctx->FirstAfterEndOfSequenceNAL = (first_eos != 0);
    ctx->IdrPicFlag = (idr != 0);
    ctx->RapPicFlag = (rap != 0);

    /* --- parameter sets --- */
    if (!read_parameter_sets(ctx, r)) break;

    /* Current parameter-set pointers (0 means "none"). */
    ctx->current_sps.reset();
    ctx->current_pps.reset();
    ctx->current_vps.reset();
    if (cur_sps_plus1 != 0) {
      int id = cur_sps_plus1 - 1;
      if (id >= DE265_MAX_SPS_SETS || !ctx->sps[id]) break;
      ctx->current_sps = ctx->sps[id];
    }
    if (cur_pps_plus1 != 0) {
      int id = cur_pps_plus1 - 1;
      if (id >= DE265_MAX_PPS_SETS || !ctx->pps[id]) break;
      ctx->current_pps = ctx->pps[id];
    }
    if (cur_vps_plus1 != 0) {
      int id = cur_vps_plus1 - 1;
      if (id >= DE265_MAX_VPS_SETS || !ctx->vps[id]) break;
      ctx->current_vps = ctx->vps[id];
    }

    /* --- DPB --- */
    if (!read_dpb(ctx, r)) break;
    uint32_t num_slots = static_cast<uint32_t>(ctx->dpb.dpb.size());

    if (!read_queues(ctx, r, num_slots)) break;

    /* The checkpoint must be consumed exactly. */
    if (!r.ok() || r.remaining() != 0) break;

    /* Internal pointers that are not valid between pictures. */
    ctx->img = nullptr;
    ctx->previous_slice_header = nullptr;
    ctx->flush_reorder_buffer_at_this_frame = false;

    ok = true;
  } while (false);

  if (!ok) {
    de265_free_decoder(reinterpret_cast<de265_decoder_context*>(ctx));
    return fail(DE265_ERROR_CHECKPOINT_FORMAT);
  }

  if (err) *err = DE265_OK;
  return ctx;
}

/* ======================================================================== */
/* Public C API                                                             */
/* ======================================================================== */

extern "C" {

LIBDE265_API size_t de265_checkpoint_size(de265_decoder_context* de265ctx)
{
  decoder_context* ctx = reinterpret_cast<decoder_context*>(de265ctx);
  if (!checkpoint_state::is_available(ctx, nullptr)) {
    return 0;
  }
  std::vector<uint8_t> bytes;
  if (!checkpoint_state::serialize(ctx, bytes)) {
    return 0;
  }
  return bytes.size();
}

LIBDE265_API de265_error de265_checkpoint_write(de265_decoder_context* de265ctx,
                                                void* buffer, size_t buffer_size)
{
  decoder_context* ctx = reinterpret_cast<decoder_context*>(de265ctx);
  if (!checkpoint_state::is_available(ctx, nullptr)) {
    return DE265_ERROR_CHECKPOINT_UNAVAILABLE;
  }

  std::vector<uint8_t> bytes;
  if (!checkpoint_state::serialize(ctx, bytes)) {
    return DE265_ERROR_CHECKPOINT_UNAVAILABLE;
  }

  if (bytes.empty() || buffer == nullptr || buffer_size < bytes.size()) {
    return DE265_ERROR_INVALID_ARGUMENT;
  }

  memcpy(buffer, bytes.data(), bytes.size());
  return DE265_OK;
}

LIBDE265_API de265_decoder_context* de265_checkpoint_read(const void* buffer,
                                                          size_t buffer_size,
                                                          de265_error* err)
{
  if (err) { *err = DE265_OK; }
  decoder_context* ctx = checkpoint_state::deserialize(buffer, buffer_size, err);
  return reinterpret_cast<de265_decoder_context*>(ctx);
}

} // extern "C"
