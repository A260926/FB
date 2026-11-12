//! Deterministic generation of line number program bytes.
//!
//! The planner first produces a stream of [`ModelInstr`] values. That exact
//! stream is handed both to the independent [`crate::model::LineModel`] and to
//! the byte encoder below, so production rows can never influence the expected
//! matrix.

use gimli::constants;

use crate::leb::{sleb128, uleb128};
use crate::model::ModelInstr;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum DwarfFormat {
    Dwarf32,
    Dwarf64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Endian {
    Little,
    Big,
}

/// One of the 24 contract combinations.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Combo {
    pub index: usize,
    pub version: u16,
    pub format: DwarfFormat,
    pub endian: Endian,
    pub max_ops: u8,
    pub min_insn_len: u8,
}

impl Combo {
    pub fn label(&self) -> String {
        format!(
            "v{}/{}/{}/mop{}/mil{}",
            self.version,
            if self.format == DwarfFormat::Dwarf32 {
                "dwarf32"
            } else {
                "dwarf64"
            },
            if self.endian == Endian::Little { "le" } else { "be" },
            self.max_ops,
            self.min_insn_len
        )
    }
}

/// The 24 combinations: DWARF 4/5 x DWARF32/64 x LE/BE x max-ops (1,3,5),
/// each paired with a fixed minimum instruction length (1,2,4).
pub(crate) fn all_combos() -> Vec<Combo> {
    let mut combos = Vec::new();
    let mut index = 0usize;
    for &version in &[4u16, 5] {
        for &format in &[DwarfFormat::Dwarf32, DwarfFormat::Dwarf64] {
            for &endian in &[Endian::Little, Endian::Big] {
                for (&max_ops, &min_insn_len) in
                    [1u8, 3, 5].iter().zip([1u8, 2, 4].iter())
                {
                    combos.push(Combo {
                        index,
                        version,
                        format,
                        endian,
                        max_ops,
                        min_insn_len,
                    });
                    index += 1;
                }
            }
        }
    }
    assert_eq!(combos.len(), 24);
    combos
}

/// Tiny deterministic PRNG (xorshift64). Every combination gets a fixed seed
/// derived from its index, so generated programs are reproducible.
struct Rng(u64);

impl Rng {
    fn new(seed: u64) -> Self {
        Rng(seed | 1)
    }

    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }

    /// Pick a value in `[lo, hi]`.
    fn between(&mut self, lo: u64, hi: u64) -> u64 {
        lo + self.next() % (hi - lo + 1)
    }
}

const LINE_BASE: i64 = -3;
const LINE_RANGE: u64 = 10;
const OPCODE_BASE: u8 = 13;
const ADDRESS_SIZE: u8 = 4;

/// Build the adjusted special opcode for a wanted line increment / operation
/// advance, keeping inside the representable special opcode window.
fn special_adjusted(line_inc: i64, operation_advance: u64) -> u8 {
    let line_index = (line_inc - LINE_BASE) as u64;
    assert!(line_index < LINE_RANGE);
    let adjusted = operation_advance * LINE_RANGE + line_index;
    assert!(adjusted <= 255 - u64::from(OPCODE_BASE));
    adjusted as u8
}

fn special(line_inc: i64, operation_advance: u64) -> ModelInstr {
    ModelInstr::Special(OPCODE_BASE + special_adjusted(line_inc, operation_advance))
}

/// Plan the instruction stream for one combination. Both sequences contain
/// more than twenty ordinary rows and exercise every required opcode.
fn plan(combo: &Combo) -> Vec<ModelInstr> {
    let mut rng = Rng::new(0x9e37_79b9_7f4a_7c15 ^ (combo.index as u64).wrapping_mul(0x100_0000_01b3));
    let m = u64::from(combo.max_ops);
    let mut p = Vec::new();

    // ---- Sequence 1, anchored at a low address ---------------------------
    p.push(ModelInstr::SetAddress(0x0100));

    p.extend_from_slice(&[ModelInstr::SetFile(1), ModelInstr::SetColumn(7), ModelInstr::Copy]);
    // Consecutive rows at the same machine address but different operation
    // slots (on VLIW combinations), then a rollover that carries the old slot.
    p.extend_from_slice(&[ModelInstr::AdvancePc(1), ModelInstr::Copy]);
    p.extend_from_slice(&[ModelInstr::AdvancePc(1), ModelInstr::Copy]);
    p.extend_from_slice(&[ModelInstr::AdvancePc(1), ModelInstr::Copy]);
    // Line zero is exposed as None through the public API.
    p.extend_from_slice(&[
        ModelInstr::SetFile(0),
        ModelInstr::AdvanceLine(-1),
        ModelInstr::SetColumn(3),
        ModelInstr::Copy,
    ]);
    p.extend_from_slice(&[ModelInstr::AdvanceLine(1), special(0, 1)]);
    // Interleaved persistent and transient registers, then a plain copy that
    // must only clear the transient state.
    p.extend_from_slice(&[
        ModelInstr::NegateStmt,
        ModelInstr::SetIsa(2),
        ModelInstr::SetDiscriminator(5),
        ModelInstr::SetBasicBlock,
        ModelInstr::Copy,
    ]);
    p.push(ModelInstr::Copy);
    p.extend_from_slice(&[
        ModelInstr::NegateStmt,
        ModelInstr::SetPrologueEnd,
        special(1, 0),
    ]);
    p.extend_from_slice(&[ModelInstr::SetEpilogueBegin, ModelInstr::Copy]);
    p.push(ModelInstr::Copy);
    // const_add_pc paired with a special, as a producer would emit it.
    p.extend_from_slice(&[ModelInstr::ConstAddPc, special(-1, 0)]);
    p.extend_from_slice(&[
        ModelInstr::SetColumn(9),
        ModelInstr::SetFile(1),
        ModelInstr::Copy,
    ]);
    // Walk up to the last slot of an instruction, then carry over the edge.
    let to_last_slot = if m == 1 { 2 } else { m - 1 };
    p.extend_from_slice(&[ModelInstr::AdvancePc(to_last_slot), ModelInstr::Copy]);
    p.extend_from_slice(&[ModelInstr::AdvancePc(1), ModelInstr::Copy]);
    // Fixed byte advance with a live non-zero slot: the slot must reset.
    let fixed_a = rng.between(3, 9) as u16;
    p.extend_from_slice(&[
        ModelInstr::AdvancePc(if m == 1 { 1 } else { 1 }),
        ModelInstr::FixedAdvance(fixed_a),
        ModelInstr::Copy,
    ]);
    p.extend_from_slice(&[ModelInstr::AdvancePc(1), ModelInstr::Copy]);
    p.extend_from_slice(&[ModelInstr::AdvancePc(1), ModelInstr::Copy]);
    // DWARF 4 defines this file at the tail of the first sequence; DWARF 5
    // declared it in the header instead, so no program opcode is emitted there.
    if combo.version <= 4 {
        p.extend_from_slice(&[ModelInstr::DefineFile(b"extra.inc".to_vec())]);
    }
    p.extend_from_slice(&[
        ModelInstr::SetFile(2),
        ModelInstr::SetColumn(11),
        ModelInstr::SetIsa(7),
        ModelInstr::SetDiscriminator(13),
        ModelInstr::Copy,
    ]);
    p.push(special(2, 2));
    p.extend_from_slice(&[ModelInstr::SetDiscriminator(1), ModelInstr::Copy]);
    p.extend_from_slice(&[
        ModelInstr::AdvanceLine(3),
        special(0, 1),
        ModelInstr::EndSequence,
    ]);

    // ---- Sequence 2 restarts at a completely different address -----------
    p.push(ModelInstr::SetAddress(0x8000));

    p.extend_from_slice(&[ModelInstr::SetFile(2), ModelInstr::SetColumn(4), ModelInstr::Copy]);
    p.extend_from_slice(&[ModelInstr::AdvancePc(1), ModelInstr::Copy]);
    p.extend_from_slice(&[ModelInstr::AdvancePc(2), ModelInstr::Copy]);
    p.extend_from_slice(&[
        ModelInstr::SetFile(0),
        ModelInstr::SetColumn(0),
        ModelInstr::AdvanceLine(-1),
        ModelInstr::Copy,
    ]);
    p.extend_from_slice(&[ModelInstr::AdvanceLine(4), special(0, 1)]);
    p.extend_from_slice(&[
        ModelInstr::SetDiscriminator(21),
        ModelInstr::SetBasicBlock,
        ModelInstr::SetPrologueEnd,
        ModelInstr::Copy,
    ]);
    p.push(special(1, 0));
    p.extend_from_slice(&[
        ModelInstr::SetEpilogueBegin,
        ModelInstr::SetIsa(3),
        ModelInstr::Copy,
    ]);
    p.push(ModelInstr::Copy);
    p.extend_from_slice(&[ModelInstr::ConstAddPc, ModelInstr::Copy]);
    // Another fixed byte advance, then a carry from the slot it created.
    let fixed_b = rng.between(5, 11) as u16;
    p.extend_from_slice(&[
        ModelInstr::AdvancePc(1),
        ModelInstr::FixedAdvance(fixed_b),
        ModelInstr::Copy,
    ]);
    p.extend_from_slice(&[ModelInstr::AdvancePc(1), ModelInstr::Copy]);
    p.extend_from_slice(&[ModelInstr::AdvancePc(to_last_slot), ModelInstr::Copy]);
    p.extend_from_slice(&[ModelInstr::NegateStmt, ModelInstr::Copy]);
    p.extend_from_slice(&[ModelInstr::NegateStmt, special(-2, 1)]);
    p.extend_from_slice(&[
        ModelInstr::SetFile(1),
        ModelInstr::SetColumn(13),
        ModelInstr::AdvanceLine(2),
        ModelInstr::Copy,
    ]);
    p.extend_from_slice(&[ModelInstr::AdvancePc(3), special(0, 0)]);
    p.extend_from_slice(&[ModelInstr::SetDiscriminator(9), ModelInstr::Copy]);
    p.push(ModelInstr::Copy);
    p.extend_from_slice(&[ModelInstr::AdvanceLine(1), special(1, 2)]);
    p.extend_from_slice(&[ModelInstr::SetColumn(5), ModelInstr::Copy]);
    p.push(ModelInstr::EndSequence);

    p
}

/// Everything the test needs for one combination: the raw section bytes, the
/// compilation file name (DWARF 4 only) and the files present in the header.
pub(crate) struct BuiltProgram {
    pub bytes: Vec<u8>,
    pub comp_name: Option<Vec<u8>>,
    pub initial_files: Vec<Vec<u8>>,
}

struct Encoder {
    out: Vec<u8>,
    endian: Endian,
    address_size: u8,
}

impl Encoder {
    fn u16(&mut self, value: u16) {
        let bytes = match self.endian {
            Endian::Little => value.to_le_bytes(),
            Endian::Big => value.to_be_bytes(),
        };
        self.out.extend_from_slice(&bytes);
    }

    fn u32(&mut self, value: u32) {
        let bytes = match self.endian {
            Endian::Little => value.to_le_bytes(),
            Endian::Big => value.to_be_bytes(),
        };
        self.out.extend_from_slice(&bytes);
    }

    fn u64(&mut self, value: u64) {
        let bytes = match self.endian {
            Endian::Little => value.to_le_bytes(),
            Endian::Big => value.to_be_bytes(),
        };
        self.out.extend_from_slice(&bytes);
    }

    fn address(&mut self, value: u64) {
        match self.address_size {
            4 => self.u32(value as u32),
            8 => self.u64(value),
            other => panic!("unsupported address size {other}"),
        }
    }

    fn extended(&mut self, sub_opcode: u8, body: impl FnOnce(&mut Encoder)) {
        self.out.push(0);
        // Reserve one byte for the length; all extended bodies used here fit a
        // single LEB128 byte.
        let marker = self.out.len();
        self.out.push(0);
        let body_start = self.out.len();
        self.out.push(sub_opcode);
        body(self);
        // The length covers the sub-opcode and its operands.
        let length = (self.out.len() - body_start) as u8;
        assert!(length < 0x80);
        self.out[marker] = length;
    }

    fn instr(&mut self, instr: &ModelInstr, version: u16) {
        match *instr {
            ModelInstr::SetAddress(addr) => {
                self.extended(constants::DW_LNE_set_address.0, |e| e.address(addr));
            }
            ModelInstr::EndSequence => {
                self.out.push(0);
                self.out.push(1);
                self.out.push(constants::DW_LNE_end_sequence.0);
            }
            ModelInstr::Copy => self.out.push(constants::DW_LNS_copy.0),
            ModelInstr::Special(opcode) => self.out.push(opcode),
            ModelInstr::AdvancePc(advance) => {
                self.out.push(constants::DW_LNS_advance_pc.0);
                uleb128(advance, &mut self.out);
            }
            ModelInstr::AdvanceLine(delta) => {
                self.out.push(constants::DW_LNS_advance_line.0);
                sleb128(delta, &mut self.out);
            }
            ModelInstr::SetFile(file) => {
                self.out.push(constants::DW_LNS_set_file.0);
                uleb128(file, &mut self.out);
            }
            ModelInstr::SetColumn(column) => {
                self.out.push(constants::DW_LNS_set_column.0);
                uleb128(column, &mut self.out);
            }
            ModelInstr::NegateStmt => self.out.push(constants::DW_LNS_negate_stmt.0),
            ModelInstr::SetBasicBlock => self.out.push(constants::DW_LNS_set_basic_block.0),
            ModelInstr::ConstAddPc => self.out.push(constants::DW_LNS_const_add_pc.0),
            ModelInstr::FixedAdvance(operand) => {
                self.out.push(constants::DW_LNS_fixed_advance_pc.0);
                self.u16(operand);
            }
            ModelInstr::SetPrologueEnd => {
                self.out.push(constants::DW_LNS_set_prologue_end.0)
            }
            ModelInstr::SetEpilogueBegin => {
                self.out.push(constants::DW_LNS_set_epilogue_begin.0)
            }
            ModelInstr::SetIsa(isa) => {
                self.out.push(constants::DW_LNS_set_isa.0);
                uleb128(isa, &mut self.out);
            }
            ModelInstr::SetDiscriminator(discriminator) => {
                self.extended(constants::DW_LNE_set_discriminator.0, |e| {
                    uleb128(discriminator, &mut e.out)
                });
            }
            ModelInstr::DefineFile(ref name) => {
                assert!(version <= 4, "DWARF 5 files live in the header");
                self.extended(constants::DW_LNE_define_file.0, |e| {
                    e.out.extend_from_slice(name);
                    e.out.push(0);
                    uleb128(0, &mut e.out);
                    uleb128(0, &mut e.out);
                    uleb128(0, &mut e.out);
                });
            }
        }
    }
}

/// Standard opcode LEB128 operand counts for opcodes 1 ..= 12.
fn standard_opcode_lengths() -> [u8; 12] {
    [
        0, // copy
        1, // advance_pc
        1, // advance_line
        1, // set_file
        1, // set_column
        0, // negate_stmt
        0, // set_basic_block
        0, // const_add_pc
        0, // fixed_advance_pc (raw uhalf, not LEB128)
        0, // set_prologue_end
        0, // set_epilogue_begin
        1, // set_isa
    ]
}

/// Encode one full `.debug_line` unit and return the section bytes.
pub(crate) fn encode(combo: &Combo, instrs: &[ModelInstr]) -> BuiltProgram {
    // Header payload (everything after the header-length field) and program
    // bytes are built separately so the lengths can be backfilled.
    let mut header_payload = Vec::new();
    header_payload.push(combo.min_insn_len);
    header_payload.push(combo.max_ops);
    header_payload.push(1); // default_is_stmt
    header_payload.push(LINE_BASE as u8);
    header_payload.push(LINE_RANGE as u8);
    header_payload.push(OPCODE_BASE);
    header_payload.extend_from_slice(&standard_opcode_lengths());

    // Files known before the program runs.
    let comp_name = b"main.rs".to_vec();
    let initial_files: Vec<Vec<u8>> = if combo.version <= 4 {
        // v4: the compilation file is supplied out-of-band; only lib.rs sits
        // in the header table, extra.inc arrives via define_file later.
        header_payload.push(0); // empty include directory list
        header_payload.extend_from_slice(b"lib.rs\0");
        uleb128(0, &mut header_payload); // directory index
        uleb128(0, &mut header_payload); // timestamp
        uleb128(0, &mut header_payload); // size
        header_payload.push(0); // end of file names
        vec![b"lib.rs".to_vec()]
    } else {
        // One directory entry format: path as an inline string.
        header_payload.push(1);
        uleb128(u64::from(constants::DW_LNCT_path.0), &mut header_payload);
        uleb128(u64::from(constants::DW_FORM_string.0), &mut header_payload);
        uleb128(1, &mut header_payload); // directory count
        header_payload.extend_from_slice(b"dir\0");

        // File entry formats: inline path plus a uleb directory index.
        header_payload.push(2);
        uleb128(u64::from(constants::DW_LNCT_path.0), &mut header_payload);
        uleb128(u64::from(constants::DW_FORM_string.0), &mut header_payload);
        uleb128(u64::from(constants::DW_LNCT_directory_index.0), &mut header_payload);
        uleb128(u64::from(constants::DW_FORM_udata.0), &mut header_payload);
        uleb128(3, &mut header_payload); // file count
        for name in [comp_name.as_slice(), b"lib.rs".as_slice(), b"extra.inc".as_slice()] {
            header_payload.extend_from_slice(name);
            header_payload.push(0);
            uleb128(0, &mut header_payload);
        }
        vec![comp_name.clone(), b"lib.rs".to_vec(), b"extra.inc".to_vec()]
    };

    let mut encoder = Encoder {
        out: Vec::new(),
        endian: combo.endian,
        address_size: ADDRESS_SIZE,
    };
    for instr in instrs {
        encoder.instr(instr, combo.version);
    }
    let program = encoder.out;

    // Assemble the version..end body, then wrap it in the initial length.
    let mut body = Vec::new();
    match combo.endian {
        Endian::Little => body.extend_from_slice(&combo.version.to_le_bytes()),
        Endian::Big => body.extend_from_slice(&combo.version.to_be_bytes()),
    }
    if combo.version >= 5 {
        body.push(ADDRESS_SIZE);
        body.push(0); // segment selector size
    }
    let header_length = header_payload.len() as u64;
    match combo.format {
        DwarfFormat::Dwarf32 => {
            let len = header_length as u32;
            match combo.endian {
                Endian::Little => body.extend_from_slice(&len.to_le_bytes()),
                Endian::Big => body.extend_from_slice(&len.to_be_bytes()),
            }
        }
        DwarfFormat::Dwarf64 => {
            match combo.endian {
                Endian::Little => body.extend_from_slice(&header_length.to_le_bytes()),
                Endian::Big => body.extend_from_slice(&header_length.to_be_bytes()),
            }
        }
    }
    body.extend_from_slice(&header_payload);
    body.extend_from_slice(&program);

    let mut section = Vec::new();
    match combo.format {
        DwarfFormat::Dwarf32 => {
            let unit_length = body.len() as u32;
            match combo.endian {
                Endian::Little => section.extend_from_slice(&unit_length.to_le_bytes()),
                Endian::Big => section.extend_from_slice(&unit_length.to_be_bytes()),
            }
        }
        DwarfFormat::Dwarf64 => {
            section.extend_from_slice(&[0xff, 0xff, 0xff, 0xff]);
            let unit_length = body.len() as u64;
            match combo.endian {
                Endian::Little => section.extend_from_slice(&unit_length.to_le_bytes()),
                Endian::Big => section.extend_from_slice(&unit_length.to_be_bytes()),
            }
        }
    }
    section.extend_from_slice(&body);

    BuiltProgram {
        bytes: section,
        comp_name: if combo.version <= 4 {
            Some(comp_name)
        } else {
            None
        },
        initial_files,
    }
}

/// Build bytes and the instruction plan together.
pub(crate) fn build(
    combo: &Combo,
) -> (Vec<u8>, Option<Vec<u8>>, Vec<Vec<u8>>, Vec<ModelInstr>) {
    let instrs = plan(combo);
    let built = encode(combo, &instrs);
    (built.bytes, built.comp_name, built.initial_files, instrs)
}

/// What kind of rejection a corrupted unit is expected to produce.
#[derive(Clone, Copy, Debug)]
pub(crate) enum CorruptKind {
    /// A header field that must be non-zero was encoded as zero.
    HeaderZero,
    /// The unit ended in the middle of an encoded operand.
    Truncated,
}

/// One corrupted unit plus the section that follows it with a healthy
/// neighbour unit.
pub(crate) struct CorruptCase {
    pub name: &'static str,
    /// Just the corrupted unit's bytes.
    pub corrupt: Vec<u8>,
    /// Corrupted unit immediately followed by a well-formed unit.
    pub section: Vec<u8>,
    pub expected: CorruptKind,
}

/// Assemble a minimal DWARF 4 / DWARF32 / little-endian unit around a custom
/// header payload and program. The length fields are backfilled.
fn minimal_unit(
    max_ops: u8,
    line_range: u8,
    opcode_base: u8,
    std_lengths: &[u8],
    program: &[u8],
) -> Vec<u8> {
    let mut header_payload = Vec::new();
    header_payload.push(1); // minimum instruction length
    header_payload.push(max_ops);
    header_payload.push(1); // default is_stmt
    header_payload.push(0); // line base
    header_payload.push(line_range);
    header_payload.push(opcode_base);
    header_payload.extend_from_slice(std_lengths);
    header_payload.push(0); // end of include directories
    header_payload.push(0); // end of file names

    let mut body = Vec::new();
    body.extend_from_slice(&4u16.to_le_bytes()); // version
    body.extend_from_slice(&(header_payload.len() as u32).to_le_bytes());
    body.extend_from_slice(&header_payload);
    body.extend_from_slice(program);

    let mut unit = Vec::new();
    unit.extend_from_slice(&(body.len() as u32).to_le_bytes());
    unit.extend_from_slice(&body);
    unit
}

/// A short, fully valid neighbour unit: one set_address, a copy, end_sequence.
fn good_neighbour() -> Vec<u8> {
    let program = [
        0x00, 0x05, constants::DW_LNE_set_address.0, // set_address 0x200
        0x00, 0x02, 0x00, 0x00,
        constants::DW_LNS_copy.0, // copy
        0x00, 0x01, constants::DW_LNE_end_sequence.0, // end_sequence
    ];
    minimal_unit(1, 1, 3, &[0, 1], &program)
}

/// Build the independent corrupted inputs. Every unit's length field bounds
/// the reader, so truncation can never borrow the neighbour's bytes.
pub(crate) fn corrupt_units() -> Vec<CorruptCase> {
    let neighbour = good_neighbour();
    let mut cases = Vec::new();

    let make = |name: &'static str, corrupt: Vec<u8>, expected: CorruptKind| {
        let mut section = corrupt.clone();
        section.extend_from_slice(&neighbour);
        CorruptCase {
            name,
            corrupt,
            section,
            expected,
        }
    };

    // 1. maximum_operations_per_instruction == 0 must be rejected at the header.
    let zero_mop = minimal_unit(0, 1, 3, &[0, 1], &[0x00, 0x01, 0x01]);
    cases.push(make("zero_max_operations", zero_mop, CorruptKind::HeaderZero));

    // 2. line_range == 0 must be rejected at the header.
    let zero_range = minimal_unit(1, 0, 3, &[0, 1], &[0x00, 0x01, 0x01]);
    cases.push(make("zero_line_range", zero_range, CorruptKind::HeaderZero));

    // 3. A truncated unsigned LEB128 operand to advance_pc: the only byte has
    //    its continuation bit set and the unit ends immediately.
    let truncated_leb = minimal_unit(1, 1, 3, &[0, 1], &[
        constants::DW_LNS_advance_pc.0,
        0x80, // continuation bit set, no following byte inside this unit
    ]);
    cases.push(make("truncated_leb128", truncated_leb, CorruptKind::Truncated));

    // 4. A truncated set_address: the extended opcode declares a four-byte
    //    address but only two bytes remain before the unit boundary.
    let truncated_addr = minimal_unit(1, 1, 3, &[0, 1], &[
        0x00,
        0x05, // length covers sub-opcode + four address bytes
        constants::DW_LNE_set_address.0,
        0x11,
        0x22, // only two of the four promised address bytes are present
    ]);
    cases.push(make("truncated_set_address", truncated_addr, CorruptKind::Truncated));

    cases
}
