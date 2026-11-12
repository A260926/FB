//! Picture order count derivation (ITU-T H.264 section 8.2.1).
//!
//! [`PocReader`] consumes access units made of complete NAL units (Annex B
//! start codes already removed; the NAL header and emulation-prevention bytes
//! are still present) and derives the picture order count of each primary
//! coded picture.
//!
//! This module only derives the picture order count. It does not generate
//! timestamps, decode pixels, or maintain a reference picture store.
//!
//! Access unit boundaries are the caller's responsibility: the NALs handed to
//! [`PocReader::push_access_unit`] must already be grouped into access units in
//! decoding order.

use crate::nal::pps::{PicParameterSet, PicParamSetId};
use crate::nal::slice::{
    DecRefPicMarking, Field, FieldPic, MemoryManagementControlOperation, PicOrderCountLsb,
    SliceHeader,
};
use crate::nal::sps::{PicOrderCntType, SeqParameterSet};
use crate::nal::{Nal, NalHeader, RefNal, UnitType};
use crate::Context;
use std::fmt;

/// Picture order counts derived for one primary coded picture.
///
/// For a coded frame the two field counts correspond to its top and bottom
/// fields (the bottom one is only present when
/// `bottom_field_pic_order_in_frame_present_flag` is set); for a coded field
/// only the field actually present is set.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PictureOrder {
    /// `frame_num` syntax element of the picture.
    pub frame_num: u32,
    /// ID of the SPS referenced by the picture's PPS.
    pub sps_id: u8,
    /// ID of the PPS referenced by the picture's slices.
    pub pps_id: u8,
    /// `TopFieldOrderCnt`, when present for this picture.
    pub top_field_order_cnt: Option<i64>,
    /// `BottomFieldOrderCnt`, when present for this picture.
    pub bottom_field_order_cnt: Option<i64>,
}

impl PictureOrder {
    /// Minimum of the field order counts that are actually present.
    ///
    /// A field that does not exist for the picture is `None` and is never
    /// treated as zero.
    pub fn min_field_order_cnt(&self) -> i64 {
        match (self.top_field_order_cnt, self.bottom_field_order_cnt) {
            (Some(top), Some(bottom)) => top.min(bottom),
            (Some(top), None) => top,
            (None, Some(bottom)) => bottom,
            // a primary coded picture always carries at least one field
            (None, None) => unreachable!("picture order count without any field"),
        }
    }
}

/// Errors that can occur while deriving picture order counts.
#[derive(Debug)]
#[non_exhaustive]
pub enum PocError {
    /// A NAL unit was too short to contain a NAL header.
    EmptyNal,
    /// A NAL header had its `forbidden_zero_bit` set.
    InvalidNalHeader,
    /// Parsing an SPS/PPS/slice header failed (truncated header or bad syntax).
    MalformedNal(String),
    /// A slice referenced a PPS that has not been declared.
    MissingPps(u8),
    /// A PPS referenced an SPS that has not been declared.
    MissingSps(u8),
    /// SVC/MVC extensions, data partitions and redundant pictures are not supported.
    UnsupportedNal(&'static str),
    /// Multiple VCL NALs in the access unit did not belong to the same primary
    /// coded picture (spec 7.4.1.2.4), or MMCO 5 appeared more than once.
    InconsistentAccessUnit(&'static str),
    /// The first primary coded picture (or the first one after
    /// [`PocReader::reset`]) was not an IDR picture.
    FirstPictureNotIdr,
    /// Arithmetic overflow while deriving a picture order count.
    ArithmeticOverflow,
}

impl fmt::Display for PocError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PocError::EmptyNal => f.write_str("empty NAL unit"),
            PocError::InvalidNalHeader => f.write_str("NAL header has forbidden_zero_bit set"),
            PocError::MalformedNal(msg) => write!(f, "malformed NAL unit: {msg}"),
            PocError::MissingPps(id) => write!(f, "reference to undeclared PPS {id}"),
            PocError::MissingSps(id) => write!(f, "reference to undeclared SPS {id}"),
            PocError::UnsupportedNal(what) => write!(f, "unsupported NAL content: {what}"),
            PocError::InconsistentAccessUnit(what) => {
                write!(f, "access unit mixes different primary coded pictures: {what}")
            }
            PocError::FirstPictureNotIdr => {
                f.write_str("the first primary coded picture must be an IDR picture")
            }
            PocError::ArithmeticOverflow => f.write_str("picture order count arithmetic overflow"),
        }
    }
}

impl std::error::Error for PocError {}

/// State carried between primary coded pictures for one SPS (8.2.1.1).
#[derive(Clone, Debug, Default)]
struct PocHistory {
    /// Whether any primary coded picture has established this history.
    started: bool,
    /// `PrevPicOrderCntMsb` / `PrevPicOrderCntLsb` (type 0).
    prev_poc_msb: i64,
    prev_poc_lsb: i64,
    /// `FrameNumOffset` and the previous reference `frame_num`.
    frame_num_offset: i64,
    prev_frame_num: u32,
    /// `TopFieldOrderCnt` of the most recent top field, used when the bottom
    /// field of a complementary field pair arrives in a later access unit
    /// (type 1, equation 8-8).
    last_top_field_poc: Option<i64>,
}

/// One parsed VCL NAL plus the parameter-set ids resolved while parsing it.
struct ParsedVcl {
    header: NalHeader,
    slice: SliceHeader,
    pps_id: u8,
    sps_id: u8,
}

/// Derives picture order counts from a stream of access units.
///
/// Construct with [`PocReader::new`], feed one access unit at a time with
/// [`PocReader::push_access_unit`], and receive [`PictureOrder`] for access
/// units containing a primary coded picture (other access units yield `None`).
#[derive(Debug, Default)]
pub struct PocReader {
    /// Declared parameter sets; retained across [`reset`](Self::reset).
    ctx: Context,
    /// Per-SPS POC history, keyed by SPS id; discarded by [`reset`](Self::reset).
    history: Vec<Option<PocHistory>>,
}

impl PocReader {
    /// Creates a reader with no declared parameter sets and no POC history.
    pub fn new() -> Self {
        Self::default()
    }

    /// Clears all POC/frame_num history and requires the next primary coded
    /// picture to be an IDR picture. Already-declared SPS/PPS parameter sets
    /// are retained.
    pub fn reset(&mut self) {
        self.history.clear();
    }

    /// Feeds one access unit, given as the complete NAL units it contains
    /// (without Annex B start codes), in decoding order within the unit.
    ///
    /// Returns `None` for access units containing only non-VCL NAL units
    /// (SPS/PPS, AUD, SEI and friends), and [`PictureOrder`] exactly once for
    /// an access unit containing a primary coded picture. On error no
    /// parameter set declaration and no history update from this access unit
    /// takes effect, so the next valid input behaves as if the failed access
    /// unit had never been supplied.
    ///
    /// The input slices may be reused immediately after this call returns; no
    /// borrow into them is retained.
    pub fn push_access_unit(&mut self, nals: &[&[u8]]) -> Result<Option<PictureOrder>, PocError> {
        // Scratch parameter-set context: parameter sets of a failed access unit
        // are not committed.
        let mut scratch = self.ctx.clone();
        let mut vcl: Vec<ParsedVcl> = Vec::new();
        let mut mmco5_seen = false;

        for &nal_bytes in nals {
            if nal_bytes.is_empty() {
                return Err(PocError::EmptyNal);
            }
            let nal = RefNal::new(nal_bytes, &[], true);
            let header = nal.header().map_err(|_| PocError::InvalidNalHeader)?;
            match header.nal_unit_type() {
                UnitType::SeqParameterSet => {
                    let sps = SeqParameterSet::from_bits(nal.rbsp_bits())
                        .map_err(|e| PocError::MalformedNal(format!("SPS: {e:?}")))?;
                    scratch.put_seq_param_set(sps);
                }
                UnitType::PicParameterSet => {
                    let pps = PicParameterSet::from_bits(&scratch, nal.rbsp_bits())
                        .map_err(|e| PocError::MalformedNal(format!("PPS: {e:?}")))?;
                    scratch.put_pic_param_set(pps);
                }
                // Non-VCL NALs without POC-relevant state.
                UnitType::AccessUnitDelimiter
                | UnitType::SEI
                | UnitType::EndOfSeq
                | UnitType::EndOfStream
                | UnitType::FillerData => {}
                UnitType::SliceDataPartitionALayer
                | UnitType::SliceDataPartitionBLayer
                | UnitType::SliceDataPartitionCLayer => {
                    return Err(PocError::UnsupportedNal("data partitioned slices"));
                }
                UnitType::SliceLayerWithoutPartitioningAux => {
                    return Err(PocError::UnsupportedNal("auxiliary coded picture"));
                }
                UnitType::SliceExtension | UnitType::SliceExtensionViewComponent => {
                    return Err(PocError::UnsupportedNal("SVC/MVC slice extension"));
                }
                UnitType::PrefixNALUnit
                | UnitType::SubsetSeqParameterSet
                | UnitType::SeqParameterSetExtension
                | UnitType::DepthParameterSet => {
                    return Err(PocError::UnsupportedNal("SVC/MVC extension NAL unit"));
                }
                UnitType::SliceLayerWithoutPartitioningNonIdr
                | UnitType::SliceLayerWithoutPartitioningIdr => {
                    let (slice, sps, pps) =
                        SliceHeader::from_bits(&scratch, &mut nal.rbsp_bits(), header, None)
                            .map_err(map_slice_error)?;
                    if slice.redundant_pic_cnt.unwrap_or(0) != 0 {
                        return Err(PocError::UnsupportedNal("redundant coded picture"));
                    }
                    if let Some(DecRefPicMarking::Adaptive(ops)) = &slice.dec_ref_pic_marking {
                        for op in ops {
                            if matches!(
                                op,
                                MemoryManagementControlOperation::AllRefPicturesUnused
                            ) {
                                if mmco5_seen {
                                    return Err(PocError::InconsistentAccessUnit(
                                        "MMCO 5 may only be executed once per picture",
                                    ));
                                }
                                mmco5_seen = true;
                            }
                        }
                    }
                    vcl.push(ParsedVcl {
                        header,
                        pps_id: pps.pic_parameter_set_id.id(),
                        sps_id: sps.seq_parameter_set_id.id(),
                        slice,
                    });
                }
                UnitType::Unspecified(_) | UnitType::Reserved(_) => {
                    return Err(PocError::UnsupportedNal("unspecified/reserved NAL unit type"));
                }
            }
        }

        let Some(first) = vcl.first() else {
            // Non-VCL access unit: commit its parameter sets, no POC output.
            self.ctx = scratch;
            return Ok(None);
        };

        // 7.4.1.2.4: every VCL NAL of an access unit belongs to one picture.
        check_picture_identity(&vcl)?;

        let pps = scratch
            .pps_by_id(PicParamSetId::from_u32(u32::from(first.pps_id)).unwrap())
            .expect("PPS presence validated while parsing the slice");
        let sps = scratch
            .sps_by_id(pps.seq_parameter_set_id)
            .expect("SPS presence validated while parsing the PPS");

        let slot = usize::from(first.sps_id);
        if self.history.len() <= slot {
            self.history.resize_with(slot + 1, || None);
        }
        let is_idr = matches!(
            first.header.nal_unit_type(),
            UnitType::SliceLayerWithoutPartitioningIdr
        );
        let is_reference = first.header.nal_ref_idc() != 0;

        // An IDR activates fresh POC history for its SPS: an SPS that changed
        // POC-related configuration only establishes a new history here, never
        // when the SPS NAL itself is declared. The fresh history is built in
        // the local copy so a failed derivation commits nothing.
        let mut history = if is_idr {
            PocHistory::default()
        } else {
            self.history[slot].clone().unwrap_or_default()
        };

        let result = derive_picture(
            sps,
            pps.bottom_field_pic_order_in_frame_present_flag,
            first.pps_id,
            &first.slice,
            is_idr,
            is_reference,
            &mut history,
        )?;

        // Commit everything only after full success.
        self.history[slot] = Some(history);
        self.ctx = scratch;
        Ok(Some(result))
    }
}

fn map_slice_error(e: crate::nal::slice::SliceHeaderError) -> PocError {
    use crate::nal::slice::SliceHeaderError;
    match e {
        SliceHeaderError::UndefinedPicParamSetId(id) => PocError::MissingPps(id.id()),
        SliceHeaderError::UndefinedSeqParamSetId(id) => PocError::MissingSps(id.id()),
        SliceHeaderError::UnsupportedSyntax(what) => PocError::UnsupportedNal(what),
        other => PocError::MalformedNal(format!("slice header: {other:?}")),
    }
}

fn check_picture_identity(vcl: &[ParsedVcl]) -> Result<(), PocError> {
    let first = &vcl[0];
    let first_idr = matches!(
        first.header.nal_unit_type(),
        UnitType::SliceLayerWithoutPartitioningIdr
    );
    let first_ref = first.header.nal_ref_idc() != 0;
    for cur in &vcl[1..] {
        let idr = matches!(
            cur.header.nal_unit_type(),
            UnitType::SliceLayerWithoutPartitioningIdr
        );
        if idr != first_idr {
            return Err(PocError::InconsistentAccessUnit("IDR identification differs"));
        }
        if (cur.header.nal_ref_idc() != 0) != first_ref {
            return Err(PocError::InconsistentAccessUnit(
                "reference/non-reference attribute differs",
            ));
        }
        if cur.pps_id != first.pps_id {
            return Err(PocError::InconsistentAccessUnit("PPS id differs"));
        }
        if cur.slice.frame_num != first.slice.frame_num {
            return Err(PocError::InconsistentAccessUnit("frame_num differs"));
        }
        if cur.slice.field_pic != first.slice.field_pic {
            return Err(PocError::InconsistentAccessUnit("field identity differs"));
        }
        if cur.slice.idr_pic_id != first.slice.idr_pic_id {
            return Err(PocError::InconsistentAccessUnit("idr_pic_id differs"));
        }
        if !pic_order_syntax_eq(
            cur.slice.pic_order_cnt_lsb.as_ref(),
            first.slice.pic_order_cnt_lsb.as_ref(),
        ) {
            return Err(PocError::InconsistentAccessUnit(
                "picture order count syntax differs",
            ));
        }
        // first_mb_in_slice intentionally not compared.
    }
    Ok(())
}

/// Structural comparison of the POC syntax carried by two slices of the same
/// picture (`PicOrderCountLsb` does not implement `PartialEq`).
fn pic_order_syntax_eq(a: Option<&PicOrderCountLsb>, b: Option<&PicOrderCountLsb>) -> bool {
    match (a, b) {
        (None, None) => true,
        (Some(PicOrderCountLsb::Frame(x)), Some(PicOrderCountLsb::Frame(y))) => x == y,
        (
            Some(PicOrderCountLsb::FieldsAbsolute {
                pic_order_cnt_lsb: l0,
                delta_pic_order_cnt_bottom: d0,
            }),
            Some(PicOrderCountLsb::FieldsAbsolute {
                pic_order_cnt_lsb: l1,
                delta_pic_order_cnt_bottom: d1,
            }),
        ) => l0 == l1 && d0 == d1,
        (Some(PicOrderCountLsb::FieldsDelta(x)), Some(PicOrderCountLsb::FieldsDelta(y))) => {
            x == y
        }
        _ => false,
    }
}

fn has_mmco5(slice: &SliceHeader) -> bool {
    matches!(&slice.dec_ref_pic_marking, Some(DecRefPicMarking::Adaptive(ops))
        if ops.iter().any(|op| matches!(op, MemoryManagementControlOperation::AllRefPicturesUnused)))
}

/// Normalised frame numbering shared by all three POC types: the frame_num
/// wraparound and the `FrameNumOffset` accumulation are derived once here
/// rather than separately in each POC type. Returns
/// `FrameNumOffset + frame_num`, from which each type builds `absFrameNum`.
fn normalise_frame_num(
    sps: &SeqParameterSet,
    slice: &SliceHeader,
    is_idr: bool,
    is_reference: bool,
    is_bottom_field: bool,
    mmco5: bool,
    h: &mut PocHistory,
) -> Result<i64, PocError> {
    let max_frame_num: i64 = 1i64
        .checked_shl(u32::from(sps.log2_max_frame_num()))
        .ok_or(PocError::ArithmeticOverflow)?;
    let frame_num = i64::from(slice.frame_num);

    if is_idr || mmco5 {
        h.frame_num_offset = 0;
        h.prev_frame_num = 0;
    } else if is_reference && !is_bottom_field && frame_num < i64::from(h.prev_frame_num) {
        // frame_num wrapped around MaxFrameNum. Non-reference pictures and
        // bottom fields (which share their top field's frame_num) do not move
        // FrameNumOffset.
        h.frame_num_offset = h
            .frame_num_offset
            .checked_add(max_frame_num)
            .ok_or(PocError::ArithmeticOverflow)?;
    }

    h.frame_num_offset
        .checked_add(frame_num)
        .ok_or(PocError::ArithmeticOverflow)
}

#[allow(clippy::too_many_arguments)]
fn derive_picture(
    sps: &SeqParameterSet,
    bottom_field_pic_order_in_frame_present_flag: bool,
    pps_id: u8,
    slice: &SliceHeader,
    is_idr: bool,
    is_reference: bool,
    h: &mut PocHistory,
) -> Result<PictureOrder, PocError> {
    // The first primary coded picture, and the first one after an explicit
    // reset, must be an IDR.
    if !h.started && !is_idr {
        return Err(PocError::FirstPictureNotIdr);
    }

    let is_bottom_field = matches!(slice.field_pic, FieldPic::Field(Field::Bottom));
    let mmco5 = has_mmco5(slice);

    // MMCO 5 restarts every inherited counter before this picture is normalised.
    if mmco5 {
        h.prev_poc_msb = 0;
        h.prev_poc_lsb = 0;
        h.last_top_field_poc = None;
    }

    let frame_num_base = normalise_frame_num(
        sps,
        slice,
        is_idr,
        is_reference,
        is_bottom_field,
        mmco5,
        h,
    )?;

    let (top, bottom) = match &sps.pic_order_cnt {
        PicOrderCntType::TypeZero {
            log2_max_pic_order_cnt_lsb_minus4,
        } => derive_type0(
            bottom_field_pic_order_in_frame_present_flag,
            slice,
            is_idr,
            is_reference,
            is_bottom_field,
            mmco5,
            h,
            *log2_max_pic_order_cnt_lsb_minus4,
        )?,
        PicOrderCntType::TypeOne { .. } => derive_type1(
            bottom_field_pic_order_in_frame_present_flag,
            slice,
            is_idr,
            is_reference,
            frame_num_base,
            sps,
            h,
        )?,
        PicOrderCntType::TypeTwo => derive_type2(slice, is_idr, is_reference, frame_num_base)?,
    };

    // State inherited by the following picture.
    if is_idr || mmco5 {
        h.frame_num_offset = 0;
        h.prev_frame_num = 0;
    } else if is_reference && !is_bottom_field {
        h.prev_frame_num = u32::from(slice.frame_num);
    }
    h.started = true;

    Ok(PictureOrder {
        frame_num: u32::from(slice.frame_num),
        sps_id: sps.seq_parameter_set_id.id(),
        pps_id,
        top_field_order_cnt: top,
        bottom_field_order_cnt: bottom,
    })
}

// ---- pic_order_cnt_type == 0 (spec 8.2.1.1) -------------------------------

#[allow(clippy::too_many_arguments)]
fn derive_type0(
    bottom_field_pic_order_in_frame_present_flag: bool,
    slice: &SliceHeader,
    is_idr: bool,
    is_reference: bool,
    is_bottom_field: bool,
    mmco5: bool,
    h: &mut PocHistory,
    log2_max_pic_order_cnt_lsb_minus4: u8,
) -> Result<(Option<i64>, Option<i64>), PocError> {
    let max_poc_lsb: i64 = 1i64
        .checked_shl(u32::from(log2_max_pic_order_cnt_lsb_minus4) + 4)
        .ok_or(PocError::ArithmeticOverflow)?;

    let (poc_lsb, delta_bottom) = match slice
        .pic_order_cnt_lsb
        .as_ref()
        .expect("type 0 slice carries pic_order_cnt_lsb")
    {
        PicOrderCountLsb::Frame(lsb) => (i64::from(*lsb), 0),
        PicOrderCountLsb::FieldsAbsolute {
            pic_order_cnt_lsb,
            delta_pic_order_cnt_bottom,
        } => (
            i64::from(*pic_order_cnt_lsb),
            i64::from(*delta_pic_order_cnt_bottom),
        ),
        PicOrderCountLsb::FieldsDelta(_) => {
            return Err(PocError::MalformedNal(
                "type 1 POC delta syntax in a type 0 SPS".into(),
            ));
        }
    };

    let poc_msb = if is_idr || mmco5 {
        // IDR, or a history reset by MMCO 5: the MSB counter starts at zero.
        0
    } else if is_reference {
        // Half-modulus boundary per 8.2.1.1: a difference >= MaxPicOrderCntLsb/2
        // with a smaller lsb wraps upward, a strictly larger difference with a
        // larger lsb wraps downward.
        if poc_lsb < h.prev_poc_lsb && h.prev_poc_lsb - poc_lsb >= max_poc_lsb / 2 {
            h.prev_poc_msb
                .checked_add(max_poc_lsb)
                .ok_or(PocError::ArithmeticOverflow)?
        } else if poc_lsb > h.prev_poc_lsb && poc_lsb - h.prev_poc_lsb > max_poc_lsb / 2 {
            h.prev_poc_msb
                .checked_sub(max_poc_lsb)
                .ok_or(PocError::ArithmeticOverflow)?
        } else {
            h.prev_poc_msb
        }
    } else {
        // A non-reference picture reuses the MSB history without updating it.
        h.prev_poc_msb
    };

    let top = poc_msb
        .checked_add(poc_lsb)
        .ok_or(PocError::ArithmeticOverflow)?;

    let (top_cnt, bottom_cnt) = match &slice.field_pic {
        FieldPic::Frame => {
            let bottom = if bottom_field_pic_order_in_frame_present_flag {
                Some(
                    top.checked_add(delta_bottom)
                        .ok_or(PocError::ArithmeticOverflow)?,
                )
            } else {
                None
            };
            (Some(top), bottom)
        }
        FieldPic::Field(Field::Top) => (Some(top), None),
        FieldPic::Field(Field::Bottom) => {
            // 8.2.1.1: a bottom field shares the top field's
            // PicOrderCntMsb/pic_order_cnt_lsb (bottom fields do not move the
            // PrevPicOrderCnt* history), so its count is computed directly.
            (None, Some(top))
        }
    };

    // Only reference frames and reference top fields become the
    // PrevPicOrderCnt* history; a non-reference B picture or a reference bottom
    // field must not overwrite it.
    if is_reference && !is_bottom_field {
        h.prev_poc_msb = poc_msb;
        h.prev_poc_lsb = poc_lsb;
    }
    Ok((top_cnt, bottom_cnt))
}

// ---- pic_order_cnt_type == 1 (spec 8.2.1.2) -------------------------------

#[allow(clippy::too_many_arguments)]
fn derive_type1(
    bottom_field_pic_order_in_frame_present_flag: bool,
    slice: &SliceHeader,
    is_idr: bool,
    is_reference: bool,
    frame_num_base: i64,
    sps: &SeqParameterSet,
    h: &mut PocHistory,
) -> Result<(Option<i64>, Option<i64>), PocError> {
    let (
        delta_pic_order_always_zero_flag,
        offset_for_non_ref_pic,
        offset_for_top_to_bottom_field,
        offsets_for_ref_frame,
    ) = match &sps.pic_order_cnt {
        PicOrderCntType::TypeOne {
            delta_pic_order_always_zero_flag,
            offset_for_non_ref_pic,
            offset_for_top_to_bottom_field,
            offsets_for_ref_frame,
        } => (
            *delta_pic_order_always_zero_flag,
            i64::from(*offset_for_non_ref_pic),
            i64::from(*offset_for_top_to_bottom_field),
            offsets_for_ref_frame
                .iter()
                .map(|&v| i64::from(v))
                .collect::<Vec<_>>(),
        ),
        _ => unreachable!("checked by caller"),
    };
    let cycle_len = offsets_for_ref_frame.len() as i64;

    let (mut delta0, delta1) = match slice
        .pic_order_cnt_lsb
        .as_ref()
        .expect("type 1 slice carries POC deltas")
    {
        PicOrderCountLsb::FieldsDelta(d) => (i64::from(d[0]), i64::from(d[1])),
        _ => {
            return Err(PocError::MalformedNal(
                "type 0 POC syntax in a type 1 SPS".into(),
            ))
        }
    };
    if delta_pic_order_always_zero_flag {
        // delta_pic_order_cnt[0] is absent from the bitstream; its value is zero.
        delta0 = 0;
    }

    // 8.2.1.2 / JM: absFrameNum is FrameNumOffset + frame_num, decremented by
    // one for a non-reference picture, and then doubled for field coding. The
    // non-reference adjustment only applies when a reference cycle is
    // declared; with an empty cycle conformance requires absFrameNum <= 0.
    let field_coding = matches!(slice.field_pic, FieldPic::Field(_));
    let mut abs_frame_num = frame_num_base;
    if !is_reference && cycle_len > 0 {
        abs_frame_num = abs_frame_num
            .checked_sub(1)
            .ok_or(PocError::ArithmeticOverflow)?;
    }
    if field_coding {
        abs_frame_num = abs_frame_num
            .checked_mul(2)
            .ok_or(PocError::ArithmeticOverflow)?;
    }

    // Whole-cycle count plus within-cycle accumulation (equations 8-6). After
    // MMCO 5 the shared frame_num normalisation restarted FrameNumOffset at
    // zero, so the fresh closed-form result matches a reset cycle counter.
    let mut expected_poc = 0i64;
    if cycle_len > 0 && abs_frame_num > 0 {
        let cycle_cnt = (abs_frame_num - 1) / cycle_len;
        let within = ((abs_frame_num - 1) % cycle_len) as usize;
        let delta_per_cycle: i64 =
            offsets_for_ref_frame
                .iter()
                .try_fold(0i64, |acc, &v| {
                    acc.checked_add(v).ok_or(PocError::ArithmeticOverflow)
                })?;
        expected_poc = cycle_cnt
            .checked_mul(delta_per_cycle)
            .ok_or(PocError::ArithmeticOverflow)?;
        for &offset in &offsets_for_ref_frame[..=within] {
            expected_poc = expected_poc
                .checked_add(offset)
                .ok_or(PocError::ArithmeticOverflow)?;
        }
    }
    if !is_reference && cycle_len > 0 {
        expected_poc = expected_poc
            .checked_add(offset_for_non_ref_pic)
            .ok_or(PocError::ArithmeticOverflow)?;
    }

    let (top_cnt, bottom_cnt) = if is_idr {
        // IDR: all expected counts and deltas are implicitly zero.
        match slice.field_pic {
            FieldPic::Frame | FieldPic::Field(Field::Top) => (Some(0), None),
            FieldPic::Field(Field::Bottom) => (None, Some(0)),
        }
    } else {
        match &slice.field_pic {
            FieldPic::Frame => {
                let top = expected_poc
                    .checked_add(delta0)
                    .ok_or(PocError::ArithmeticOverflow)?;
                let bottom = if bottom_field_pic_order_in_frame_present_flag {
                    Some(
                        top.checked_add(offset_for_top_to_bottom_field)
                            .and_then(|v| v.checked_add(delta1))
                            .ok_or(PocError::ArithmeticOverflow)?,
                    )
                } else {
                    None
                };
                (Some(top), bottom)
            }
            FieldPic::Field(Field::Top) => (
                Some(
                    expected_poc
                        .checked_add(delta0)
                        .ok_or(PocError::ArithmeticOverflow)?,
                ),
                None,
            ),
            FieldPic::Field(Field::Bottom) => {
                // Equation 8-8: a separately coded bottom field is anchored on
                // the complementary top field's TopFieldOrderCnt, plus the
                // top-to-bottom offset and this field's delta[0].
                let complementary = h
                    .last_top_field_poc
                    .ok_or(PocError::InconsistentAccessUnit(
                        "bottom field without its complementary top field",
                    ))?;
                let bottom = complementary
                    .checked_add(offset_for_top_to_bottom_field)
                    .and_then(|v| v.checked_add(delta0))
                    .ok_or(PocError::ArithmeticOverflow)?;
                (None, Some(bottom))
            }
        }
    };

    // Remember a separately coded top field so the complementary bottom field
    // can anchor on it (including an IDR top field).
    if matches!(slice.field_pic, FieldPic::Field(Field::Top)) {
        h.last_top_field_poc = top_cnt;
    }
    Ok((top_cnt, bottom_cnt))
}

// ---- pic_order_cnt_type == 2 (spec 8.2.1.3) -------------------------------

fn derive_type2(
    slice: &SliceHeader,
    is_idr: bool,
    is_reference: bool,
    frame_num_base: i64,
) -> Result<(Option<i64>, Option<i64>), PocError> {
    if is_idr {
        return match slice.field_pic {
            FieldPic::Frame | FieldPic::Field(Field::Top) => Ok((Some(0), None)),
            FieldPic::Field(Field::Bottom) => Ok((None, Some(0))),
        };
    }

    // 8.2.1.3: tempPicOrderCnt is 2*absFrameNum for a reference picture and
    // 2*absFrameNum - 1 for a non-reference picture. absFrameNum already
    // includes FrameNumOffset from the shared frame_num normalisation.
    let mut temp_poc = frame_num_base
        .checked_mul(2)
        .ok_or(PocError::ArithmeticOverflow)?;
    if !is_reference {
        temp_poc = temp_poc
            .checked_sub(1)
            .ok_or(PocError::ArithmeticOverflow)?;
    }

    let (top_cnt, bottom_cnt) = match &slice.field_pic {
        FieldPic::Frame => (Some(temp_poc), None),
        FieldPic::Field(Field::Top) => (Some(temp_poc), None),
        // A separate bottom field carries the same temporal count; 8.2.1.3
        // assigns tempPicOrderCnt directly, and it follows the complementary
        // top field within the same frame_num pair.
        FieldPic::Field(Field::Bottom) => (None, Some(temp_poc)),
    };

    Ok((top_cnt, bottom_cnt))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn picture_order_min_ignores_absent_field() {
        let p = PictureOrder {
            frame_num: 0,
            sps_id: 0,
            pps_id: 0,
            top_field_order_cnt: None,
            bottom_field_order_cnt: Some(7),
        };
        assert_eq!(p.min_field_order_cnt(), 7);
        let p = PictureOrder {
            frame_num: 0,
            sps_id: 0,
            pps_id: 0,
            top_field_order_cnt: Some(-3),
            bottom_field_order_cnt: Some(4),
        };
        assert_eq!(p.min_field_order_cnt(), -3);
    }
}
