//! The original single-slot starting point exercised by `baseline/run.sh`:
//! parse a little line program containing two sequences through `DebugLine`,
//! read six rows in total, and resume each sequence once via `resume_from`.
//!
//! This deliberately stays on the ordinary non-VLIW path; the VLIW carry
//! contract lives in `line_contract.rs`.

use gimli::{DebugLine, DebugLineOffset, EndianSlice, LittleEndian};

/// Two DWARF 4 / DWARF32 / little-endian sequences, three ordinary rows each
/// plus their end-of-sequence row.
fn section_bytes() -> Vec<u8> {
    #[rustfmt::skip]
    let program: &[u8] = &[
        // Sequence 1: set_address 0x1000; copy; advance_pc 2; copy;
        // advance_pc 4; copy; end_sequence.
        0x00, 0x05, 0x02, 0x00, 0x10, 0x00, 0x00,
        0x01,
        0x02, 0x02,
        0x01,
        0x02, 0x04,
        0x01,
        0x00, 0x01, 0x01,

        // Sequence 2: set_address 0x4000; same shape.
        0x00, 0x05, 0x02, 0x00, 0x40, 0x00, 0x00,
        0x01,
        0x02, 0x02,
        0x01,
        0x02, 0x04,
        0x01,
        0x00, 0x01, 0x01,
    ];

    let mut header_payload: Vec<u8> = Vec::new();
    header_payload.push(1); // minimum instruction length
    header_payload.push(1); // maximum operations per instruction
    header_payload.push(1); // default is_stmt
    header_payload.push(0); // line base
    header_payload.push(1); // line range
    header_payload.push(3); // opcode base
    header_payload.extend_from_slice(&[0, 1]); // standard opcode lengths
    header_payload.push(0); // no include directories
    header_payload.push(0); // no file names

    let mut body = Vec::new();
    body.extend_from_slice(&4u16.to_le_bytes());
    body.extend_from_slice(&(header_payload.len() as u32).to_le_bytes());
    body.extend_from_slice(&header_payload);
    body.extend_from_slice(program);

    let mut section = Vec::new();
    section.extend_from_slice(&(body.len() as u32).to_le_bytes());
    section.extend_from_slice(&body);
    section
}

#[test]
fn baseline_start() {
    let section = section_bytes();
    let debug_line = DebugLine::new(&section, LittleEndian);
    let program = debug_line
        .program(DebugLineOffset(0), 4, None, None)
        .expect("header parses");

    // Six ordinary rows across the two sequences.
    let mut ordinary = 0u64;
    let mut end_sequences = 0u64;
    let mut rows = program.clone().rows();
    while let Some((_header, row)) = rows.next_row().expect("row executes") {
        if row.end_sequence() {
            end_sequences += 1;
        } else {
            ordinary += 1;
            assert_eq!(row.op_index(), 0, "starting point is single-slot only");
        }
    }
    assert_eq!(ordinary, 6);
    assert_eq!(end_sequences, 2);

    // Resume each sequence independently and count its ordinary rows.
    let (complete, sequences) = program.sequences().expect("sequences resolve");
    assert_eq!(sequences.len(), 2);
    let expected_starts = [0x1000u64, 0x4000];
    for (idx, sequence) in sequences.iter().enumerate() {
        assert_eq!(sequence.start, expected_starts[idx]);
        let mut count = 0u64;
        let mut resumed = complete.resume_from(sequence);
        while let Some((_header, row)) = resumed.next_row().expect("resumed row executes") {
            if !row.end_sequence() {
                count += 1;
            }
        }
        assert_eq!(count, 3, "sequence {idx} resumes with its three rows");
    }

    // Keep the EndianSlice import meaningful for readers.
    let _ = EndianSlice::new(&section, LittleEndian);
}
