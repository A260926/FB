//! Dumps the picture order count of every primary coded picture in an Annex B
//! stream.
//!
//! The [`AnnexBReader`] splits the byte stream into complete NAL units; this
//! example then groups those NALs into access units (a new access unit starts
//! at each VCL NAL that follows another VCL NAL, per H.264 7.4.1.2.4) and feeds
//! each access unit to a [`PocReader`]. AU boundaries in `push_access_unit` are
//! the caller's responsibility, which is exactly the grouping done here.
//!
//! Usage:
//!
//! ```text
//! cargo run --example poc_dump -- path/to/stream.h264
//! ```

use h264_reader::annexb::AnnexBReader;
use h264_reader::nal::{Nal, RefNal, UnitType};
use h264_reader::poc::PocReader;
use h264_reader::push::NalInterest;
use std::io::Read;

fn is_vcl(unit_type: UnitType) -> bool {
    matches!(
        unit_type,
        UnitType::SliceLayerWithoutPartitioningNonIdr
            | UnitType::SliceLayerWithoutPartitioningIdr
    )
}

fn main() {
    let path = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "verification/poc-fixture.h264".to_string());
    let mut file = std::fs::File::open(&path).expect("open input stream");
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes).expect("read input stream");

    // Complete NAL units, in stream order.
    let mut nals: Vec<Vec<u8>> = Vec::new();
    let mut reader = AnnexBReader::accumulate(|nal: RefNal<'_>| {
        if nal.is_complete() {
            let mut buf = Vec::new();
            nal.reader().read_to_end(&mut buf).expect("read NAL");
            nals.push(buf);
        }
        NalInterest::Buffer
    });
    reader.push(&bytes);
    reader.reset();

    let mut poc_reader = PocReader::new();
    // NALs of the access unit currently being assembled.
    let mut current: Vec<Vec<u8>> = Vec::new();
    let mut current_has_vcl = false;
    let mut picture_index = 0u32;

    let flush = |poc_reader: &mut PocReader,
                     current: &mut Vec<Vec<u8>>,
                     has_vcl: &mut bool,
                     picture_index: &mut u32| {
        if current.is_empty() {
            return;
        }
        let refs: Vec<&[u8]> = current.iter().map(Vec::as_slice).collect();
        match poc_reader.push_access_unit(&refs) {
            Ok(Some(poc)) => {
                *picture_index += 1;
                let top = poc
                    .top_field_order_cnt
                    .map(|v| v.to_string())
                    .unwrap_or_else(|| "-".to_string());
                let bottom = poc
                    .bottom_field_order_cnt
                    .map(|v| v.to_string())
                    .unwrap_or_else(|| "-".to_string());
                println!(
                    "picture {}: frame_num={} sps={} pps={} top={} bottom={} min={}",
                    picture_index,
                    poc.frame_num,
                    poc.sps_id,
                    poc.pps_id,
                    top,
                    bottom,
                    poc.min_field_order_cnt(),
                );
            }
            Ok(None) => {
                // Non-VCL access unit (SPS/PPS/AUD/SEI): nothing to print.
            }
            Err(e) => {
                eprintln!("POC error: {e}");
                std::process::exit(1);
            }
        }
        current.clear();
        *has_vcl = false;
    };

    for nal in nals {
        let header = RefNal::new(&nal, &[], true)
            .header()
            .expect("NAL header");
        let vcl = is_vcl(header.nal_unit_type());
        if vcl && current_has_vcl {
            // The previous VCL NAL finished the preceding access unit.
            flush(
                &mut poc_reader,
                &mut current,
                &mut current_has_vcl,
                &mut picture_index,
            );
        }
        current_has_vcl |= vcl;
        current.push(nal);
    }
    flush(
        &mut poc_reader,
        &mut current,
        &mut current_has_vcl,
        &mut picture_index,
    );
}
