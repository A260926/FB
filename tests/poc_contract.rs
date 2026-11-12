//! Contract tests for the access-unit-level picture order count reader.
//!
//! Every NAL here is built from scratch with a small bit writer, run through
//! emulation-prevention encoding, and then fed to `PocReader` as raw NAL bytes
//! (Annex B start codes removed), so the whole path is exercised: RBSP
//! decoding, SPS/PPS and `SliceHeader` parsing, and finally the POC state
//! machine. The expected field counts below are written out independently of
//! the derivation code.

use h264_reader::nal::UnitType;
use h264_reader::poc::{PocError, PocReader, PictureOrder};
use h264_reader::rbsp::{BitWriter, ByteWriter, BitWrite as _};

// ---- bit-level NAL construction -------------------------------------------

#[derive(Clone, Copy)]
enum PocKind {
    Type0 { lsb_minus4: u8 },
    Type1 {
        always_zero: bool,
        non_ref_offset: i32,
        top_bottom_offset: i32,
        offsets: &'static [i32],
    },
    Type2,
}

#[derive(Clone)]
struct SpsCfg {
    id: u32,
    log2_max_frame_num_minus4: u8,
    fields: bool,
    poc: PocKind,
    gaps_allowed: bool,
}

impl Default for SpsCfg {
    fn default() -> Self {
        SpsCfg {
            id: 0,
            log2_max_frame_num_minus4: 0,
            fields: false,
            poc: PocKind::Type0 { lsb_minus4: 0 },
            gaps_allowed: false,
        }
    }
}

fn encode_sps(cfg: &SpsCfg) -> Vec<u8> {
    let mut out = Vec::new();
    out.push(0x67); // nal_ref_idc=3, type 7
    let mut w = BitWriter::new(ByteWriter::new(&mut out));
    w.write::<8, u8>(66).unwrap(); // profile_idc (Baseline: no chroma block)
    w.write::<8, u8>(0).unwrap(); // constraint flags
    w.write::<8, u8>(10).unwrap(); // level_idc
    w.write_ue(cfg.id).unwrap();
    w.write_ue(cfg.log2_max_frame_num_minus4 as u32).unwrap();
    match cfg.poc {
        PocKind::Type0 { lsb_minus4 } => {
            w.write_ue(0).unwrap();
            w.write_ue(lsb_minus4 as u32).unwrap();
        }
        PocKind::Type1 {
            always_zero,
            non_ref_offset,
            top_bottom_offset,
            offsets,
        } => {
            w.write_ue(1).unwrap();
            w.write_bit(always_zero).unwrap();
            w.write_se(non_ref_offset).unwrap();
            w.write_se(top_bottom_offset).unwrap();
            w.write_ue(offsets.len() as u32).unwrap();
            for &o in offsets {
                w.write_se(o).unwrap();
            }
        }
        PocKind::Type2 => {
            w.write_ue(2).unwrap();
        }
    }
    w.write_ue(1).unwrap(); // max_num_ref_frames
    w.write_bit(cfg.gaps_allowed).unwrap();
    w.write_ue(0).unwrap(); // pic_width_in_mbs_minus1
    w.write_ue(0).unwrap(); // pic_height_in_map_units_minus1
    w.write_bit(!cfg.fields).unwrap(); // frame_mbs_only_flag
    if cfg.fields {
        w.write_bit(false).unwrap(); // mb_adaptive_frame_field_flag
    }
    w.write_bit(true).unwrap(); // direct_8x8_inference_flag
    w.write_bit(false).unwrap(); // frame_cropping_flag
    w.write_bit(false).unwrap(); // vui_parameters_present_flag
    w.write_rbsp_trailing_bits().unwrap();
    out
}

#[derive(Clone, Copy)]
struct PpsCfg {
    id: u32,
    sps_id: u32,
    bottom_field_poc_in_frame: bool,
}

impl Default for PpsCfg {
    fn default() -> Self {
        PpsCfg {
            id: 0,
            sps_id: 0,
            bottom_field_poc_in_frame: false,
        }
    }
}

fn encode_pps(cfg: &PpsCfg) -> Vec<u8> {
    let mut out = Vec::new();
    out.push(0x68); // nal_ref_idc=3, type 8
    let mut w = BitWriter::new(ByteWriter::new(&mut out));
    w.write_ue(cfg.id).unwrap();
    w.write_ue(cfg.sps_id).unwrap();
    w.write_bit(false).unwrap(); // entropy_coding_mode_flag (CAVLC)
    w.write_bit(cfg.bottom_field_poc_in_frame).unwrap();
    w.write_ue(0).unwrap(); // num_slice_groups_minus1
    w.write_ue(0).unwrap(); // num_ref_idx_l0_default_active_minus1
    w.write_ue(0).unwrap(); // num_ref_idx_l1_default_active_minus1
    w.write_bit(false).unwrap(); // weighted_pred_flag
    w.write::<2, u8>(0).unwrap(); // weighted_bipred_idc
    w.write_se(0).unwrap(); // pic_init_qp_minus26
    w.write_se(0).unwrap(); // pic_init_qs_minus26
    w.write_se(0).unwrap(); // chroma_qp_index_offset
    w.write_bit(false).unwrap(); // deblocking_filter_control_present_flag
    w.write_bit(false).unwrap(); // constrained_intra_pred_flag
    w.write_bit(false).unwrap(); // redundant_pic_cnt_present_flag
    w.write_rbsp_trailing_bits().unwrap();
    out
}

#[derive(Clone, Copy)]
#[allow(dead_code)]
enum SliceKind {
    I,
    P,
    B,
}

#[derive(Clone, Copy)]
enum FieldMode {
    Frame,
    TopField,
    BottomField,
}

#[derive(Clone)]
struct SliceCfg {
    pps_id: u32,
    first_mb: u32,
    frame_num: u32,
    frame_num_bits: u32,
    idr: bool,
    reference: bool,
    slice_kind: SliceKind,
    field: FieldMode,
    /// Mirrors the SPS `frame_mbs_only_flag == 0` setting: field_pic_flag is
    /// only present in the slice header when field coding is enabled.
    sps_fields: bool,
    /// Mirrors PPS bottom_field_pic_order_in_frame_present_flag: when set and
    /// the slice is a coded frame, the frame-level bottom field POC syntax is
    /// always present in the bitstream.
    frame_bottom_poc: bool,
    poc_lsb_bits: u32,
    poc_lsb: u32,
    delta_bottom: i32,
    delta0: i32,
    delta1: i32,
    has_delta0: bool,
    mmco5: bool,
}

impl Default for SliceCfg {
    fn default() -> Self {
        SliceCfg {
            pps_id: 0,
            first_mb: 0,
            frame_num: 0,
            frame_num_bits: 4,
            idr: false,
            reference: true,
            slice_kind: SliceKind::P,
            field: FieldMode::Frame,
            sps_fields: false,
            frame_bottom_poc: false,
            poc_lsb_bits: 4,
            poc_lsb: 0,
            delta_bottom: 0,
            delta0: 0,
            delta1: 0,
            has_delta0: true,
            mmco5: false,
        }
    }
}

fn encode_slice(cfg: &SliceCfg, poc: PocKind) -> Vec<u8> {
    let nal_type = if cfg.idr {
        UnitType::SliceLayerWithoutPartitioningIdr
    } else {
        UnitType::SliceLayerWithoutPartitioningNonIdr
    };
    let nal_ref_idc: u8 = if cfg.reference { 2 } else { 0 };
    let header = (nal_ref_idc << 5) | nal_type.id();

    let mut out = Vec::new();
    out.push(header);
    let mut w = BitWriter::new(ByteWriter::new(&mut out));
    w.write_ue(cfg.first_mb).unwrap();
    let slice_type_id = match cfg.slice_kind {
        SliceKind::I => 2u32,
        SliceKind::P => 0,
        SliceKind::B => 1,
    };
    w.write_ue(slice_type_id).unwrap();
    w.write_ue(cfg.pps_id).unwrap();
    w.write_var(cfg.frame_num_bits, cfg.frame_num).unwrap();

    // field_pic_flag exists only when the SPS enabled field coding.
    if cfg.sps_fields {
        let is_field = !matches!(cfg.field, FieldMode::Frame);
        w.write_bit(is_field).unwrap();
        if is_field {
            w.write_bit(matches!(cfg.field, FieldMode::BottomField)).unwrap();
        }
    }

    if cfg.idr {
        w.write_ue(0).unwrap(); // idr_pic_id
    }

    let frame = matches!(cfg.field, FieldMode::Frame);
    match poc {
        PocKind::Type0 { .. } => {
            w.write_var(cfg.poc_lsb_bits, cfg.poc_lsb).unwrap();
            if frame && cfg.frame_bottom_poc {
                w.write_se(cfg.delta_bottom).unwrap();
            }
        }
        PocKind::Type1 { always_zero, .. } => {
            if !always_zero && cfg.has_delta0 {
                w.write_se(cfg.delta0).unwrap();
                if frame && cfg.frame_bottom_poc {
                    w.write_se(cfg.delta1).unwrap();
                }
            }
        }
        PocKind::Type2 => {}
    }

    if matches!(cfg.slice_kind, SliceKind::B) {
        w.write_bit(false).unwrap(); // direct_spatial_mv_pred_flag
        w.write_bit(false).unwrap(); // num_ref_idx_active_override_flag
    }
    if matches!(cfg.slice_kind, SliceKind::P) {
        w.write_bit(false).unwrap(); // num_ref_idx_active_override_flag
    }
    match cfg.slice_kind {
        SliceKind::I => {}
        SliceKind::P => {
            w.write_bit(false).unwrap(); // ref_pic_list_modification_flag_l0
        }
        SliceKind::B => {
            w.write_bit(false).unwrap(); // ref_pic_list_modification_flag_l0
            w.write_bit(false).unwrap(); // ref_pic_list_modification_flag_l1
        }
    }

    if cfg.reference {
        if cfg.idr {
            w.write_bit(false).unwrap(); // no_output_of_prior_pics_flag
            w.write_bit(false).unwrap(); // long_term_reference_flag
        } else {
            w.write_bit(cfg.mmco5).unwrap(); // adaptive_ref_pic_marking_mode_flag
            if cfg.mmco5 {
                w.write_ue(5).unwrap(); // memory_management_control_operation
                w.write_ue(0).unwrap(); // end
            }
        }
    }

    w.write_se(0).unwrap(); // slice_qp_delta
    // Placeholder slice data: the parser only reads the header and checks
    // `has_more_rbsp_data`, which must see a byte beyond the header fields.
    w.write::<8, u8>(0xFF).unwrap();
    w.write_rbsp_trailing_bits().unwrap();
    out
}

fn aud_nal() -> Vec<u8> {
    vec![0x09, 0x10] // primary_pic_type=0, RBSP trailing bits
}

fn sei_nal() -> Vec<u8> {
    vec![0x06, 0x80] // payload type/size 0 is not parsed by the POC reader
}

fn param_aus(reader: &mut PocReader, sps: &SpsCfg, pps: &PpsCfg) {
    reader
        .push_access_unit(&[&encode_sps(sps)])
        .expect("SPS AU");
    reader
        .push_access_unit(&[&encode_pps(pps)])
        .expect("PPS AU");
}

fn only(p: Option<PictureOrder>) -> PictureOrder {
    p.expect("access unit with a primary picture must yield PictureOrder")
}

// ---- tests ----------------------------------------------------------------

#[test]
fn non_vcl_access_units_return_none() {
    let mut reader = PocReader::new();
    let sps = SpsCfg::default();
    let pps = PpsCfg::default();
    assert_eq!(
        reader
            .push_access_unit(&[&aud_nal(), &encode_sps(&sps), &encode_pps(&pps), &sei_nal()])
            .unwrap(),
        None
    );
}

#[test]
fn first_picture_must_be_idr() {
    let mut reader = PocReader::new();
    let sps = SpsCfg::default();
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);
    let mut slice = SliceCfg::default();
    slice.idr = false;
    slice.reference = true;
    slice.frame_num = 0;
    let err = reader
        .push_access_unit(&[&encode_slice(&slice, sps.poc)])
        .unwrap_err();
    assert!(matches!(err, PocError::FirstPictureNotIdr), "got {:?}", err);
}

#[test]
fn type0_half_modulus_and_non_reference_history() {
    let mut reader = PocReader::new();
    let sps = SpsCfg {
        poc: PocKind::Type0 { lsb_minus4: 0 }, // MaxPicOrderCntLsb = 16
        ..Default::default()
    };
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);

    let mut idr = SliceCfg::default();
    idr.idr = true;
    idr.poc_lsb = 0;
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&idr, sps.poc)]).unwrap()).top_field_order_cnt, Some(0));

    let ref_pic = |frame_num: u32, lsb: u32| {
        let mut s = SliceCfg::default();
        s.frame_num = frame_num;
        s.poc_lsb = lsb;
        s
    };

    // lsb 4 -> top 4
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&ref_pic(1, 4), sps.poc)]).unwrap()).top_field_order_cnt, Some(4));

    // A non-reference B at lsb 13 must not move the PrevPicOrderCnt* history,
    // which still records lsb 4 afterwards.
    let mut b = ref_pic(2, 13);
    b.reference = false;
    b.slice_kind = SliceKind::B;
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&b, sps.poc)]).unwrap()).top_field_order_cnt, Some(13));

    // lsb 12 is exactly half the modulus above the stored lsb 4. The downward
    // wrap needs a strictly greater difference, so the MSB does not move yet.
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&ref_pic(2, 12), sps.poc)]).unwrap()).top_field_order_cnt, Some(12));

    // Back to lsb 4: the difference is exactly MaxPicOrderCntLsb/2 and the lsb
    // decreased, so the greater-than-or-equal boundary wraps the MSB upward.
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&ref_pic(3, 4), sps.poc)]).unwrap()).top_field_order_cnt, Some(20));

    // lsb 13 is now more than half a modulus above lsb 4: strictly greater
    // wraps the MSB back down.
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&ref_pic(4, 13), sps.poc)]).unwrap()).top_field_order_cnt, Some(13));
}

#[test]
fn type0_frame_bottom_field_delta() {
    let mut reader = PocReader::new();
    let sps = SpsCfg {
        fields: true,
        poc: PocKind::Type0 { lsb_minus4: 0 },
        ..Default::default()
    };
    let pps = PpsCfg {
        bottom_field_poc_in_frame: true,
        ..Default::default()
    };
    param_aus(&mut reader, &sps, &pps);

    let mut idr = SliceCfg::default();
    idr.idr = true;
    idr.field = FieldMode::Frame;
    idr.sps_fields = true;
    idr.frame_bottom_poc = true;
    let idr_pic = only(reader.push_access_unit(&[&encode_slice(&idr, sps.poc)]).unwrap());
    assert_eq!(idr_pic.top_field_order_cnt, Some(0));
    assert_eq!(idr_pic.bottom_field_order_cnt, Some(0));

    let mut frame = SliceCfg::default();
    frame.frame_num = 1;
    frame.field = FieldMode::Frame;
    frame.sps_fields = true;
    frame.frame_bottom_poc = true;
    frame.poc_lsb = 2;
    frame.delta_bottom = 1;
    let pic = only(reader.push_access_unit(&[&encode_slice(&frame, sps.poc)]).unwrap());
    assert_eq!(pic.top_field_order_cnt, Some(2));
    assert_eq!(pic.bottom_field_order_cnt, Some(3));
    assert_eq!(pic.min_field_order_cnt(), 2);
}

#[test]
fn type1_cycle_with_positive_and_negative_offsets() {
    let mut reader = PocReader::new();
    let sps = SpsCfg {
        poc: PocKind::Type1 {
            always_zero: false,
            non_ref_offset: -1,
            top_bottom_offset: 2,
            offsets: &[4, -2],
        },
        ..Default::default()
    };
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);

    let mut idr = SliceCfg::default();
    idr.idr = true;
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&idr, sps.poc)]).unwrap()).top_field_order_cnt, Some(0));

    let ref_pic = |frame_num: u32, delta0: i32| {
        let mut s = SliceCfg::default();
        s.frame_num = frame_num;
        s.delta0 = delta0;
        s.has_delta0 = true;
        s
    };

    // absFrameNum 1 -> first offset (+4)
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&ref_pic(1, 0), sps.poc)]).unwrap()).top_field_order_cnt, Some(4));
    // absFrameNum 2 -> +4 then -2
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&ref_pic(2, 0), sps.poc)]).unwrap()).top_field_order_cnt, Some(2));
    // absFrameNum 3 -> one full cycle (net +2) then +4, plus delta0 of +1
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&ref_pic(3, 1), sps.poc)]).unwrap()).top_field_order_cnt, Some(7));

    // Non-reference B carrying the upcoming frame_num: absFrameNum is
    // decremented first and offset_for_non_ref_pic (-1) is added.
    let mut b = ref_pic(4, 0);
    b.reference = false;
    b.slice_kind = SliceKind::B;
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&b, sps.poc)]).unwrap()).top_field_order_cnt, Some(5));

    // Reference at frame_num 4: whole cycle + both within-cycle offsets.
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&ref_pic(4, 0), sps.poc)]).unwrap()).top_field_order_cnt, Some(4));
}

#[test]
fn type1_empty_cycle_is_supported() {
    let mut reader = PocReader::new();
    let sps = SpsCfg {
        poc: PocKind::Type1 {
            always_zero: true,
            non_ref_offset: -3,
            top_bottom_offset: 1,
            offsets: &[],
        },
        ..Default::default()
    };
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);

    let mut idr = SliceCfg::default();
    idr.idr = true;
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&idr, sps.poc)]).unwrap()).top_field_order_cnt, Some(0));

    // With an empty cycle expectedPicOrderCnt stays zero and deltas are absent.
    let mut p = SliceCfg::default();
    p.frame_num = 1;
    p.has_delta0 = false;
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&p, sps.poc)]).unwrap()).top_field_order_cnt, Some(0));

    // Non-reference picture with empty cycle: no absFrameNum decrement, no
    // offset_for_non_ref_pic addition.
    let mut b = SliceCfg::default();
    b.frame_num = 2;
    b.reference = false;
    b.slice_kind = SliceKind::B;
    b.has_delta0 = false;
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&b, sps.poc)]).unwrap()).top_field_order_cnt, Some(0));
}

#[test]
fn type2_frame_num_wraparound_with_non_reference_between() {
    let mut reader = PocReader::new();
    let sps = SpsCfg {
        log2_max_frame_num_minus4: 0, // MaxFrameNum = 16
        poc: PocKind::Type2,
        ..Default::default()
    };
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);

    let mut idr = SliceCfg::default();
    idr.idr = true;
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&idr, sps.poc)]).unwrap()).top_field_order_cnt, Some(0));

    let mut last_ref = 0u32;
    for frame_num in 1u32..=15 {
        let mut s = SliceCfg::default();
        s.frame_num = frame_num;
        let expected = (frame_num as i64) * 2;
        assert_eq!(
            only(reader.push_access_unit(&[&encode_slice(&s, sps.poc)]).unwrap()).top_field_order_cnt,
            Some(expected),
            "reference frame_num {frame_num}"
        );
        last_ref = frame_num;
    }
    assert_eq!(last_ref, 15);

    // Non-reference B using the wrapped frame_num 0: 2*0 - 1 = -1.
    let mut b = SliceCfg::default();
    b.frame_num = 0;
    b.reference = false;
    b.slice_kind = SliceKind::B;
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&b, sps.poc)]).unwrap()).top_field_order_cnt, Some(-1));

    // Reference frame_num 0 triggers the MaxFrameNum wraparound.
    let mut wrapped = SliceCfg::default();
    wrapped.frame_num = 0;
    assert_eq!(only(reader.push_access_unit(&[&encode_slice(&wrapped, sps.poc)]).unwrap()).top_field_order_cnt, Some(32));
}

#[test]
fn multiple_slices_in_one_access_unit_return_one_picture() {
    let mut reader = PocReader::new();
    let sps = SpsCfg {
        poc: PocKind::Type0 { lsb_minus4: 0 },
        ..Default::default()
    };
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);

    let mut s0 = SliceCfg::default();
    s0.idr = true;
    s0.first_mb = 0;
    let mut s1 = SliceCfg::default();
    s1.idr = true;
    s1.first_mb = 0; // first_mb may differ; equality here is harmless
    let n0 = encode_slice(&s0, sps.poc);
    s1.first_mb = 0;
    let n1 = encode_slice(&s1, sps.poc);
    let pic = only(reader.push_access_unit(&[&n0, &n1]).unwrap());
    assert_eq!(pic.top_field_order_cnt, Some(0));
    assert_eq!(pic.frame_num, 0);
    assert_eq!(pic.sps_id, 0);
    assert_eq!(pic.pps_id, 0);

    // Two slices with a different frame_num in the same AU are rejected.
    let mut a = SliceCfg::default();
    a.frame_num = 1;
    a.poc_lsb = 2;
    let mut mixed = SliceCfg::default();
    mixed.frame_num = 2;
    mixed.poc_lsb = 4;
    let err = reader
        .push_access_unit(&[&encode_slice(&a, sps.poc), &encode_slice(&mixed, sps.poc)])
        .unwrap_err();
    assert!(matches!(err, PocError::InconsistentAccessUnit(_)), "got {:?}", err);

    // The failed AU committed nothing: the same single-slice picture now works.
    let pic = only(reader.push_access_unit(&[&encode_slice(&a, sps.poc)]).unwrap());
    assert_eq!(pic.top_field_order_cnt, Some(2));
}

#[test]
fn separate_top_and_bottom_fields_type0() {
    let mut reader = PocReader::new();
    let sps = SpsCfg {
        fields: true,
        poc: PocKind::Type0 { lsb_minus4: 0 },
        ..Default::default()
    };
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);

    let mut idr_top = SliceCfg::default();
    idr_top.idr = true;
    idr_top.field = FieldMode::TopField;
    idr_top.sps_fields = true;
    let top = only(reader.push_access_unit(&[&encode_slice(&idr_top, sps.poc)]).unwrap());
    assert_eq!(top.top_field_order_cnt, Some(0));
    assert_eq!(top.bottom_field_order_cnt, None);

    let mut idr_bottom = SliceCfg::default();
    idr_bottom.idr = true;
    idr_bottom.field = FieldMode::BottomField;
    idr_bottom.sps_fields = true;
    let bottom = only(reader.push_access_unit(&[&encode_slice(&idr_bottom, sps.poc)]).unwrap());
    assert_eq!(bottom.top_field_order_cnt, None);
    assert_eq!(bottom.bottom_field_order_cnt, Some(0));

    let mut p_top = SliceCfg::default();
    p_top.frame_num = 1;
    p_top.field = FieldMode::TopField;
    p_top.sps_fields = true;
    p_top.poc_lsb = 2;
    let top = only(reader.push_access_unit(&[&encode_slice(&p_top, sps.poc)]).unwrap());
    assert_eq!(top.top_field_order_cnt, Some(2));
    assert_eq!(top.bottom_field_order_cnt, None);

    let mut p_bottom = SliceCfg::default();
    p_bottom.frame_num = 1;
    p_bottom.field = FieldMode::BottomField;
    p_bottom.sps_fields = true;
    p_bottom.poc_lsb = 2;
    let bottom = only(reader.push_access_unit(&[&encode_slice(&p_bottom, sps.poc)]).unwrap());
    assert_eq!(bottom.top_field_order_cnt, None);
    assert_eq!(bottom.bottom_field_order_cnt, Some(2));
}

#[test]
fn separate_top_and_bottom_fields_type1() {
    let mut reader = PocReader::new();
    let sps = SpsCfg {
        fields: true,
        poc: PocKind::Type1 {
            always_zero: false,
            non_ref_offset: -1,
            top_bottom_offset: 2,
            offsets: &[4],
        },
        ..Default::default()
    };
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);

    let mut idr_top = SliceCfg::default();
    idr_top.idr = true;
    idr_top.field = FieldMode::TopField;
    idr_top.sps_fields = true;
    assert_eq!(
        only(reader.push_access_unit(&[&encode_slice(&idr_top, sps.poc)]).unwrap())
            .top_field_order_cnt,
        Some(0)
    );

    // Reference top field: frame_num 1 is doubled to absFrameNum 2 for field
    // coding, so two +4 offsets accumulate.
    let mut top = SliceCfg::default();
    top.frame_num = 1;
    top.field = FieldMode::TopField;
    top.sps_fields = true;
    top.delta0 = 0;
    assert_eq!(
        only(reader.push_access_unit(&[&encode_slice(&top, sps.poc)]).unwrap())
            .top_field_order_cnt,
        Some(8)
    );

    // The complementary bottom field shares frame_num 1; equation 8-8 anchors
    // it on the top field's count plus the top-to-bottom offset (2) and the
    // field delta (0): 8 + 2 + 0 = 10.
    let mut bottom = SliceCfg::default();
    bottom.frame_num = 1;
    bottom.field = FieldMode::BottomField;
    bottom.sps_fields = true;
    bottom.delta0 = 0;
    let pic = only(reader.push_access_unit(&[&encode_slice(&bottom, sps.poc)]).unwrap());
    assert_eq!(pic.top_field_order_cnt, None);
    assert_eq!(pic.bottom_field_order_cnt, Some(10));
}

#[test]
fn mmco5_resets_history_and_next_picture_uses_fresh_state() {
    let mut reader = PocReader::new();
    let sps = SpsCfg {
        poc: PocKind::Type0 { lsb_minus4: 0 },
        ..Default::default()
    };
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);

    let mut idr = SliceCfg::default();
    idr.idr = true;
    assert_eq!(
        only(reader.push_access_unit(&[&encode_slice(&idr, sps.poc)]).unwrap())
            .top_field_order_cnt,
        Some(0)
    );

    let mut p1 = SliceCfg::default();
    p1.frame_num = 1;
    p1.poc_lsb = 4;
    assert_eq!(
        only(reader.push_access_unit(&[&encode_slice(&p1, sps.poc)]).unwrap())
            .top_field_order_cnt,
        Some(4)
    );
    let mut p2 = SliceCfg::default();
    p2.frame_num = 2;
    p2.poc_lsb = 12; // difference of exactly half the modulus does not wrap
    assert_eq!(
        only(reader.push_access_unit(&[&encode_slice(&p2, sps.poc)]).unwrap())
            .top_field_order_cnt,
        Some(12)
    );

    // MMCO 5 on frame_num 2: POC history restarts, so lsb 6 is read with a
    // zero MSB.
    let mut mmco = SliceCfg::default();
    mmco.frame_num = 2;
    mmco.poc_lsb = 6;
    mmco.mmco5 = true;
    assert_eq!(
        only(reader.push_access_unit(&[&encode_slice(&mmco, sps.poc)]).unwrap())
            .top_field_order_cnt,
        Some(6)
    );

    // frame_num and POC history were both reset: a normal reference picture
    // with frame_num 0 and lsb 8 follows without any wraparound.
    let mut after = SliceCfg::default();
    after.frame_num = 0;
    after.poc_lsb = 8;
    let pic = only(reader.push_access_unit(&[&encode_slice(&after, sps.poc)]).unwrap());
    assert_eq!(pic.top_field_order_cnt, Some(8));
    assert_eq!(pic.frame_num, 0);
}

#[test]
fn mmco5_twice_in_one_access_unit_is_rejected() {
    let mut reader = PocReader::new();
    let sps = SpsCfg {
        poc: PocKind::Type0 { lsb_minus4: 0 },
        ..Default::default()
    };
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);

    let mut idr = SliceCfg::default();
    idr.idr = true;
    reader.push_access_unit(&[&encode_slice(&idr, sps.poc)]).unwrap();

    // Two VCL NALs each carrying MMCO 5 must be flagged.
    let mut s = SliceCfg::default();
    s.frame_num = 1;
    s.mmco5 = true;
    let n1 = encode_slice(&s, sps.poc);
    let n2 = encode_slice(&s, sps.poc);
    let err = reader.push_access_unit(&[&n1, &n2]).unwrap_err();
    assert!(matches!(err, PocError::InconsistentAccessUnit(_)), "got {:?}", err);
}

#[test]
fn parameter_set_resend_and_inactive_declaration() {
    let mut reader = PocReader::new();
    let sps0 = SpsCfg {
        id: 0,
        poc: PocKind::Type0 { lsb_minus4: 0 },
        ..Default::default()
    };
    let pps0 = PpsCfg::default();
    param_aus(&mut reader, &sps0, &pps0);

    // An inactive SPS (id 1, different POC type) is declared but never
    // referenced by a picture: it must neither advance nor reset POC state.
    let sps1 = SpsCfg {
        id: 1,
        poc: PocKind::Type2,
        ..Default::default()
    };
    reader.push_access_unit(&[&encode_sps(&sps1)]).unwrap();
    // Identical re-send of the active SPS and PPS: no reset either.
    reader.push_access_unit(&[&encode_sps(&sps0)]).unwrap();
    reader.push_access_unit(&[&encode_pps(&pps0)]).unwrap();

    let mut idr = SliceCfg::default();
    idr.idr = true;
    assert_eq!(
        only(reader.push_access_unit(&[&encode_slice(&idr, sps0.poc)]).unwrap())
            .top_field_order_cnt,
        Some(0)
    );
    let mut p = SliceCfg::default();
    p.frame_num = 1;
    p.poc_lsb = 2;
    assert_eq!(
        only(reader.push_access_unit(&[&encode_slice(&p, sps0.poc)]).unwrap())
            .top_field_order_cnt,
        Some(2)
    );
}

#[test]
fn missing_parameter_sets_are_errors() {
    let mut reader = PocReader::new();
    let sps = SpsCfg::default();
    // Only the SPS is declared; the slice references PPS 0.
    reader.push_access_unit(&[&encode_sps(&sps)]).unwrap();
    let s = SliceCfg::default();
    let err = reader
        .push_access_unit(&[&encode_slice(&s, sps.poc)])
        .unwrap_err();
    assert!(matches!(err, PocError::MissingPps(0)), "got {:?}", err);

    // A PPS referencing an undeclared SPS is also an error.
    let pps = PpsCfg {
        sps_id: 7,
        ..Default::default()
    };
    let err = reader
        .push_access_unit(&[&encode_pps(&pps)])
        .unwrap_err();
    assert!(matches!(err, PocError::MalformedNal(_)), "got {:?}", err);
}

#[test]
fn truncated_slice_header_is_rejected() {
    let mut reader = PocReader::new();
    let sps = SpsCfg::default();
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);
    let mut good = encode_slice(&{
        let mut s = SliceCfg::default();
        s.idr = true;
        s
    }, sps.poc);
    // Drop the payload and trailing bytes, leaving a truncated header.
    good.truncate(2);
    let err = reader.push_access_unit(&[&good]).unwrap_err();
    assert!(matches!(err, PocError::MalformedNal(_)), "got {:?}", err);
}

#[test]
fn unsupported_and_empty_nals_are_rejected() {
    let mut reader = PocReader::new();
    // NAL type 20 (slice extension, SVC/MVC) is unsupported.
    let ext = vec![0x14u8, 0x80];
    let err = reader.push_access_unit(&[&ext]).unwrap_err();
    assert!(matches!(err, PocError::UnsupportedNal(_)), "got {:?}", err);

    // Data partition A (NAL type 2) is unsupported.
    let part = vec![0x02u8, 0x80];
    let err = reader.push_access_unit(&[&part]).unwrap_err();
    assert!(matches!(err, PocError::UnsupportedNal(_)), "got {:?}", err);

    let empty: Vec<u8> = Vec::new();
    let err = reader.push_access_unit(&[&empty]).unwrap_err();
    assert!(matches!(err, PocError::EmptyNal), "got {:?}", err);
}

#[test]
fn failed_access_unit_leaves_no_trace_against_control() {
    let sps = SpsCfg {
        poc: PocKind::Type0 { lsb_minus4: 0 },
        ..Default::default()
    };
    let pps = PpsCfg::default();

    let mut control = PocReader::new();
    let mut tested = PocReader::new();
    for r in [&mut control, &mut tested] {
        param_aus(r, &sps, &pps);
        let mut idr = SliceCfg::default();
        idr.idr = true;
        r.push_access_unit(&[&encode_slice(&idr, sps.poc)]).unwrap();
        let mut p = SliceCfg::default();
        p.frame_num = 1;
        p.poc_lsb = 2;
        r.push_access_unit(&[&encode_slice(&p, sps.poc)]).unwrap();
    }

    // The tested reader swallows a bad AU (undeclared PPS 9).
    let mut bad = SliceCfg::default();
    bad.pps_id = 9;
    bad.frame_num = 2;
    assert!(tested
        .push_access_unit(&[&encode_slice(&bad, sps.poc)])
        .is_err());

    // Both readers then receive identical good AUs and must agree.
    for (frame_num, lsb) in [(2u32, 4u32), (3, 6), (4, 8)] {
        let mut s = SliceCfg::default();
        s.frame_num = frame_num;
        s.poc_lsb = lsb;
        let nal = encode_slice(&s, sps.poc);
        let a = only(control.push_access_unit(&[&nal]).unwrap());
        let nal = encode_slice(&s, sps.poc);
        let b = only(tested.push_access_unit(&[&nal]).unwrap());
        assert_eq!(a, b);
    }
}

#[test]
fn reset_keeps_parameter_sets_but_requires_idr() {
    let mut reader = PocReader::new();
    let sps = SpsCfg {
        poc: PocKind::Type2,
        ..Default::default()
    };
    let pps = PpsCfg::default();
    param_aus(&mut reader, &sps, &pps);

    let mut idr = SliceCfg::default();
    idr.idr = true;
    reader.push_access_unit(&[&encode_slice(&idr, sps.poc)]).unwrap();
    let mut p = SliceCfg::default();
    p.frame_num = 1;
    assert_eq!(
        only(reader.push_access_unit(&[&encode_slice(&p, sps.poc)]).unwrap())
            .top_field_order_cnt,
        Some(2)
    );

    reader.reset();

    // Parameter sets survive reset; history does not, so a non-IDR is rejected.
    let mut non_idr = SliceCfg::default();
    non_idr.frame_num = 2;
    let err = reader
        .push_access_unit(&[&encode_slice(&non_idr, sps.poc)])
        .unwrap_err();
    assert!(matches!(err, PocError::FirstPictureNotIdr), "got {:?}", err);

    // No need to redeclare SPS/PPS: an IDR establishes fresh history.
    let mut idr2 = SliceCfg::default();
    idr2.idr = true;
    let pic = only(reader.push_access_unit(&[&encode_slice(&idr2, sps.poc)]).unwrap());
    assert_eq!(pic.top_field_order_cnt, Some(0));

    let mut p2 = SliceCfg::default();
    p2.frame_num = 1;
    assert_eq!(
        only(reader.push_access_unit(&[&encode_slice(&p2, sps.poc)]).unwrap())
            .top_field_order_cnt,
        Some(2)
    );
}

#[test]
fn input_bytes_can_be_reused_after_call() {
    let mut reader = PocReader::new();
    let sps = SpsCfg::default();
    let pps = PpsCfg::default();
    let sps_nal = encode_sps(&sps);
    let pps_nal = encode_pps(&pps);
    reader.push_access_unit(&[&sps_nal, &pps_nal]).unwrap();
    // Mutate and reuse the backing storage after the call returned.
    let mut reused = sps_nal.clone();
    reused.fill(0xFF);
    let mut idr = SliceCfg::default();
    idr.idr = true;
    let slice_nal = encode_slice(&idr, sps.poc);
    let pic = only(reader.push_access_unit(&[&slice_nal]).unwrap());
    assert_eq!(pic.top_field_order_cnt, Some(0));
    assert_eq!(reused[0], 0xFF);
}
