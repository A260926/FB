// row_cases.cc - task-specific test suite for per CTU row CABAC restart.
//
// Registers five groups through task_support/check.h:
//   entropy         - XRST directory, fresh CABAC/QP state per row
//   neighbors       - spatial isolation at CTU row borders
//   reconstruction  - forward/reverse row scheduling, formats, B frames
//   malformed       - directory / length / termination rejection via C API
//   compat          - setter validation and version 2.0/3.0 compatibility
//
// Everything here uses C++11 plus the standard library and the production
// xvc static libraries only.

#include "check.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xvc_common_lib/cabac.h"
#include "xvc_common_lib/coding_unit.h"
#include "xvc_common_lib/picture_data.h"
#include "xvc_common_lib/quantize.h"
#include "xvc_common_lib/restrictions.h"
#include "xvc_common_lib/segment_header.h"
#include "xvc_dec_lib/decoder.h"
#include "xvc_dec_lib/xvcdec.h"
#include "xvc_enc_lib/xvcenc.h"

namespace {

// ---------------------------------------------------------------------------
// Small configurable encode/decode harness
// ---------------------------------------------------------------------------

struct Cfg {
  int width = 160;
  int height = 96;
  int bitdepth = 8;
  int chroma = 1;          // xvc_enc_chroma_format: 1 = 4:2:0, 3 = 4:4:4
  int frames = 1;
  int sub_gop = 1;
  int num_ref_pics = 0;    // 0 = all intra, -1 = encoder default
  int qp = 32;
  int speed = 2;
  int flat = 0;
  int row_restart = 1;
};

struct VNal {
  std::vector<uint8_t> d;
  int type = 0;
};

struct EncOut {
  std::vector<VNal> nals;
  std::vector<std::vector<uint8_t> > recs;
};

static void FillFrame(const Cfg &cfg, int frame_idx,
                      std::vector<uint8_t> *bytes) {
  int cw = cfg.chroma == 3 ? cfg.width : (cfg.width + 1) / 2;
  int ch = cfg.chroma == 3 ? cfg.height : (cfg.height + 1) / 2;
  int bps = cfg.bitdepth == 8 ? 1 : 2;
  size_t samples = static_cast<size_t>(cfg.width) * cfg.height +
    2u * static_cast<size_t>(cw) * ch;
  bytes->assign(samples * bps, 0);
  if (cfg.flat) {
    int mid = 1 << (cfg.bitdepth - 1);
    if (bps == 1) {
      std::memset(bytes->data(), mid, bytes->size());
    } else {
      uint16_t *p = reinterpret_cast<uint16_t*>(bytes->data());
      for (size_t i = 0; i < samples; ++i) {
        p[i] = static_cast<uint16_t>(mid);
      }
    }
    return;
  }
  if (bps == 1) {
    for (size_t k = 0; k < bytes->size(); ++k) {
      (*bytes)[k] = static_cast<uint8_t>(
        (k * 13 + (k / cfg.width) * 17 + frame_idx * 23 +
         (k % 19) * (frame_idx + 1)) & 255);
    }
  } else {
    uint16_t *p = reinterpret_cast<uint16_t*>(bytes->data());
    for (size_t k = 0; k < samples; ++k) {
      p[k] = static_cast<uint16_t>(
        ((k * 13 + (k / cfg.width) * 17 + frame_idx * 23 +
          (k % 19) * (frame_idx + 1)) * 4) & 1023);
    }
  }
}

static int DoEncode(const Cfg &cfg, EncOut *out) {
  const xvc_encoder_api *e = xvc_encoder_api_get();
  xvc_encoder_parameters *p = e->parameters_create();
  if (!p || e->parameters_set_default(p) != XVC_ENC_OK) {
    return 1;
  }
  p->width = cfg.width;
  p->height = cfg.height;
  p->input_bitdepth = cfg.bitdepth;
  p->internal_bitdepth = cfg.bitdepth;
  p->chroma_format = static_cast<xvc_enc_chroma_format>(cfg.chroma);
  p->speed_mode = cfg.speed;
  p->qp = cfg.qp;
  p->sub_gop_length = cfg.sub_gop;
  p->max_keypic_distance = 64;
  p->simd_mask = 0;
  p->threads = 0;
  p->num_ref_pics = cfg.num_ref_pics;
  if (xvc_encoder_set_row_restart(p, cfg.row_restart) != XVC_ENC_OK) {
    e->parameters_destroy(p);
    return 2;
  }
  if (e->parameters_check(p) != XVC_ENC_OK) {
    e->parameters_destroy(p);
    return 3;
  }
  xvc_encoder *enc = e->encoder_create(p);
  if (!enc) {
    e->parameters_destroy(p);
    return 4;
  }
  xvc_enc_pic_buffer *rec = e->picture_create(enc);

  auto capture_rec = [&]() {
    if (rec->pic && rec->size) {
      out->recs.emplace_back(rec->pic, rec->pic + rec->size);
    }
  };
  auto take = [&](xvc_enc_nal_unit *nals, int count) {
    for (int i = 0; i < count; ++i) {
      VNal n;
      n.d.assign(nals[i].bytes, nals[i].bytes + nals[i].size);
      n.type = (nals[i].bytes[0] >> 1) & 31;
      out->nals.push_back(n);
    }
  };

  std::vector<uint8_t> frame;
  for (int f = 0; f < cfg.frames; ++f) {
    FillFrame(cfg, f, &frame);
    xvc_enc_nal_unit *nals = nullptr;
    int count = 0;
    if (e->encoder_encode(enc, frame.data(), &nals, &count, rec)
        != XVC_ENC_OK) {
      e->picture_destroy(rec);
      e->encoder_destroy(enc);
      e->parameters_destroy(p);
      return 5;
    }
    capture_rec();
    take(nals, count);
  }
  bool flushed = false;
  for (int i = 0; i < 64; ++i) {
    xvc_enc_nal_unit *nals = nullptr;
    int count = 0;
    xvc_enc_return_code rc =
      e->encoder_flush(enc, &nals, &count, rec);
    if (rc != XVC_ENC_OK && rc != XVC_ENC_NO_MORE_OUTPUT) {
      break;
    }
    capture_rec();
    take(nals, count);
    if (rc == XVC_ENC_NO_MORE_OUTPUT) {
      flushed = true;
      break;
    }
  }
  e->picture_destroy(rec);
  e->encoder_destroy(enc);
  e->parameters_destroy(p);
  return flushed ? 0 : 6;
}

struct DecOut {
  std::vector<std::vector<uint8_t> > pics;
  std::vector<xvc::PictureDecoder::RowDiag> diag;
  int bad = 0;
  int decode_rc = XVC_DEC_OK;
};

static int DoDecode(const std::vector<VNal> &nals, int out_bitdepth,
                    int order, DecOut *out, bool require_nal_ok = true) {
  const xvc_decoder_api *d = xvc_decoder_api_get();
  xvc_decoder_parameters *dp = d->parameters_create();
  if (!dp || d->parameters_set_default(dp) != XVC_DEC_OK) {
    return 1;
  }
  dp->output_bitdepth = out_bitdepth;
  dp->threads = 0;
  dp->simd_mask = 0;
  xvc_decoder *base = d->decoder_create(dp);
  if (!base) {
    d->parameters_destroy(dp);
    return 2;
  }
  xvc::Decoder *dec = static_cast<xvc::Decoder*>(base);
  dec->SetRowDecodeOrderForTest(order);
  xvc_decoded_picture *pic = d->picture_create(base);

  auto drain = [&]() {
    xvc_dec_return_code rc;
    while ((rc = d->decoder_get_picture(base, pic)) == XVC_DEC_OK) {
      out->pics.emplace_back(pic->bytes, pic->bytes + pic->size);
    }
  };

  for (size_t i = 0; i < nals.size(); ++i) {
    xvc_dec_return_code rc =
      d->decoder_decode_nal(base, nals[i].d.data(), nals[i].d.size(), 0);
    if (rc != XVC_DEC_OK) {
      out->decode_rc = rc;
      if (require_nal_ok) {
        d->picture_destroy(pic);
        d->decoder_destroy(base);
        d->parameters_destroy(dp);
        return 3;
      }
    }
    drain();
  }
  d->decoder_flush(base);
  drain();
  d->decoder_check_conformance(base, &out->bad);
  out->diag = dec->GetRowDiagnosticsForTest();
  d->picture_destroy(pic);
  d->decoder_destroy(base);
  d->parameters_destroy(dp);
  return 0;
}

// ---------------------------------------------------------------------------
// Independent XRST directory interpreter (does not use library parsing)
// ---------------------------------------------------------------------------

struct RowDir {
  size_t magic = 0;
  int rows = 0;
  int reserved = 0;
  std::vector<uint32_t> len;
  std::vector<size_t> offset;
  size_t rows_start = 0;
  size_t rows_end = 0;
};

static uint32_t Be16(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 8) | p[1];
}
static uint32_t Be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
    (static_cast<uint32_t>(p[1]) << 16) |
    (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

static int InterpretDir(const std::vector<uint8_t> &n, int expect_rows,
                        RowDir *rd) {
  size_t magic = static_cast<size_t>(-1);
  for (size_t i = 0; i + 4 <= n.size(); ++i) {
    if (n[i] == 'X' && n[i + 1] == 'R' &&
        n[i + 2] == 'S' && n[i + 3] == 'T') {
      magic = i;
      break;
    }
  }
  if (magic == static_cast<size_t>(-1)) {
    return 10;  // missing magic
  }
  if (magic + 8 > n.size()) {
    return 11;  // truncated fixed directory header
  }
  int rows = static_cast<int>(Be16(&n[magic + 4]));
  rd->reserved = static_cast<int>(Be16(&n[magic + 6]));
  if (rows != expect_rows || rows <= 0 || rd->reserved != 0) {
    return 12;
  }
  size_t lengths_bytes = static_cast<size_t>(rows) * 4u;
  if (lengths_bytes > n.size() ||
      magic + 8 > n.size() - lengths_bytes) {
    return 13;  // truncated length table
  }
  rd->magic = magic;
  rd->rows = rows;
  rd->len.assign(rows, 0);
  rd->offset.assign(rows, 0);
  uint64_t sum = 0;
  for (int i = 0; i < rows; ++i) {
    uint32_t l = Be32(&n[magic + 8 + static_cast<size_t>(i) * 4]);
    if (l == 0) {
      return 14;  // zero length row
    }
    rd->len[i] = l;
    sum += l;
    if (sum > n.size()) {
      return 15;  // integer / cumulative overflow guard
    }
  }
  rd->rows_start = magic + 8 + lengths_bytes;
  if (sum > static_cast<uint64_t>(n.size() - rd->rows_start)) {
    return 16;  // lengths run past the NAL
  }
  size_t off = rd->rows_start;
  for (int i = 0; i < rows; ++i) {
    rd->offset[i] = off;
    // Independent per-row range validation.
    if (rd->offset[i] + rd->len[i] > n.size()) {
      return 17;
    }
    off += rd->len[i];
  }
  rd->rows_end = off;
  return 0;
}

static const VNal* FindSegment(const EncOut &enc) {
  for (size_t i = 0; i < enc.nals.size(); ++i) {
    if (enc.nals[i].type == 16) {
      return &enc.nals[i];
    }
  }
  return nullptr;
}

static const VNal* FindRowPicture(const EncOut &enc) {
  for (size_t i = 0; i < enc.nals.size(); ++i) {
    const std::vector<uint8_t> &b = enc.nals[i].d;
    for (size_t j = 0; j + 4 <= b.size(); ++j) {
      if (b[j] == 'X' && b[j + 1] == 'R' &&
          b[j + 2] == 'S' && b[j + 3] == 'T') {
        return &enc.nals[i];
      }
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// entropy group
// ---------------------------------------------------------------------------

static int case_entropy_directory_geometry(void) {
  Cfg cfg;  // 160x96 -> 3 CTU columns x 2 CTU rows, right/bottom truncation
  EncOut enc;
  TASK_CHECK(DoEncode(cfg, &enc) == 0);
  const VNal *pic = FindRowPicture(enc);
  TASK_CHECK(pic != nullptr);
  const int expect_cols = (cfg.width + 63) / 64;
  const int expect_rows = (cfg.height + 63) / 64;
  TASK_CHECK(expect_cols == 3);
  TASK_CHECK(expect_rows == 2);
  RowDir dir;
  TASK_CHECK(InterpretDir(pic->d, expect_rows, &dir) == 0);
  TASK_CHECK(dir.rows == 2);
  TASK_CHECK(dir.len.size() == 2);
  uint64_t total = 0;
  for (int i = 0; i < dir.rows; ++i) {
    TASK_CHECK(dir.len[i] > 16);
    TASK_CHECK(dir.offset[i] + dir.len[i] <= pic->d.size());
    // Rows are contiguous with no gaps.
    if (i > 0) {
      TASK_CHECK(dir.offset[i] == dir.offset[i - 1] + dir.len[i - 1]);
    }
    total += dir.len[i];
  }
  TASK_CHECK(dir.rows_end == dir.offset[1] + dir.len[1]);
  TASK_CHECK(total > 0);
  return 0;
}

static int case_entropy_context_qp_reset(void) {
  Cfg cfg;
  EncOut enc;
  TASK_CHECK(DoEncode(cfg, &enc) == 0);
  DecOut dec;
  TASK_CHECK(DoDecode(enc.nals, 8, 0, &dec) == 0);
  TASK_CHECK(dec.bad == 0);
  // One picture with two rows -> two diagnostics records.
  TASK_CHECK(dec.diag.size() == 2);
  TASK_CHECK(dec.diag[0].row == 0);
  TASK_CHECK(dec.diag[1].row == 1);
  TASK_CHECK(dec.diag[0].context_fingerprint != 0);
  TASK_CHECK(dec.diag[0].context_fingerprint ==
             dec.diag[1].context_fingerprint);
  TASK_CHECK(dec.diag[0].pic_qp == dec.diag[1].pic_qp);
  TASK_CHECK(dec.diag[0].predicted_qp_first == dec.diag[0].pic_qp);
  TASK_CHECK(dec.diag[1].predicted_qp_first == dec.diag[1].pic_qp);
  TASK_CHECK(dec.diag[0].consumed_bytes == dec.diag[0].stream_length);
  TASK_CHECK(dec.diag[1].consumed_bytes == dec.diag[1].stream_length);

  // Independently build the expected fresh context fingerprint.
  xvc::Qp qp(dec.diag[0].pic_qp, xvc::ChromaFormat::k420, 8, 0.0);
  uint32_t expected_fp = 0;
  if (xvc::Restrictions::Get().disable_cabac_ctx_update) {
    xvc::CabacContexts<xvc::ContextModelStatic> ctx;
    ctx.ResetStates(qp, xvc::PicturePredictionType::kIntra);
    expected_fp = ctx.DiagnosticsFingerprint();
  } else {
    xvc::CabacContexts<xvc::ContextModelDynamic> ctx;
    ctx.ResetStates(qp, xvc::PicturePredictionType::kIntra);
    expected_fp = ctx.DiagnosticsFingerprint();
  }
  TASK_CHECK(expected_fp != 0);
  TASK_CHECK(dec.diag[0].context_fingerprint == expected_fp);
  // Intra 4:2:0 uses the separate chroma CU tree.
  TASK_CHECK(dec.diag[0].secondary_tree == 1);
  TASK_CHECK(dec.diag[1].secondary_tree == 1);
  return 0;
}

static int case_entropy_zero_residual(void) {
  Cfg cfg;
  cfg.flat = 1;
  cfg.frames = 2;
  EncOut enc;
  TASK_CHECK(DoEncode(cfg, &enc) == 0);
  DecOut fwd, rev;
  TASK_CHECK(DoDecode(enc.nals, 8, 0, &fwd) == 0);
  TASK_CHECK(DoDecode(enc.nals, 8, 1, &rev) == 0);
  TASK_CHECK(fwd.bad == 0 && rev.bad == 0);
  TASK_CHECK(fwd.pics.size() == 2);
  TASK_CHECK(fwd.pics == rev.pics);
  return 0;
}

// ---------------------------------------------------------------------------
// neighbors group - direct PictureData spatial band tests
// ---------------------------------------------------------------------------

static xvc::PictureData* MakeBandTestData(void) {
  xvc::SegmentHeader seg;
  seg.max_binary_split_depth = 3;
  seg.chroma_qp_offset_table = 1;
  seg.chroma_qp_offset_u = 0;
  seg.chroma_qp_offset_v = 0;
  seg.adaptive_qp = 0;
  xvc::PictureData *pd =
    new xvc::PictureData(xvc::ChromaFormat::k420, 160, 96, 8);
  xvc::Qp qp(32, xvc::ChromaFormat::k420, 8, 0.0);
  pd->Init(seg, qp, true);
  // Populate the CU table as if CTU 0 / row 1 CTUs were coded.
  pd->MarkUsedInPic(pd->GetCtu(xvc::CuTree::Primary, 0));
  pd->MarkUsedInPic(pd->GetCtu(xvc::CuTree::Primary, 3));  // (0,64)
  pd->MarkUsedInPic(pd->GetCtu(xvc::CuTree::Primary, 4));  // (64,64)
  return pd;
}

static int case_neighbors_row_band(void) {
  xvc::PictureData *pd = MakeBandTestData();
  TASK_CHECK(pd->GetCtuNumX() == 3);
  TASK_CHECK(pd->GetCtuNumY() == 2);
  xvc::CodingUnit *first_row1 = pd->GetCtu(xvc::CuTree::Primary, 3);
  xvc::CodingUnit *second_row1 = pd->GetCtu(xvc::CuTree::Primary, 4);

  // Without isolation the row above is reachable.
  TASK_CHECK(first_row1->GetCodingUnitAbove() != nullptr);
  TASK_CHECK(second_row1->GetCodingUnitAboveLeft() != nullptr);

  // With the row-1 band active, top / above-left neighbors are unavailable.
  pd->SetSpatialRowBand(true, 64, 128);
  TASK_CHECK(first_row1->GetCodingUnitAbove() == nullptr);
  TASK_CHECK(first_row1->GetCodingUnitAboveLeft() == nullptr);
  TASK_CHECK(second_row1->GetCodingUnitAbove() == nullptr);
  TASK_CHECK(second_row1->GetCodingUnitAboveLeft() == nullptr);
  TASK_CHECK(second_row1->GetCodingUnitAboveRight() == nullptr);
  // In-row left neighbor stays available.
  TASK_CHECK(second_row1->GetCodingUnitLeft() != nullptr);
  TASK_CHECK(second_row1->GetCodingUnitLeft() == first_row1);

  // A lookup below the band is hidden even if the table were populated.
  pd->SetSpatialRowBand(true, 64, 96);
  TASK_CHECK(pd->GetCuAt(xvc::CuTree::Primary, 0, 100) == nullptr);
  pd->SetSpatialRowBand(false, 0, 0);
  TASK_CHECK(pd->GetCuAt(xvc::CuTree::Primary, 0, 100) != nullptr);

  // Turning isolation off restores the above row.
  TASK_CHECK(first_row1->GetCodingUnitAbove() != nullptr);

  // A second instance keeps independent band state.
  xvc::PictureData other(xvc::ChromaFormat::k420, 160, 96, 8);
  TASK_CHECK(!other.IsSpatialRowBandActive());
  TASK_CHECK(!pd->IsSpatialRowBandActive());
  pd->SetSpatialRowBand(true, 64, 128);
  TASK_CHECK(!other.IsSpatialRowBandActive());
  TASK_CHECK(other.GetSpatialRowTop() == 0);
  TASK_CHECK(pd->GetSpatialRowTop() == 64);
  delete pd;
  return 0;
}

static int case_neighbors_qp_predictor(void) {
  xvc::PictureData *pd = MakeBandTestData();
  xvc::CodingUnit *row0 = pd->GetCtu(xvc::CuTree::Primary, 0);
  xvc::CodingUnit *first_row1 = pd->GetCtu(xvc::CuTree::Primary, 3);
  xvc::CodingUnit *second_row1 = pd->GetCtu(xvc::CuTree::Primary, 4);
  TASK_CHECK(pd->GetPicQp()->GetQpRaw(xvc::YuvComponent::kY) == 32);

  // Distinctive QP on the previous row.
  row0->SetQp(41);
  TASK_CHECK(row0->GetQp(xvc::YuvComponent::kY) == 41);

  // Isolated first CTU in the row resets to picture initial QP.
  pd->SetSpatialRowBand(true, 64, 128);
  TASK_CHECK(first_row1->GetPredictedQp() == 32);

  // Once the first CTU carries its own QP, the next CTU predicts from the
  // in-row left neighbor (never from above).
  first_row1->SetQp(35);
  TASK_CHECK(second_row1->GetPredictedQp() == 35);

  // Without isolation the above-row QP leaks into prediction.
  pd->SetSpatialRowBand(false, 0, 0);
  TASK_CHECK(first_row1->GetPredictedQp() == 41);
  delete pd;
  return 0;
}

// ---------------------------------------------------------------------------
// reconstruction group
// ---------------------------------------------------------------------------

static int CheckForwardReverse(const Cfg &cfg, bool compare_enc_rec) {
  EncOut enc;
  TASK_CHECK(DoEncode(cfg, &enc) == 0);
  DecOut fwd, rev;
  TASK_CHECK(DoDecode(enc.nals, cfg.bitdepth, 0, &fwd) == 0);
  TASK_CHECK(DoDecode(enc.nals, cfg.bitdepth, 1, &rev) == 0);
  TASK_CHECK(fwd.bad == 0);
  TASK_CHECK(rev.bad == 0);
  TASK_CHECK(static_cast<int>(fwd.pics.size()) == cfg.frames);
  TASK_CHECK(fwd.pics.size() == rev.pics.size());
  // Opposite row scheduling reconstructs identical pictures.
  TASK_CHECK(fwd.pics == rev.pics);
  if (compare_enc_rec) {
    TASK_CHECK(fwd.pics.size() == enc.recs.size());
    TASK_CHECK(fwd.pics == enc.recs);
  }
  return 0;
}

static int case_recon_truncated_texture(void) {
  Cfg cfg;
  cfg.frames = 2;
  TASK_CHECK(CheckForwardReverse(cfg, true) == 0);
  return 0;
}

static int case_recon_b_frame_delay(void) {
  Cfg cfg;
  cfg.width = 128;
  cfg.height = 128;
  cfg.frames = 9;
  cfg.sub_gop = 4;
  cfg.num_ref_pics = -1;
  TASK_CHECK(CheckForwardReverse(cfg, false) == 0);
  return 0;
}

static int case_recon_10bit_420(void) {
  Cfg cfg;
  cfg.bitdepth = 10;
  cfg.frames = 2;
  TASK_CHECK(CheckForwardReverse(cfg, false) == 0);
  return 0;
}

static int case_recon_8bit_444(void) {
  Cfg cfg;
  cfg.chroma = 3;
  cfg.frames = 2;
  TASK_CHECK(CheckForwardReverse(cfg, false) == 0);
  return 0;
}

static int case_recon_10bit_444(void) {
  Cfg cfg;
  cfg.bitdepth = 10;
  cfg.chroma = 3;
  TASK_CHECK(CheckForwardReverse(cfg, false) == 0);
  return 0;
}

static int case_recon_interleaved_instances(void) {
  Cfg cfg;
  cfg.frames = 3;
  EncOut enc;
  TASK_CHECK(DoEncode(cfg, &enc) == 0);
  const xvc_decoder_api *d = xvc_decoder_api_get();

  xvc_decoder_parameters *p0 = d->parameters_create();
  d->parameters_set_default(p0);
  p0->output_bitdepth = 8;
  p0->threads = 0;
  p0->simd_mask = 0;
  xvc_decoder *b0 = d->decoder_create(p0);
  xvc_decoder_parameters *p1 = d->parameters_create();
  d->parameters_set_default(p1);
  p1->output_bitdepth = 8;
  p1->threads = 0;
  p1->simd_mask = 0;
  xvc_decoder *b1 = d->decoder_create(p1);
  TASK_CHECK(b0 != nullptr && b1 != nullptr);
  static_cast<xvc::Decoder*>(b0)->SetRowDecodeOrderForTest(0);
  static_cast<xvc::Decoder*>(b1)->SetRowDecodeOrderForTest(1);
  xvc_decoded_picture *o0 = d->picture_create(b0);
  xvc_decoded_picture *o1 = d->picture_create(b1);

  std::vector<std::vector<uint8_t> > q0, q1;
  // Interleave NAL calls between the two independent instances.
  for (size_t i = 0; i < enc.nals.size(); ++i) {
    TASK_CHECK(d->decoder_decode_nal(b0, enc.nals[i].d.data(),
                                     enc.nals[i].d.size(), 0) == XVC_DEC_OK);
    TASK_CHECK(d->decoder_decode_nal(b1, enc.nals[i].d.data(),
                                     enc.nals[i].d.size(), 0) == XVC_DEC_OK);
    xvc_dec_return_code r0, r1;
    while ((r0 = d->decoder_get_picture(b0, o0)) == XVC_DEC_OK) {
      q0.emplace_back(o0->bytes, o0->bytes + o0->size);
    }
    while ((r1 = d->decoder_get_picture(b1, o1)) == XVC_DEC_OK) {
      q1.emplace_back(o1->bytes, o1->bytes + o1->size);
    }
  }
  d->decoder_flush(b0);
  d->decoder_flush(b1);
  while (d->decoder_get_picture(b0, o0) == XVC_DEC_OK) {
    q0.emplace_back(o0->bytes, o0->bytes + o0->size);
  }
  while (d->decoder_get_picture(b1, o1) == XVC_DEC_OK) {
    q1.emplace_back(o1->bytes, o1->bytes + o1->size);
  }
  TASK_CHECK(q0.size() == 3);
  TASK_CHECK(q0 == q1);
  d->picture_destroy(o0);
  d->picture_destroy(o1);
  d->decoder_destroy(b0);
  d->decoder_destroy(b1);
  d->parameters_destroy(p0);
  d->parameters_destroy(p1);
  return 0;
}

// ---------------------------------------------------------------------------
// malformed group - all rejection observed through the public C decoder API
// ---------------------------------------------------------------------------

struct MalResult {
  int decode_rc = XVC_DEC_OK;
  int picture_rc = XVC_DEC_OK;
  int bad = 0;
  bool discard = false;
};

static int RunMalformed(const VNal &seg,
                        const std::vector<uint8_t> &bad_pic, MalResult *r) {
  const xvc_decoder_api *d = xvc_decoder_api_get();
  xvc_decoder_parameters *dp = d->parameters_create();
  d->parameters_set_default(dp);
  dp->output_bitdepth = 8;
  dp->threads = 0;
  dp->simd_mask = 0;
  xvc_decoder *base = d->decoder_create(dp);
  xvc::Decoder *dec = static_cast<xvc::Decoder*>(base);
  xvc_decoded_picture *out = d->picture_create(base);
  d->decoder_decode_nal(base, seg.d.data(), seg.d.size(), 0);
  r->decode_rc = d->decoder_decode_nal(base, bad_pic.data(),
                                       bad_pic.size(), 0);
  d->decoder_flush(base);
  r->picture_rc = d->decoder_get_picture(base, out);
  d->decoder_check_conformance(base, &r->bad);
  r->discard = dec->HasDiscardPictureForTest();
  d->picture_destroy(out);
  d->decoder_destroy(base);
  d->parameters_destroy(dp);
  return 0;
}

struct BaseFixture {
  EncOut enc;
  const VNal *seg = nullptr;
  const VNal *pic = nullptr;
  RowDir dir;
};

static int BuildFixture(BaseFixture *f) {
  Cfg cfg;
  if (DoEncode(cfg, &f->enc) != 0) {
    return 1;
  }
  f->seg = FindSegment(f->enc);
  f->pic = FindRowPicture(f->enc);
  if (!f->seg || !f->pic) {
    return 2;
  }
  return InterpretDir(f->pic->d, 2, &f->dir);
}

static void CheckRejectSignature(const MalResult &r, int *checks) {
  if (r.decode_rc != XVC_DEC_OK) {
    ++(*checks);
  }
  if (r.picture_rc != XVC_DEC_OK) {
    ++(*checks);
  }
  if (r.bad >= 1) {
    ++(*checks);
  }
  if (r.discard) {
    ++(*checks);
  }
}

static int case_malformed_missing_directory(void) {
  BaseFixture f;
  TASK_CHECK(BuildFixture(&f) == 0);
  std::vector<uint8_t> nal = f.pic->d;
  nal[f.dir.magic + 0] = 'Q';
  nal[f.dir.magic + 1] = 'X';
  nal[f.dir.magic + 2] = 'X';
  nal[f.dir.magic + 3] = 'X';
  MalResult r;
  TASK_CHECK(RunMalformed(*f.seg, nal, &r) == 0);
  TASK_CHECK(r.decode_rc != XVC_DEC_OK);
  TASK_CHECK(r.picture_rc != XVC_DEC_OK);  // no half picture published
  TASK_CHECK(r.bad >= 1);
  TASK_CHECK(r.discard);                   // kept out of the DPB/reference use
  return 0;
}

static int case_malformed_short_directory(void) {
  BaseFixture f;
  TASK_CHECK(BuildFixture(&f) == 0);
  std::vector<uint8_t> nal = f.pic->d;
  nal.resize(f.dir.magic + 5);  // claims more rows than bytes left
  MalResult r;
  TASK_CHECK(RunMalformed(*f.seg, nal, &r) == 0);
  TASK_CHECK(r.decode_rc != XVC_DEC_OK);
  TASK_CHECK(r.picture_rc != XVC_DEC_OK);
  TASK_CHECK(r.bad >= 1);
  TASK_CHECK(r.discard);
  return 0;
}

static int case_malformed_zero_length_row(void) {
  BaseFixture f;
  TASK_CHECK(BuildFixture(&f) == 0);
  std::vector<uint8_t> nal = f.pic->d;
  size_t len0 = f.dir.magic + 8;
  nal[len0 + 0] = nal[len0 + 1] = nal[len0 + 2] = nal[len0 + 3] = 0;
  MalResult r;
  TASK_CHECK(RunMalformed(*f.seg, nal, &r) == 0);
  TASK_CHECK(r.decode_rc != XVC_DEC_OK);
  TASK_CHECK(r.picture_rc != XVC_DEC_OK);
  TASK_CHECK(r.bad >= 1);
  TASK_CHECK(r.discard);
  return 0;
}

static int case_malformed_out_of_bounds_length(void) {
  BaseFixture f;
  TASK_CHECK(BuildFixture(&f) == 0);
  std::vector<uint8_t> nal = f.pic->d;
  size_t len0 = f.dir.magic + 8;
  nal[len0 + 0] = 0xff;
  nal[len0 + 1] = 0xff;
  nal[len0 + 2] = 0xff;
  nal[len0 + 3] = 0xff;
  MalResult r;
  TASK_CHECK(RunMalformed(*f.seg, nal, &r) == 0);
  TASK_CHECK(r.decode_rc != XVC_DEC_OK);
  TASK_CHECK(r.picture_rc != XVC_DEC_OK);
  TASK_CHECK(r.bad >= 1);
  TASK_CHECK(r.discard);
  return 0;
}

static int case_malformed_bad_termination(void) {
  BaseFixture f;
  TASK_CHECK(BuildFixture(&f) == 0);
  // Corrupt the termination/alignment tail of row 0. The exact bit that
  // breaks termination depends on the coded bytes, so try a small fixed grid
  // and require at least one corruption to produce the full hard-reject
  // signature through the public decoder API (no crash).
  static const uint8_t masks[] = { 0x80, 0x40, 0xff, 0x01 };
  bool found_termination_reject = false;
  int signature_checks = 0;
  for (int back = 1; back <= 3 && !found_termination_reject; ++back) {
    size_t pos = f.dir.offset[0] + f.dir.len[0] - static_cast<size_t>(back);
    if (pos >= f.pic->d.size()) {
      continue;
    }
    for (size_t mi = 0; mi < sizeof(masks); ++mi) {
      std::vector<uint8_t> nal = f.pic->d;
      if (masks[mi] == 0x01 && back == 1) {
        nal[pos] = 0;  // zero the termination byte
      } else {
        nal[pos] ^= masks[mi];
      }
      MalResult r;
      TASK_CHECK(RunMalformed(*f.seg, nal, &r) == 0);
      CheckRejectSignature(r, &signature_checks);
      if (r.decode_rc != XVC_DEC_OK &&
          r.picture_rc != XVC_DEC_OK && r.bad >= 1 && r.discard) {
        found_termination_reject = true;
        break;
      }
    }
  }
  TASK_CHECK(signature_checks > 0);
  TASK_CHECK(found_termination_reject);
  return 0;
}

// ---------------------------------------------------------------------------
// compat group
// ---------------------------------------------------------------------------

static int case_compat_setter_validation(void) {
  const xvc_encoder_api *e = xvc_encoder_api_get();
  TASK_CHECK(xvc_encoder_set_row_restart(nullptr, 1) ==
             XVC_ENC_INVALID_ARGUMENT);
  xvc_encoder_parameters *p = e->parameters_create();
  TASK_CHECK(p != nullptr);
  TASK_CHECK(e->parameters_set_default(p) == XVC_ENC_OK);
  p->width = 128;
  p->height = 128;
  TASK_CHECK(p->row_restart == 0);  // default closed

  p->row_restart = 7;  // simulate a rejected earlier call leaving state alone
  TASK_CHECK(xvc_encoder_set_row_restart(p, 2) ==
             XVC_ENC_INVALID_ARGUMENT);
  TASK_CHECK(p->row_restart == 7);
  TASK_CHECK(xvc_encoder_set_row_restart(p, -1) ==
             XVC_ENC_INVALID_ARGUMENT);
  TASK_CHECK(p->row_restart == 7);

  TASK_CHECK(xvc_encoder_set_row_restart(p, 1) == XVC_ENC_OK);
  TASK_CHECK(p->row_restart == 1);
  TASK_CHECK(e->parameters_check(p) == XVC_ENC_OK);
  TASK_CHECK(xvc_encoder_set_row_restart(p, 0) == XVC_ENC_OK);
  TASK_CHECK(p->row_restart == 0);
  TASK_CHECK(e->parameters_check(p) == XVC_ENC_OK);

  p->row_restart = 5;
  TASK_CHECK(e->parameters_check(p) == XVC_ENC_INVALID_PARAMETER);
  e->parameters_destroy(p);
  return 0;
}

static int case_compat_version_2_still_reads(void) {
  Cfg cfg;
  cfg.row_restart = 0;
  cfg.frames = 2;
  EncOut enc;
  TASK_CHECK(DoEncode(cfg, &enc) == 0);
  const VNal *seg = FindSegment(enc);
  TASK_CHECK(seg != nullptr);
  TASK_CHECK(seg->d.size() >= 8);
  TASK_CHECK(seg->d[4] == 0);
  TASK_CHECK(seg->d[5] == 2);  // major version 2.0
  // No XRST directory in version 2.
  TASK_CHECK(FindRowPicture(enc) == nullptr);
  DecOut dec;
  TASK_CHECK(DoDecode(enc.nals, 8, 0, &dec) == 0);
  TASK_CHECK(dec.bad == 0);
  TASK_CHECK(dec.pics.size() == 2);
  TASK_CHECK(dec.pics == enc.recs);
  // The old decoder gate (major > 2 rejects) accepts this stream.
  int major = (static_cast<int>(seg->d[4]) << 8) | seg->d[5];
  TASK_CHECK(major == 2);
  TASK_CHECK(!(major > 2));
  return 0;
}

static int case_compat_version_3_old_gate_rejects(void) {
  Cfg cfg;
  EncOut enc;
  TASK_CHECK(DoEncode(cfg, &enc) == 0);
  const VNal *seg = FindSegment(enc);
  TASK_CHECK(seg != nullptr);
  TASK_CHECK(seg->d[4] == 0);
  TASK_CHECK(seg->d[5] == 3);  // explicit new major version 3.0
  int major = (static_cast<int>(seg->d[4]) << 8) | seg->d[5];
  // An unmodified version 2 decoder rejects major > 2.
  TASK_CHECK(major > 2);
  // The current decoder still reads it.
  DecOut dec;
  TASK_CHECK(DoDecode(enc.nals, 8, 0, &dec) == 0);
  TASK_CHECK(dec.bad == 0);
  TASK_CHECK(dec.pics.size() == 1);
  return 0;
}

}  // namespace

extern const struct task_case task_cases[] = {
  {"entropy", "directory_3x2_truncated", case_entropy_directory_geometry},
  {"entropy", "context_qp_reset", case_entropy_context_qp_reset},
  {"entropy", "zero_residual_flat", case_entropy_zero_residual},

  {"neighbors", "row_band_availability", case_neighbors_row_band},
  {"neighbors", "qp_predictor_reset", case_neighbors_qp_predictor},

  {"reconstruction", "truncated_texture", case_recon_truncated_texture},
  {"reconstruction", "b_frame_delay", case_recon_b_frame_delay},
  {"reconstruction", "bitdepth10_420", case_recon_10bit_420},
  {"reconstruction", "bitdepth8_444", case_recon_8bit_444},
  {"reconstruction", "bitdepth10_444", case_recon_10bit_444},
  {"reconstruction", "interleaved_instances",
   case_recon_interleaved_instances},

  {"malformed", "missing_directory", case_malformed_missing_directory},
  {"malformed", "short_directory", case_malformed_short_directory},
  {"malformed", "zero_length_row", case_malformed_zero_length_row},
  {"malformed", "out_of_bounds_length", case_malformed_out_of_bounds_length},
  {"malformed", "bad_termination", case_malformed_bad_termination},

  {"compat", "setter_validation", case_compat_setter_validation},
  {"compat", "version_2_still_reads", case_compat_version_2_still_reads},
  {"compat", "version_3_old_gate_rejects",
   case_compat_version_3_old_gate_rejects},
};

extern const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
