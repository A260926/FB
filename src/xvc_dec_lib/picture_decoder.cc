/******************************************************************************
* Copyright (C) 2018, Divideon.
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*
* This library is also available under a commercial license.
* Please visit https://xvc.io/license/ for more information.
******************************************************************************/

#include "xvc_dec_lib/picture_decoder.h"

#include <cassert>
#include <cstring>
#include <memory>
#include <utility>

#include "xvc_common_lib/deblocking_filter.h"
#include "xvc_common_lib/quantize.h"
#include "xvc_common_lib/resample.h"
#include "xvc_common_lib/restrictions.h"
#include "xvc_common_lib/segment_header.h"
#include "xvc_dec_lib/cu_decoder.h"
#include "xvc_dec_lib/entropy_decoder.h"

namespace xvc {

PictureDecoder::PictureDecoder(const SimdFunctions &simd,
                               const PictureFormat &pic_fmt,
                               int crop_width, int crop_height)
  : simd_(simd),
  output_resampler_(simd.resampler),
  output_format_(),
  pic_data_(std::make_shared<PictureData>(pic_fmt.chroma_format, pic_fmt.width,
                                          pic_fmt.height, pic_fmt.bitdepth)),
  rec_pic_(std::make_shared<YuvPicture>(pic_fmt.chroma_format, pic_fmt.width,
                                        pic_fmt.height, pic_fmt.bitdepth, true,
                                        crop_width, crop_height)) {
}

PictureDecoder::PicNalHeader
PictureDecoder::DecodeHeader(const SegmentHeader &segment_header,
                             BitReader *bit_reader, PicNum *sub_gop_end_poc,
                             PicNum *sub_gop_start_poc, PicNum *sub_gop_length,
                             PicNum prev_sub_gop_length, PicNum doc,
                             SegmentNum soc_counter, int num_buffered_nals) {
  // Start by reading the picture header data
  uint32_t header_byte = bit_reader->ReadBits(8);
  NalUnitType nal_unit_type = NalUnitType((header_byte >> 1) & 31);
  int buffer_flag = bit_reader->ReadBits(1);
  SegmentNum soc = (buffer_flag) ? soc_counter - 1 : soc_counter;
  int tid = bit_reader->ReadBits(3);
  if (nal_unit_type == NalUnitType::kIntraAccessPicture &&
      segment_header.leading_pictures) {
    *sub_gop_length = segment_header.max_sub_gop_length;
    *sub_gop_start_poc += doc > 1 ? constants::kMaxSubGopLength : 0;
    *sub_gop_end_poc = *sub_gop_start_poc;
  } else if (tid == 0) {
    PicNum length = segment_header.max_sub_gop_length;
    if (num_buffered_nals) {
      *sub_gop_length = prev_sub_gop_length;
    } else if (nal_unit_type == NalUnitType::kIntraAccessPicture) {
      *sub_gop_length = 1;
    } else if (length > 0) {
      *sub_gop_length = length;
    } else if (doc > 0) {
      *sub_gop_length = 1;
    }
    *sub_gop_start_poc = *sub_gop_end_poc;
  } else if (segment_header.max_sub_gop_length > *sub_gop_length) {
    *sub_gop_length = segment_header.max_sub_gop_length;
  }
  int pic_qp_ = bit_reader->ReadBits(7) - constants::kQpSignalBase;
  bool allow_lic = false;
  if (!Restrictions::Get().disable_ext2_inter_local_illumination_comp) {
    allow_lic = bit_reader->ReadBit() != 0;
  }
  bool deblock = segment_header.deblocking_mode != DeblockingMode::kDisabled;
  if (segment_header.deblocking_mode == DeblockingMode::kPerPicture) {
    deblock = bit_reader->ReadBit() != 0;
  }
  bit_reader->SkipBits();

  // Ensure that Sub Gop start is updated to include the current doc.
  if (doc > *sub_gop_end_poc) {
    *sub_gop_start_poc = *sub_gop_end_poc;
  }
  while (doc > *sub_gop_start_poc + *sub_gop_length) {
    *sub_gop_start_poc += *sub_gop_length;
  }
  if (doc > 0 && doc <= *sub_gop_start_poc) {
    doc = *sub_gop_start_poc + 1;
  }

  // The tid in the segment header can differ from the expected tid if:
  //  1. Temporal layers have been removed, or
  //  2. The Sub Gop is incomplete (i.e. at the end of a stream)
  // Figure out the correct doc to use by stepping until the tids are equal.
  while (!segment_header.low_delay &&
         SegmentHeader::CalcTidFromDoc(doc, *sub_gop_length, *sub_gop_start_poc)
         != tid) {
    doc++;
    if (doc > *sub_gop_end_poc) {
      *sub_gop_start_poc = *sub_gop_end_poc;
    }
  }

  if (tid == 0) {
    *sub_gop_end_poc =
      SegmentHeader::CalcPocFromDoc(doc, *sub_gop_length, *sub_gop_start_poc);
  }
  PicNum poc =
    SegmentHeader::CalcPocFromDoc(doc, *sub_gop_length, *sub_gop_start_poc);
  if (segment_header.low_delay) {
    poc = doc;
  }

  // Set High-level syntax parameters of the current picture
  PictureDecoder::PicNalHeader header;
  header.nal_unit_type = nal_unit_type;
  header.soc = soc;
  header.poc = poc;
  header.doc = doc;
  header.tid = tid;
  header.pic_qp = pic_qp_;
  header.highest_layer = tid == SegmentHeader::GetMaxTid(*sub_gop_length);
  header.deblock = deblock;
  header.allow_lic = allow_lic;
  return header;
}

void PictureDecoder::Init(const SegmentHeader &segment,
                          const PicNalHeader &header,
                          ReferencePictureLists &&ref_pic_list,
                          const PictureFormat &output_pic_format,
                          int64_t user_data) {
  assert(output_status_ == OutputStatus::kHasBeenOutput);
  pic_qp_ = header.pic_qp;
  output_format_ = output_pic_format;
  user_data_ = user_data;
  output_status_ = OutputStatus::kProcessing;
  ref_count = 0;
  pic_data_->SetNalType(header.nal_unit_type);
  pic_data_->SetSoc(header.soc);
  pic_data_->SetPoc(header.poc);
  pic_data_->SetDoc(header.doc);
  pic_data_->SetTid(header.tid);
  pic_data_->SetSubGopLength(segment.max_sub_gop_length);
  pic_data_->SetHighestLayer(header.highest_layer && !segment.low_delay);
  pic_data_->SetAdaptiveQp(segment.adaptive_qp);
  pic_data_->SetDeblock(header.deblock);
  pic_data_->SetBetaOffset(segment.beta_offset);
  pic_data_->SetTcOffset(segment.tc_offset);
  pic_data_->SetUseLocalIlluminationCompensation(header.allow_lic);
  *pic_data_->GetRefPicLists() = std::move(ref_pic_list);
}

bool PictureDecoder::Decode(const SegmentHeader &segment,
                            const SegmentHeader &prev_segment_header,
                            BitReader *bit_reader, bool post_process) {
  assert(output_status_ == OutputStatus::kProcessing);
  bool success = true;
  double lambda = 0;
  Qp qp(pic_qp_, pic_data_->GetChromaFormat(), pic_data_->GetBitdepth(),
        lambda);

  pic_data_->Init(segment, qp, true);
  discard_picture_ = false;
  row_diag_.clear();

  if (segment.row_restart) {
    // Version 3.0: validate the complete directory first, then decode each
    // CTU row as an independent bounded CABAC substream in the requested
    // (forward or reverse) scheduling order.
    bool rows_ok = false;
    try {
      rows_ok = DecodeRowRestart(segment, bit_reader, qp);
    } catch (...) {
      rows_ok = false;
    }
    if (!rows_ok) {
      // Hard rejection: no deblock/checksum/output/DPB publication of the
      // partial reconstruction.
      discard_picture_ = true;
      conforming_ = false;
      pic_data_->SetReferenceUsable(false);
      pic_data_->SetSpatialRowBand(false, 0, 0);
      return false;
    }
    if (pic_data_->GetDeblock()) {
      DeblockingFilter deblocker(pic_data_.get(), rec_pic_.get(),
                                 pic_data_->GetBetaOffset(),
                                 pic_data_->GetTcOffset());
      deblocker.DeblockPicture();
    }
  } else {
    std::unique_ptr<SyntaxReader> syntax_reader =
      SyntaxReader::Create(qp, pic_data_->GetPredictionType(), bit_reader);
    std::unique_ptr<CuDecoder> cu_decoder(
      new CuDecoder(simd_, rec_pic_.get(), pic_data_.get()));
    int num_ctus = pic_data_->GetNumberOfCtu();
    for (int rsaddr = 0; rsaddr < num_ctus; rsaddr++) {
      cu_decoder->DecodeCtu(rsaddr, syntax_reader.get());
    }
    if (pic_data_->GetDeblock()) {
      DeblockingFilter deblocker(pic_data_.get(), rec_pic_.get(),
                                 pic_data_->GetBetaOffset(),
                                 pic_data_->GetTcOffset());
      deblocker.DeblockPicture();
    }
    if (!syntax_reader->Finish()) {
      assert(0);
      success = false;
    }
  }
  if (pic_data_->GetTid() == 0 || !pic_data_->IsHighestLayer()) {
    rec_pic_->PadBorder();
  }
  if (pic_data_->GetNalType() == NalUnitType::kIntraAccessPicture &&
      prev_segment_header.open_gop) {
    GenerateAlternativeRecPic(segment, prev_segment_header);
  }
  pic_data_->GetRefPicLists()->ZeroOutReferences();
  if (post_process) {
    success &= Postprocess(segment, bit_reader);
  }
  return success;
}

bool PictureDecoder::DecodeRowRestart(const SegmentHeader &segment,
                                      BitReader *bit_reader,
                                      const Qp &base_qp) {
  const uint8_t *nal = bit_reader->GetBuffer();
  const size_t nal_size = bit_reader->GetLength();
  // DecodeHeader byte-aligns the reader at the end of the picture header.
  const size_t header_end = bit_reader->GetPosition();
  if (!nal || header_end + 8 > nal_size) {
    return false;  // short / missing directory
  }
  if (nal[header_end + 0] != 'X' || nal[header_end + 1] != 'R' ||
      nal[header_end + 2] != 'S' || nal[header_end + 3] != 'T') {
    return false;  // missing XRST directory
  }
  auto read_u16 = [&](size_t off) -> uint32_t {
    return (static_cast<uint32_t>(nal[off]) << 8) | nal[off + 1];
  };
  auto read_u32 = [&](size_t off) -> uint32_t {
    return (static_cast<uint32_t>(nal[off + 0]) << 24) |
      (static_cast<uint32_t>(nal[off + 1]) << 16) |
      (static_cast<uint32_t>(nal[off + 2]) << 8) |
      static_cast<uint32_t>(nal[off + 3]);
  };
  const int row_count = static_cast<int>(read_u16(header_end + 4));
  const uint32_t reserved_zero = read_u16(header_end + 6);
  const int expected_rows = pic_data_->GetCtuNumY();
  if (reserved_zero != 0 || row_count <= 0 || row_count != expected_rows) {
    return false;
  }
  const size_t directory_bytes =
    static_cast<size_t>(row_count) * static_cast<size_t>(4);
  // Overflow safe directory extent check.
  if (directory_bytes > nal_size ||
      header_end + 8 > nal_size - directory_bytes) {
    return false;  // short directory
  }
  const size_t lengths_start = header_end + 8;
  const size_t rows_start = lengths_start + directory_bytes;

  std::vector<uint32_t> lengths(row_count);
  uint64_t total_length = 0;
  for (int row = 0; row < row_count; row++) {
    uint32_t len = read_u32(lengths_start + static_cast<size_t>(row) * 4);
    if (len == 0) {
      return false;  // zero length row
    }
    total_length += len;
    if (total_length > static_cast<uint64_t>(nal_size)) {
      return false;  // cumulative lengths already out of bounds
    }
    lengths[row] = len;
  }
  if (total_length > static_cast<uint64_t>(nal_size - rows_start)) {
    return false;  // rows run past the picture NAL
  }
  size_t rows_end = rows_start + static_cast<size_t>(total_length);

  // Determine the exact picture checksum footprint so row lengths are bound
  // against the NAL excluding the trailing image-level checksum.
  const bool checksum_present =
    pic_data_->GetTid() == 0 ||
    segment.checksum_mode == Checksum::Mode::kMaxRobust;
  size_t checksum_len = 0;
  if (checksum_present) {
    const int components =
      util::GetNumComponents(pic_data_->GetChromaFormat());
    const bool md5 =
      !Restrictions::Get().disable_high_level_default_checksum_method;
    if (md5) {
      checksum_len =
        segment.checksum_mode == Checksum::Mode::kMaxRobust ?
        static_cast<size_t>(16 * components) : static_cast<size_t>(16);
    } else {
      checksum_len =
        segment.checksum_mode == Checksum::Mode::kMaxRobust ?
        static_cast<size_t>(2 * components) : static_cast<size_t>(2);
    }
    if (checksum_len > nal_size || rows_end > nal_size - checksum_len ||
        rows_end + checksum_len != nal_size) {
      return false;  // rows overlap checksum or trailing bytes are malformed
    }
  } else if (rows_end != nal_size) {
    return false;
  }

  // Build contiguous byte offsets from the stored lengths.
  std::vector<size_t> offsets(row_count);
  size_t offset = rows_start;
  for (int row = 0; row < row_count; row++) {
    offsets[row] = offset;
    offset += lengths[row];
  }

  std::vector<int> schedule;
  schedule.reserve(row_count);
  if (row_decode_order_ == 0) {
    for (int row = 0; row < row_count; row++) {
      schedule.push_back(row);
    }
  } else {
    for (int row = row_count - 1; row >= 0; row--) {
      schedule.push_back(row);
    }
  }

  const int ctu_num_x = pic_data_->GetCtuNumX();
  for (int row : schedule) {
    const int row_top = row * constants::kCtuSize;
    const int row_bottom = row_top + constants::kCtuSize;
    pic_data_->SetSpatialRowBand(true, row_top, row_bottom);

    // Restricted reader: it can only see this row's own bytes.
    BitReader row_reader(nal + offsets[row], lengths[row]);
    row_reader.SetSafeBounds(true);
    std::unique_ptr<SyntaxReader> syntax_reader =
      SyntaxReader::Create(base_qp, pic_data_->GetPredictionType(),
                           &row_reader);

    RowDiag diag;
    diag.row = row;
    diag.rsaddr_first = row * ctu_num_x;
    diag.pic_qp = pic_qp_;
    diag.stream_length = lengths[row];
    diag.context_fingerprint = syntax_reader->DiagnosticsFingerprint();
    diag.secondary_tree = pic_data_->HasSecondaryCuTree() ? 1 : 0;
    if (CodingUnit *first_ctu =
          pic_data_->GetCtu(CuTree::Primary, diag.rsaddr_first)) {
      // Band is active and the first CTU has no available left/above QP
      // predictor, so this must equal the picture initial QP.
      diag.predicted_qp_first = first_ctu->GetPredictedQp();
    }

    std::unique_ptr<CuDecoder> cu_decoder(
      new CuDecoder(simd_, rec_pic_.get(), pic_data_.get()));
    for (int cx = 0; cx < ctu_num_x; cx++) {
      int rsaddr = row * ctu_num_x + cx;
      cu_decoder->DecodeCtu(rsaddr, syntax_reader.get());
    }
    // Every row must carry a legal CABAC termination of its own.
    if (!syntax_reader->Finish()) {
      return false;
    }
    diag.consumed_bytes = row_reader.GetPosition();
    if (diag.consumed_bytes != lengths[row]) {
      return false;
    }
    row_diag_.push_back(diag);
  }
  pic_data_->SetSpatialRowBand(false, 0, 0);

  // Position the outer reader at the image-level checksum.
  bit_reader->SeekToByte(rows_end);
  return true;
}

bool PictureDecoder::Postprocess(const SegmentHeader &segment,
                                 BitReader *bit_reader) {
  const int pic_tid = pic_data_->GetTid();
  bool success = true;
  if (pic_tid == 0 || segment.checksum_mode == Checksum::Mode::kMaxRobust) {
    success &= ValidateChecksum(segment, bit_reader, segment.checksum_mode);
  } else {
    pic_hash_.clear();
  }
  output_resampler_.ConvertTo(*rec_pic_, output_format_, &output_pic_bytes_);
  return success;
}

std::shared_ptr<YuvPicture>
PictureDecoder::GetAlternativeRecPic(const PictureFormat &pic_fmt,
                                     int crop_width, int crop_height) const {
  if (alt_rec_pic_) {
    return alt_rec_pic_;
  }
  auto alt_rec_pic =
    std::make_shared<YuvPicture>(pic_fmt.chroma_format, pic_fmt.width,
                                 pic_fmt.height, pic_fmt.bitdepth, true,
                                 crop_width, crop_height);
  // TODO(PH) Revise const_cast by making this function a pure getter?
  // In the future alternate pictures should probably be created
  // beforehand in a separate thread as this can be quite time consuming
  const_cast<PictureDecoder*>(this)->alt_rec_pic_ = alt_rec_pic;
  return alt_rec_pic;
}

void
PictureDecoder::GenerateAlternativeRecPic(
  const SegmentHeader &segment, const SegmentHeader &prev_segment_header)
  const {
  if (prev_segment_header.GetInternalPicFormat().chroma_format ==
      ChromaFormat::kUndefined ||
      prev_segment_header.GetInternalPicFormat().width <= 0 ||
      prev_segment_header.GetInternalPicFormat().height <= 0 ||
      (prev_segment_header.GetInternalPicFormat().chroma_format ==
       segment.GetInternalPicFormat().chroma_format &&
       prev_segment_header.GetInternalPicFormat().width ==
       segment.GetInternalPicFormat().width &&
       prev_segment_header.GetInternalPicFormat().height ==
       segment.GetInternalPicFormat().height &&
       prev_segment_header.GetInternalPicFormat().bitdepth ==
       segment.GetInternalPicFormat().bitdepth)) {
    return;
  }
  auto pic_fmt = prev_segment_header.GetInternalPicFormat();
  int crop_width = prev_segment_header.GetCropWidth();
  int crop_height = prev_segment_header.GetCropHeight();
  std::shared_ptr<YuvPicture> alt_rec_pic;
  if (alt_rec_pic_) {
    alt_rec_pic = alt_rec_pic_;
  } else {
    alt_rec_pic =
      std::make_shared<YuvPicture>(pic_fmt.chroma_format, pic_fmt.width,
                                   pic_fmt.height, pic_fmt.bitdepth, true,
                                   crop_width, crop_height);
    const_cast<PictureDecoder*>(this)->alt_rec_pic_ = alt_rec_pic;
  }
  for (int c = 0; c < util::GetNumComponents(pic_fmt.chroma_format); c++) {
    YuvComponent comp = YuvComponent(c);
    uint8_t* dst =
      reinterpret_cast<uint8_t*>(alt_rec_pic->GetSamplePtr(comp, 0, 0));
    if (rec_pic_->GetChromaFormat() == ChromaFormat::kMonochrome &&
        comp != YuvComponent::kY) {
      std::memset(dst, 1 << (alt_rec_pic->GetBitdepth() - 1),
                  alt_rec_pic->GetStride(comp) *
                  alt_rec_pic->GetHeight(comp) * sizeof(Sample));
      continue;
    }
    uint8_t* src =
      reinterpret_cast<uint8_t*>(rec_pic_->GetSamplePtr(comp, 0, 0));
    resample::Resample<Sample, Sample>
      (dst, alt_rec_pic->GetWidth(comp), alt_rec_pic->GetHeight(comp),
       alt_rec_pic->GetStride(comp), alt_rec_pic->GetBitdepth(),
       src, rec_pic_->GetWidth(comp), rec_pic_->GetHeight(comp),
       rec_pic_->GetStride(comp), rec_pic_->GetBitdepth());
  }
  alt_rec_pic->PadBorder();
}

bool PictureDecoder::ValidateChecksum(const SegmentHeader &segment,
                                      BitReader *bit_reader,
                                      Checksum::Mode checksum_mode) {
  Checksum::Method checksum_method =
    Restrictions::Get().disable_high_level_default_checksum_method ?
    Checksum::kFallbackMethod : Checksum::kDefaultMethod;
  Checksum checksum(checksum_method, checksum_mode);
  checksum.HashPicture(*rec_pic_);
  pic_hash_ = checksum.GetHash();
  if (segment.major_version <= 1) {
    size_t checksum_len = bit_reader->ReadByte();
    assert(checksum_len == pic_hash_.size());
    ((void)(checksum_len));   // not used
  }
  std::vector<uint8_t> expected_hash;
  expected_hash.resize(pic_hash_.size());
  bit_reader->ReadBytes(&expected_hash[0], pic_hash_.size());
  return pic_hash_ == expected_hash;
}

}   // namespace xvc
