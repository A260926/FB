//! Independent regression for the DWARF line number program read path.
//!
//! The bytes fed to gimli are generated entirely inside this harness, and the
//! expected matrix is produced by a free-standing state machine in the `ir`
//! module. That model never calls `LineRow::execute`, never uses the gimli
//! writer and never derives expectations from rows gimli produced.
//!
//! There is exactly one `#[test]` entry point, `line_contract`. Everything
//! else in this file is harness-only support code.

// ---------------------------------------------------------------------------
// Deterministic pseudo random source. Fixed seed, no platform randomness.
// ---------------------------------------------------------------------------

mod rng {
    pub struct Rng(u64);

    impl Rng {
        pub fn new(seed: u64) -> Self {
            Rng(seed)
        }

        pub fn next_u64(&mut self) -> u64 {
            self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
            let mut z = self.0;
            z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
            z ^ (z >> 31)
        }

        pub fn below(&mut self, bound: u64) -> u64 {
            assert!(bound > 0);
            self.next_u64() % bound
        }
    }
}

// ---------------------------------------------------------------------------
// Instruction IR. The generator emits this, the encoder serialises it and the
// reference model interprets it independently of gimli's parser.
// ---------------------------------------------------------------------------

mod ir {
    #[derive(Clone, Debug)]
    pub enum Insn {
        SetAddress(u64),
        AdvancePc(u64),
        AdvanceLine(i64),
        SetFile(u64),
        SetColumn(u64),
        NegateStmt,
        SetBasicBlock,
        ConstAddPc,
        FixedAdvancePc(u16),
        SetPrologueEnd,
        SetEpilogueBegin,
        SetIsa(u64),
        Copy,
        EndSequence,
        DefineFile(Vec<u8>),
        SetDiscriminator(u64),
        Special(u8),
    }

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub enum Kind {
        AdvancePc,
        Special,
        ConstAddPc,
        FixedAdvancePc,
        SetAddress,
        Copy,
        EndSequence,
    }

    impl Insn {
        pub fn kind(&self) -> Option<Kind> {
            Some(match self {
                Insn::AdvancePc(..) => Kind::AdvancePc,
                Insn::Special(..) => Kind::Special,
                Insn::ConstAddPc => Kind::ConstAddPc,
                Insn::FixedAdvancePc(..) => Kind::FixedAdvancePc,
                Insn::SetAddress(..) => Kind::SetAddress,
                Insn::Copy => Kind::Copy,
                Insn::EndSequence => Kind::EndSequence,
                _ => return None,
            })
        }
    }

    /// One expected matrix row, as computed by the reference model.
    #[derive(Clone, Debug, PartialEq, Eq)]
    pub struct ExpectedRow {
        pub address: u64,
        pub op_index: u64,
        pub file: u64,
        pub line: u64,
        pub column: u64,
        pub is_stmt: bool,
        pub isa: u64,
        pub discriminator: u64,
        pub basic_block: bool,
        pub prologue_end: bool,
        pub epilogue_begin: bool,
        pub end_sequence: bool,
    }

    pub struct ExpectedSequence {
        pub rows: Vec<ExpectedRow>,
    }

    /// Free-standing DWARF 6.2.5 state machine.
    ///
    /// The operation slot is advanced one slot at a time; byte displacement is
    /// accounted for separately when the slot counter wraps. There is no
    /// division or remainder in the address/op_index carry logic on purpose.
    pub struct ReferenceModel {
        pub files: Vec<Vec<u8>>,
        max_ops: u64,
        min_insn_len: u64,
        line_range: u64,
        line_base: i64,
        opcode_base: u64,
        default_is_stmt: bool,

        address: u64,
        op_index: u64,
        file: u64,
        line: u64,
        column: u64,
        is_stmt: bool,
        isa: u64,
        discriminator: u64,
        basic_block: bool,
        prologue_end: bool,
        epilogue_begin: bool,
    }

    impl ReferenceModel {
        pub fn new(
            files: Vec<Vec<u8>>,
            max_ops: u8,
            min_insn_len: u8,
            line_range: u8,
            line_base: i8,
            opcode_base: u8,
            default_is_stmt: bool,
        ) -> Self {
            ReferenceModel {
                files,
                max_ops: max_ops as u64,
                min_insn_len: min_insn_len as u64,
                line_range: line_range as u64,
                line_base: line_base as i64,
                opcode_base: opcode_base as u64,
                default_is_stmt,
                address: 0,
                op_index: 0,
                file: 1,
                line: 1,
                column: 0,
                is_stmt: default_is_stmt,
                isa: 0,
                discriminator: 0,
                basic_block: false,
                prologue_end: false,
                epilogue_begin: false,
            }
        }

        /// Restore the section 6.2.2 initial values, as done at the start of
        /// every sequence and after DW_LNE_end_sequence.
        fn reset_to_initial(&mut self) {
            self.address = 0;
            self.op_index = 0;
            self.file = 1;
            self.line = 1;
            self.column = 0;
            self.is_stmt = self.default_is_stmt;
            self.isa = 0;
            self.discriminator = 0;
            self.basic_block = false;
            self.prologue_end = false;
            self.epilogue_begin = false;
        }

        /// Clear only the transient flags after an ordinary row (special or
        /// copy). The persistent registers survive.
        fn reset_transient(&mut self) {
            self.discriminator = 0;
            self.basic_block = false;
            self.prologue_end = false;
            self.epilogue_begin = false;
        }

        /// Push the operation pointer forward one slot at a time. Every time
        /// the slot counter reaches the per-instruction width it wraps to
        /// slot zero and the byte address gains one minimum instruction.
        fn advance_slots(&mut self, mut operations: u64) {
            while operations > 0 {
                operations -= 1;
                self.op_index += 1;
                if self.op_index == self.max_ops {
                    self.op_index = 0;
                    self.address = self.address.wrapping_add(self.min_insn_len);
                }
            }
        }

        fn apply_line_delta(&mut self, delta: i64) {
            if delta < 0 {
                let decrement = (-delta) as u64;
                self.line = self.line.saturating_sub(decrement);
            } else {
                self.line = self.line.wrapping_add(delta as u64);
            }
        }

        fn snapshot(&self, end_sequence: bool) -> ExpectedRow {
            ExpectedRow {
                address: self.address,
                op_index: self.op_index,
                file: self.file,
                line: self.line,
                column: self.column,
                is_stmt: self.is_stmt,
                isa: self.isa,
                discriminator: self.discriminator,
                basic_block: self.basic_block,
                prologue_end: self.prologue_end,
                epilogue_begin: self.epilogue_begin,
                end_sequence,
            }
        }

        /// Run the IR and return one entry per sequence (each including the
        /// end-sequence row) plus the final file table.
        pub fn run(mut self, program: &[Insn]) -> (Vec<ExpectedSequence>, Vec<Vec<u8>>) {
            let mut sequences = Vec::new();
            let mut rows = Vec::new();

            for insn in program {
                match insn {
                    Insn::SetAddress(addr) => {
                        self.address = *addr;
                        self.op_index = 0;
                    }
                    Insn::AdvancePc(advance) => self.advance_slots(*advance),
                    Insn::AdvanceLine(delta) => self.apply_line_delta(*delta),
                    Insn::SetFile(file) => self.file = *file,
                    Insn::SetColumn(column) => self.column = *column,
                    Insn::NegateStmt => self.is_stmt = !self.is_stmt,
                    Insn::SetBasicBlock => self.basic_block = true,
                    Insn::ConstAddPc => {
                        // Operation advance implied by special opcode 255.
                        let adjusted = 255u64 - self.opcode_base;
                        self.advance_slots(adjusted / self.line_range);
                    }
                    Insn::FixedAdvancePc(operand) => {
                        // Raw byte displacement; the slot is always cleared.
                        self.address = self.address.wrapping_add(*operand as u64);
                        self.op_index = 0;
                    }
                    Insn::SetPrologueEnd => self.prologue_end = true,
                    Insn::SetEpilogueBegin => self.epilogue_begin = true,
                    Insn::SetIsa(isa) => self.isa = *isa,
                    Insn::SetDiscriminator(discriminator) => {
                        self.discriminator = *discriminator
                    }
                    Insn::DefineFile(name) => self.files.push(name.clone()),
                    Insn::Special(opcode) => {
                        let adjusted = u64::from(*opcode) - self.opcode_base;
                        let line_delta =
                            self.line_base + (adjusted % self.line_range) as i64;
                        let operation_advance = adjusted / self.line_range;
                        self.apply_line_delta(line_delta);
                        self.advance_slots(operation_advance);
                        rows.push(self.snapshot(false));
                        self.reset_transient();
                    }
                    Insn::Copy => {
                        rows.push(self.snapshot(false));
                        self.reset_transient();
                    }
                    Insn::EndSequence => {
                        rows.push(self.snapshot(true));
                        sequences.push(ExpectedSequence {
                            rows: core::mem::take(&mut rows),
                        });
                        self.reset_to_initial();
                    }
                }
            }
            assert!(rows.is_empty(), "program must end every sequence");
            (sequences, self.files)
        }
    }
}

// ---------------------------------------------------------------------------
// Byte generation. Pure Vec<u8> construction with explicit endianness and
// DWARF format handling; no gimli writer is involved.
// ---------------------------------------------------------------------------

mod encode {
    pub struct ByteWriter {
        pub buf: Vec<u8>,
        big_endian: bool,
    }

    impl ByteWriter {
        pub fn new(big_endian: bool) -> Self {
            ByteWriter {
                buf: Vec::new(),
                big_endian,
            }
        }

        pub fn u8(&mut self, value: u8) {
            self.buf.push(value);
        }

        pub fn u16(&mut self, value: u16) {
            let bytes = if self.big_endian {
                value.to_be_bytes()
            } else {
                value.to_le_bytes()
            };
            self.buf.extend_from_slice(&bytes);
        }

        pub fn u32(&mut self, value: u32) {
            let bytes = if self.big_endian {
                value.to_be_bytes()
            } else {
                value.to_le_bytes()
            };
            self.buf.extend_from_slice(&bytes);
        }

        pub fn u64(&mut self, value: u64) {
            let bytes = if self.big_endian {
                value.to_be_bytes()
            } else {
                value.to_le_bytes()
            };
            self.buf.extend_from_slice(&bytes);
        }

        pub fn bytes(&mut self, value: &[u8]) {
            self.buf.extend_from_slice(value);
        }

        pub fn string0(&mut self, value: &[u8]) {
            self.buf.extend_from_slice(value);
            self.buf.push(0);
        }

        pub fn uleb(&mut self, mut value: u64) {
            loop {
                let mut byte = (value & 0x7f) as u8;
                value >>= 7;
                if value != 0 {
                    byte |= 0x80;
                }
                self.buf.push(byte);
                if value == 0 {
                    break;
                }
            }
        }

        pub fn sleb(&mut self, mut value: i64) {
            loop {
                let byte = (value & 0x7f) as u8;
                let sign_bit_set = byte & 0x40 != 0;
                value >>= 7;
                if (value == 0 && !sign_bit_set) || (value == -1 && sign_bit_set) {
                    self.buf.push(byte);
                    break;
                } else {
                    self.buf.push(byte | 0x80);
                }
            }
        }
    }

    pub const OPCODE_BASE: u8 = 13;
    pub const LINE_BASE: i8 = -3;
    pub const LINE_RANGE: u8 = 12;
    pub const STD_OPCODE_LENGTHS: [u8; 12] =
        [0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1];

    // Standard opcodes.
    pub const DW_LNS_COPY: u8 = 0x01;
    pub const DW_LNS_ADVANCE_PC: u8 = 0x02;
    pub const DW_LNS_ADVANCE_LINE: u8 = 0x03;
    pub const DW_LNS_SET_FILE: u8 = 0x04;
    pub const DW_LNS_SET_COLUMN: u8 = 0x05;
    pub const DW_LNS_NEGATE_STMT: u8 = 0x06;
    pub const DW_LNS_SET_BASIC_BLOCK: u8 = 0x07;
    pub const DW_LNS_CONST_ADD_PC: u8 = 0x08;
    pub const DW_LNS_FIXED_ADVANCE_PC: u8 = 0x09;
    pub const DW_LNS_SET_PROLOGUE_END: u8 = 0x0a;
    pub const DW_LNS_SET_EPILOGUE_BEGIN: u8 = 0x0b;
    pub const DW_LNS_SET_ISA: u8 = 0x0c;

    // Extended opcodes.
    pub const DW_LNE_END_SEQUENCE: u8 = 0x01;
    pub const DW_LNE_SET_ADDRESS: u8 = 0x02;
    pub const DW_LNE_DEFINE_FILE: u8 = 0x03;
    pub const DW_LNE_SET_DISCRIMINATOR: u8 = 0x04;

    pub const DW_LNCT_PATH: u64 = 1;
    pub const DW_LNCT_DIRECTORY_INDEX: u64 = 2;
    pub const DW_FORM_STRING: u64 = 0x08;
    pub const DW_FORM_DATA1: u64 = 0x0b;

    #[derive(Clone, Copy, Debug)]
    pub struct HeaderParams {
        pub version: u16,
        pub dwarf64: bool,
        pub big_endian: bool,
        pub address_size: u8,
        pub min_insn_len: u8,
        pub max_ops: u8,
        pub line_base: i8,
        pub line_range: u8,
    }

    fn extended(w: &mut ByteWriter, opcode: u8, payload: &[u8]) {
        w.u8(0);
        w.uleb((payload.len() + 1) as u64);
        w.u8(opcode);
        w.bytes(payload);
    }

    /// Serialise the line program IR.
    pub fn encode_program(insns: &[super::ir::Insn], big_endian: bool) -> Vec<u8> {
        let mut w = ByteWriter::new(big_endian);
        for insn in insns {
            use super::ir::Insn::*;
            match insn {
                Copy => w.u8(DW_LNS_COPY),
                AdvancePc(advance) => {
                    w.u8(DW_LNS_ADVANCE_PC);
                    w.uleb(*advance);
                }
                AdvanceLine(delta) => {
                    w.u8(DW_LNS_ADVANCE_LINE);
                    w.sleb(*delta);
                }
                SetFile(file) => {
                    w.u8(DW_LNS_SET_FILE);
                    w.uleb(*file);
                }
                SetColumn(column) => {
                    w.u8(DW_LNS_SET_COLUMN);
                    w.uleb(*column);
                }
                NegateStmt => w.u8(DW_LNS_NEGATE_STMT),
                SetBasicBlock => w.u8(DW_LNS_SET_BASIC_BLOCK),
                ConstAddPc => w.u8(DW_LNS_CONST_ADD_PC),
                FixedAdvancePc(operand) => {
                    w.u8(DW_LNS_FIXED_ADVANCE_PC);
                    w.u16(*operand);
                }
                SetPrologueEnd => w.u8(DW_LNS_SET_PROLOGUE_END),
                SetEpilogueBegin => w.u8(DW_LNS_SET_EPILOGUE_BEGIN),
                SetIsa(isa) => {
                    w.u8(DW_LNS_SET_ISA);
                    w.uleb(*isa);
                }
                EndSequence => extended(&mut w, DW_LNE_END_SEQUENCE, &[]),
                SetAddress(address) => {
                    let mut payload = ByteWriter::new(big_endian);
                    payload.u64(*address);
                    extended(&mut w, DW_LNE_SET_ADDRESS, &payload.buf);
                }
                DefineFile(name) => {
                    let mut payload = ByteWriter::new(big_endian);
                    payload.string0(name);
                    payload.uleb(0); // directory index
                    payload.uleb(0); // timestamp
                    payload.uleb(0); // size
                    extended(&mut w, DW_LNE_DEFINE_FILE, &payload.buf);
                }
                SetDiscriminator(discriminator) => {
                    let mut payload = ByteWriter::new(big_endian);
                    payload.uleb(*discriminator);
                    extended(&mut w, DW_LNE_SET_DISCRIMINATOR, &payload.buf);
                }
                Special(opcode) => w.u8(*opcode),
            }
        }
        w.buf
    }

    /// Build a complete .debug_line-style unit containing one line program.
    pub fn encode_unit(params: &HeaderParams, program: &[u8]) -> Vec<u8> {
        let big_endian = params.big_endian;
        let mut header_body = ByteWriter::new(big_endian);

        header_body.u8(params.min_insn_len);
        if params.version >= 4 {
            header_body.u8(params.max_ops);
        }
        header_body.u8(1); // default_is_stmt
        header_body.u8(params.line_base as u8);
        header_body.u8(params.line_range);
        header_body.u8(OPCODE_BASE);
        header_body.bytes(&STD_OPCODE_LENGTHS);

        if params.version <= 4 {
            // Empty include directory table.
            header_body.u8(0);
            // One header-declared file: hdr.c (file index 1; index 0 is the
            // compilation file supplied through the public API).
            header_body.string0(b"hdr.c");
            header_body.uleb(0);
            header_body.uleb(0);
            header_body.uleb(0);
            // End of file name table.
            header_body.u8(0);
        } else {
            // Directory entry format: one (path, string) descriptor.
            header_body.uleb(1);
            header_body.uleb(DW_LNCT_PATH);
            header_body.uleb(DW_FORM_STRING);
            // No directories.
            header_body.uleb(0);

            // File entry format: path string + directory index data1.
            header_body.uleb(2);
            header_body.uleb(DW_LNCT_PATH);
            header_body.uleb(DW_FORM_STRING);
            header_body.uleb(DW_LNCT_DIRECTORY_INDEX);
            header_body.uleb(DW_FORM_DATA1);
            // Two files declared up front, including the file that DWARF 4
            // adds through DW_LNE_define_file instead.
            header_body.uleb(2);
            header_body.string0(b"main.c");
            header_body.u8(0);
            header_body.string0(b"extra.c");
            header_body.u8(0);
        }

        let mut unit = ByteWriter::new(big_endian);
        unit.u16(params.version);
        if params.version >= 5 {
            unit.u8(params.address_size);
            unit.u8(0); // segment selector size
        }
        if params.dwarf64 {
            unit.u64(header_body.buf.len() as u64);
        } else {
            unit.u32(header_body.buf.len() as u32);
        }
        unit.bytes(&header_body.buf);
        unit.bytes(program);

        let mut out = ByteWriter::new(big_endian);
        if params.dwarf64 {
            out.u32(0xffff_ffff);
            out.u64(unit.buf.len() as u64);
        } else {
            out.u32(unit.buf.len() as u32);
        }
        out.bytes(&unit.buf);
        out.buf
    }
}

// ---------------------------------------------------------------------------
// Deterministic program generation. The same seed always yields the same IR,
// and the generated programs deliberately exercise slot carries, fixed byte
// advances that clear the slot and the two kinds of register reset.
// ---------------------------------------------------------------------------

// Note: `gen` is a reserved keyword in edition 2024, so this module is named
// `generator`.
mod generator {
    use super::encode::{LINE_BASE, LINE_RANGE, OPCODE_BASE};
    use super::ir::Insn;
    use super::rng::Rng;

    #[derive(Clone, Copy, Debug)]
    pub struct Combo {
        pub version: u16,
        pub dwarf64: bool,
        pub big_endian: bool,
        pub max_ops: u8,
        pub min_insn_len: u8,
    }

    impl Combo {
        pub fn label(&self) -> String {
            format!(
                "v{}/{}/{}/maxops{}/mil{}",
                self.version,
                if self.dwarf64 { "dw64" } else { "dw32" },
                if self.big_endian { "be" } else { "le" },
                self.max_ops,
                self.min_insn_len
            )
        }

        /// Index of the file added at the tail of sequence 1 in DWARF 4 and
        /// declared in the DWARF 5 header.
        pub fn extra_file_index(&self) -> u64 {
            if self.version <= 4 {
                2
            } else {
                1
            }
        }
    }

    fn special_opcode(rng: &mut Rng) -> u8 {
        OPCODE_BASE + (rng.below(256 - u64::from(OPCODE_BASE)) as u8)
    }

    fn emit_row(program: &mut Vec<Insn>, rng: &mut Rng) {
        if rng.below(2) == 0 {
            program.push(Insn::Copy);
        } else {
            program.push(Insn::Special(special_opcode(rng)));
        }
    }

    fn valid_row_count(program: &[Insn]) -> usize {
        program
            .iter()
            .filter(|insn| matches!(insn, Insn::Copy | Insn::Special(..)))
            .count()
    }

    /// Generate two sequences of at least 20 valid rows each plus their
    /// end-sequence rows.
    pub fn generate(combo: &Combo, seed: u64) -> Vec<Insn> {
        let mut rng = Rng::new(seed);
        let max_ops = u64::from(combo.max_ops);
        let mut program = Vec::new();

        for seqno in 0u64..2 {
            let start = 0x1000 + seqno * 0x4000 + rng.below(0x100);

            // Sequence opens with an absolute set_address at a fresh address.
            program.push(Insn::SetAddress(start));

            // P0: interleave persistent and transient registers, then copy.
            program.push(Insn::SetColumn(1 + rng.below(8)));
            program.push(Insn::SetIsa(rng.below(3)));
            if rng.below(2) == 0 {
                program.push(Insn::NegateStmt);
            }
            program.push(Insn::SetDiscriminator(1 + rng.below(50)));
            program.push(Insn::SetBasicBlock);
            program.push(Insn::Copy);

            // P1: consecutive rows at the same byte address but in different
            // operation slots. For maxops == 1 the slot always wraps, so the
            // address moves instead.
            if combo.max_ops > 1 {
                for _ in 1..combo.max_ops {
                    program.push(Insn::AdvancePc(1));
                    program.push(Insn::AdvanceLine(1));
                    program.push(Insn::Copy);
                }
            } else {
                program.push(Insn::AdvancePc(1));
                program.push(Insn::Copy);
            }

            // P2: advance while an old slot is still live, forcing a carry
            // across at least one instruction boundary.
            let carry = max_ops + rng.below(max_ops);
            program.push(Insn::AdvancePc(carry));
            program.push(Insn::SetDiscriminator(7));
            program.push(Insn::SetBasicBlock);
            program.push(Insn::Copy);

            // P3: const_add_pc followed by copy and special.
            program.push(Insn::ConstAddPc);
            program.push(Insn::Copy);
            program.push(Insn::Special(special_opcode(&mut rng)));

            // P4: fixed byte advance first clears the slot, then the slot
            // carries again from zero.
            program.push(Insn::AdvancePc(1));
            let fixed_operand = u16::from(combo.min_insn_len) * (1 + rng.below(3) as u16);
            program.push(Insn::FixedAdvancePc(fixed_operand));
            program.push(Insn::SetPrologueEnd);
            program.push(Insn::Copy);
            program.push(Insn::AdvancePc(max_ops));
            program.push(Insn::Copy);
            program.push(Insn::AdvancePc(1));
            program.push(Insn::SetEpilogueBegin);
            program.push(Insn::Copy);

            // P5: another absolute set_address restarts the slot at zero.
            program.push(Insn::SetAddress(start + 0x800));
            program.push(Insn::Copy);

            // P6: interleaved register traffic over eight rows. Includes a
            // line-zero row and lets each transient flag prove it is cleared
            // by the following ordinary row.
            for i in 0u64..8 {
                program.push(Insn::SetColumn(i * 3));
                program.push(Insn::SetIsa(i % 4));
                if i % 2 == 0 {
                    program.push(Insn::NegateStmt);
                }

                let file = if seqno == 1 && i % 3 == 0 {
                    combo.extra_file_index()
                } else {
                    i % 2
                };
                program.push(Insn::SetFile(file));

                if i == 2 {
                    // Large negative delta clamps the line register at zero.
                    program.push(Insn::AdvanceLine(-100_000));
                } else if i == 3 {
                    program.push(Insn::AdvanceLine(5));
                }

                match i {
                    1 => program.push(Insn::SetDiscriminator(11)),
                    4 => program.push(Insn::SetBasicBlock),
                    5 => program.push(Insn::SetPrologueEnd),
                    6 => program.push(Insn::SetEpilogueBegin),
                    _ => {}
                }

                if i == 2 {
                    // Emit the clamped line-zero row with a plain copy so the
                    // line register cannot move again before the row is taken.
                    program.push(Insn::Copy);
                } else {
                    emit_row(&mut program, &mut rng);
                }
            }

            // P7: pad with mixed specials and copies until the sequence has
            // comfortably more than 20 valid rows.
            while valid_row_count(&program) < (seqno as usize + 1) * 22 {
                if rng.below(2) == 0 {
                    program.push(Insn::AdvancePc(1 + rng.below(max_ops)));
                }
                emit_row(&mut program, &mut rng);
            }

            // Sequence 2 explicitly references the late-added / header-declared
            // file by emitting a row while the file register points at it.
            if seqno == 1 {
                program.push(Insn::SetFile(combo.extra_file_index()));
                program.push(Insn::Copy);
            }

            // DWARF 4 adds the extra file at the tail of the first sequence;
            // DWARF 5 declared it in the header instead.
            if combo.version <= 4 && seqno == 0 {
                program.push(Insn::DefineFile(b"extra.c".to_vec()));
            }

            program.push(Insn::EndSequence);
        }

        // Sanity: every opcode family the contract requires is present, and
        // each individual sequence carries at least 20 valid rows.
        let mut per_sequence = [0usize; 2];
        let mut seq = 0usize;
        for insn in &program {
            if let Insn::EndSequence = insn {
                seq += 1;
                continue;
            }
            if matches!(insn, Insn::Copy | Insn::Special(..)) {
                per_sequence[seq] += 1;
            }
        }
        assert!(per_sequence[0] >= 20 && per_sequence[1] >= 20);

        for required in [
            Insn::AdvancePc(0).kind().unwrap(),
            Insn::Special(OPCODE_BASE).kind().unwrap(),
            Insn::ConstAddPc.kind().unwrap(),
            Insn::FixedAdvancePc(0).kind().unwrap(),
            Insn::SetAddress(0).kind().unwrap(),
            Insn::Copy.kind().unwrap(),
            Insn::EndSequence.kind().unwrap(),
        ] {
            assert!(
                program.iter().any(|insn| insn.kind() == Some(required)),
                "generated program is missing {:?}",
                required
            );
        }

        // The encoding parameters must stay inside their header fields.
        assert!(LINE_RANGE == 12);
        assert!(LINE_BASE == -3);
        program
    }
}

// ---------------------------------------------------------------------------
// Observed row snapshot and comparison against the independent matrix.
// ---------------------------------------------------------------------------

mod checks {
    use super::encode::{HeaderParams, LINE_BASE, LINE_RANGE};
    use super::generator::Combo;
    use super::ir::{ExpectedRow, ExpectedSequence};
    use gimli::{
        AttributeValue, BigEndian, ColumnType, DebugLine, DebugLineOffset, EndianSlice, Error,
        IncompleteLineProgram, LineProgramHeader, LineRow, LineSequence, LittleEndian,
    };
    use std::panic::{catch_unwind, AssertUnwindSafe};

    #[derive(Clone, Debug, PartialEq, Eq)]
    pub struct ObservedRow {
        pub address: u64,
        pub op_index: u64,
        pub file_name: Vec<u8>,
        pub line: Option<u64>,
        pub column: u64,
        pub is_stmt: bool,
        pub isa: u64,
        pub discriminator: u64,
        pub basic_block: bool,
        pub prologue_end: bool,
        pub epilogue_begin: bool,
        pub end_sequence: bool,
    }

    pub fn mismatch(
        combo: &Combo,
        sequence: usize,
        row: usize,
        field: &str,
        actual: &dyn core::fmt::Debug,
        expected: &dyn core::fmt::Debug,
    ) -> ! {
        panic!(
            "LINE_MODEL_MISMATCH combo={} sequence={} row={} field={} actual={:?} expected={:?}",
            combo.label(),
            sequence,
            row,
            field,
            actual,
            expected
        );
    }

    fn file_name<R: gimli::Reader>(header: &LineProgramHeader<R>, row: &LineRow) -> Vec<u8> {
        let entry = row.file(header).expect("row must reference a declared file");
        match entry.path_name() {
            AttributeValue::String(slice) => slice
                .to_slice()
                .expect("file name should be readable")
                .into_owned(),
            other => panic!("unexpected file attribute: {:?}", other),
        }
    }

    fn snapshot<R: gimli::Reader>(header: &LineProgramHeader<R>, row: &LineRow) -> ObservedRow {
        ObservedRow {
            address: row.address(),
            op_index: row.op_index(),
            file_name: file_name(header, row),
            line: row.line().map(|line| line.get()),
            column: match row.column() {
                ColumnType::LeftEdge => 0,
                ColumnType::Column(column) => column.get(),
            },
            is_stmt: row.is_stmt(),
            isa: row.isa(),
            discriminator: row.discriminator(),
            basic_block: row.basic_block(),
            prologue_end: row.prologue_end(),
            epilogue_begin: row.epilogue_begin(),
            end_sequence: row.end_sequence(),
        }
    }

    fn compare_row(
        combo: &Combo,
        sequence: usize,
        index: usize,
        observed: &ObservedRow,
        expected: &ExpectedRow,
        expected_file_name: &[u8],
    ) {
        macro_rules! field {
            ($name:expr, $got:expr, $want:expr) => {
                if $got != $want {
                    mismatch(combo, sequence, index, $name, &$got, &$want);
                }
            };
        }

        field!("address", observed.address, expected.address);
        field!("op_index", observed.op_index, expected.op_index);
        field!("file_name", observed.file_name, expected_file_name.to_vec());
        // Line zero is exposed as None by the public LineRow::line API.
        let expected_line = if expected.line == 0 {
            None
        } else {
            Some(expected.line)
        };
        field!("line", observed.line, expected_line);
        field!("column", observed.column, expected.column);
        field!("is_stmt", observed.is_stmt, expected.is_stmt);
        field!("isa", observed.isa, expected.isa);
        field!("discriminator", observed.discriminator, expected.discriminator);
        field!("basic_block", observed.basic_block, expected.basic_block);
        field!("prologue_end", observed.prologue_end, expected.prologue_end);
        field!(
            "epilogue_begin",
            observed.epilogue_begin,
            expected.epilogue_begin
        );
        field!("end_sequence", observed.end_sequence, expected.end_sequence);
    }

    fn parse_program_le<'a>(
        bytes: &'a [u8],
        version: u16,
        offset: usize,
    ) -> gimli::Result<IncompleteLineProgram<EndianSlice<'a, LittleEndian>>> {
        let section = DebugLine::new(bytes, LittleEndian);
        let comp_dir = EndianSlice::new(b".", LittleEndian);
        let comp_name = EndianSlice::new(b"main.c", LittleEndian);
        if version <= 4 {
            section.program(DebugLineOffset(offset), 8, Some(comp_dir), Some(comp_name))
        } else {
            section.program(DebugLineOffset(offset), 8, None, None)
        }
    }

    fn parse_program_be<'a>(
        bytes: &'a [u8],
        version: u16,
        offset: usize,
    ) -> gimli::Result<IncompleteLineProgram<EndianSlice<'a, BigEndian>>> {
        let section = DebugLine::new(bytes, BigEndian);
        let comp_dir = EndianSlice::new(b".", BigEndian);
        let comp_name = EndianSlice::new(b"main.c", BigEndian);
        if version <= 4 {
            section.program(DebugLineOffset(offset), 8, Some(comp_dir), Some(comp_name))
        } else {
            section.program(DebugLineOffset(offset), 8, None, None)
        }
    }

    fn compare_matrix(
        combo: &Combo,
        expected: &[ExpectedSequence],
        observed: &[ObservedRow],
        file_names: &[Vec<u8>],
    ) {
        let mut cursor = 0usize;
        for (seq_index, sequence) in expected.iter().enumerate() {
            for (row_index, expected_row) in sequence.rows.iter().enumerate() {
                let Some(observed_row) = observed.get(cursor) else {
                    mismatch(
                        combo,
                        seq_index,
                        row_index,
                        "row_count",
                        &"missing observed row",
                        &expected_row,
                    );
                };
                let expected_file_name =
                    file_names.get(expected_row.file as usize).unwrap_or_else(|| {
                        panic!(
                            "model referenced undeclared file index {} in {}",
                            expected_row.file,
                            combo.label()
                        )
                    });
                compare_row(
                    combo,
                    seq_index,
                    row_index,
                    observed_row,
                    expected_row,
                    expected_file_name,
                );
                cursor += 1;
            }
        }
        if cursor != observed.len() {
            mismatch(
                combo,
                expected.len(),
                cursor,
                "row_count",
                &observed.len(),
                &cursor,
            );
        }
    }

    fn collect_with_program<R>(
        combo: &Combo,
        expected: &[ExpectedSequence],
        file_names: &[Vec<u8>],
        program: IncompleteLineProgram<R>,
    ) where
        R: gimli::Reader,
    {
        // Read path A: execute the whole program straight through.
        let mut streamed = Vec::new();
        {
            let mut rows = program.clone().rows();
            while let Some((header, row)) = rows.next_row().expect("program should execute") {
                streamed.push(snapshot(header, row));
            }
        }
        compare_matrix(combo, expected, &streamed, file_names);

        // Read path B: split into sequences, resume them in reverse order, then
        // resume the first sequence a second time to prove no state leaks.
        let (complete, sequences) = program.sequences().expect("sequences should parse");
        if sequences.len() != expected.len() {
            mismatch(
                combo,
                0,
                0,
                "sequence_count",
                &sequences.len(),
                &expected.len(),
            );
        }
        check_sequence_bounds(combo, expected, &sequences);

        let mut resumed_rows: Vec<(usize, ObservedRow)> = Vec::new();
        for seq_index in (0..sequences.len()).rev() {
            let mut rows = complete.resume_from(&sequences[seq_index]);
            let mut count = 0;
            while let Some((header, row)) = rows.next_row().expect("resume should execute") {
                resumed_rows.push((seq_index, snapshot(header, row)));
                count += 1;
            }
            if count != expected[seq_index].rows.len() {
                mismatch(
                    combo,
                    seq_index,
                    count,
                    "sequence_row_count",
                    &count,
                    &expected[seq_index].rows.len(),
                );
            }
        }

        // Reverse-order reads must match the same independent matrix.
        for seq_index in (0..sequences.len()).rev() {
            let slice: Vec<&ObservedRow> = resumed_rows
                .iter()
                .filter(|(s, _)| *s == seq_index)
                .map(|(_, row)| row)
                .collect();
            for (row_index, observed_row) in slice.iter().enumerate() {
                let expected_row = &expected[seq_index].rows[row_index];
                let expected_file_name = &file_names[expected_row.file as usize];
                compare_row(
                    combo,
                    seq_index,
                    row_index,
                    observed_row,
                    expected_row,
                    expected_file_name,
                );
            }
        }

        // Re-reading the first sequence must reproduce the first result.
        let mut first_reread = Vec::new();
        {
            let mut rows = complete.resume_from(&sequences[0]);
            while let Some((header, row)) = rows.next_row().expect("re-read should execute") {
                first_reread.push(snapshot(header, row));
            }
        }
        let first_resume: Vec<&ObservedRow> = resumed_rows
            .iter()
            .filter(|(s, _)| *s == 0)
            .map(|(_, row)| row)
            .collect();
        if first_reread.len() != first_resume.len() {
            mismatch(
                combo,
                0,
                0,
                "resume_state_leak",
                &first_reread.len(),
                &first_resume.len(),
            );
        }
        for (index, (a, b)) in first_reread.iter().zip(first_resume).enumerate() {
            let expected_row = &expected[0].rows[index];
            let expected_file_name = &file_names[expected_row.file as usize];
            compare_row(combo, 0, index, a, expected_row, expected_file_name);
            if a != b {
                mismatch(combo, 0, index, "resume_state_leak", a, b);
            }
        }
    }

    fn check_sequence_bounds<R: gimli::Reader>(
        combo: &Combo,
        expected: &[ExpectedSequence],
        sequences: &[LineSequence<R>],
    ) {
        for (index, (sequence, expected_sequence)) in
            sequences.iter().zip(expected.iter()).enumerate()
        {
            let first = &expected_sequence.rows[0];
            let last = expected_sequence.rows.last().unwrap();
            if sequence.start != first.address {
                mismatch(
                    combo,
                    index,
                    0,
                    "sequence_start",
                    &sequence.start,
                    &first.address,
                );
            }
            if sequence.end != last.address {
                mismatch(
                    combo,
                    index,
                    expected_sequence.rows.len() - 1,
                    "sequence_end",
                    &sequence.end,
                    &last.address,
                );
            }
        }
    }

    pub fn run_combo(
        combo: &Combo,
        bytes: &[u8],
        expected: Vec<ExpectedSequence>,
        files: Vec<Vec<u8>>,
    ) {
        if combo.big_endian {
            let program = parse_program_be(bytes, combo.version, 0)
                .expect("valid header should parse");
            collect_with_program(combo, &expected, &files, program);
        } else {
            let program = parse_program_le(bytes, combo.version, 0)
                .expect("valid header should parse");
            collect_with_program(combo, &expected, &files, program);
        }
    }

    pub fn header_params(combo: &Combo) -> HeaderParams {
        HeaderParams {
            version: combo.version,
            dwarf64: combo.dwarf64,
            big_endian: combo.big_endian,
            address_size: 8,
            min_insn_len: combo.min_insn_len,
            max_ops: combo.max_ops,
            line_base: LINE_BASE,
            line_range: LINE_RANGE,
        }
    }

    // -- Corrupt input handling ---------------------------------------------

    fn assert_rejects<T: core::fmt::Debug>(label: &str, result: std::thread::Result<gimli::Result<T>>) {
        match result {
            Ok(Ok(value)) => panic!("corrupt input {label} was accepted: {value:?}"),
            Ok(Err(_)) => {}
            Err(payload) => panic!("corrupt input {label} panicked: {payload:?}"),
        }
    }

    /// Header-level corruption must surface as the precise typed error and
    /// never as a panic or an accepted program.
    fn assert_header_error(
        label: &str,
        bytes: &[u8],
        expected: gimli::Error,
    ) {
        let result = catch_unwind(AssertUnwindSafe(|| parse_program_le(bytes, 4, 0)));
        match result {
            Ok(Ok(program)) => panic!("corrupt input {label} was accepted: {program:?}"),
            Ok(Err(actual)) => assert_eq!(
                actual, expected,
                "corrupt input {label} produced an unexpected error"
            ),
            Err(payload) => panic!("corrupt input {label} panicked: {payload:?}"),
        }
    }

    fn base_header_params() -> HeaderParams {
        HeaderParams {
            version: 4,
            dwarf64: false,
            big_endian: false,
            address_size: 8,
            min_insn_len: 1,
            max_ops: 1,
            line_base: LINE_BASE,
            line_range: LINE_RANGE,
        }
    }

    /// Minimal valid neighbour unit: set_address, copy, end_sequence.
    fn neighbour_unit() -> Vec<u8> {
        let mut program = Vec::new();
        // DW_LNE_set_address(0x5555)
        program.extend_from_slice(&[0x00, 0x09, 0x02]);
        program.extend_from_slice(&0x5555u64.to_le_bytes());
        // DW_LNS_copy
        program.push(0x01);
        // DW_LNE_end_sequence
        program.extend_from_slice(&[0x00, 0x01, 0x01]);
        super::encode::encode_unit(&base_header_params(), &program)
    }

    fn truncated_unit(program: &[u8]) -> Vec<u8> {
        super::encode::encode_unit(&base_header_params(), program)
    }

    pub fn run_corrupt_cases() {
        let neighbour = neighbour_unit();

        // 1. Zero maximum_operations_per_instruction: rejected while parsing
        //    the header.
        let zero_max_ops = {
            let mut params = base_header_params();
            params.max_ops = 0;
            super::encode::encode_unit(&params, &[0x00, 0x01, 0x01])
        };
        assert_header_error(
            "zero maximum_operations_per_instruction",
            &zero_max_ops,
            Error::MaximumOperationsPerInstructionZero,
        );

        // 2. Zero line_range: rejected while parsing the header.
        let zero_line_range = {
            let mut params = base_header_params();
            params.line_range = 0;
            super::encode::encode_unit(&params, &[0x00, 0x01, 0x01])
        };
        assert_header_error(
            "zero line_range",
            &zero_line_range,
            Error::LineRangeZero,
        );

        // 3. Truncated unsigned LEB128 on DW_LNS_advance_pc. The unit length
        //    ends exactly on the dangling byte, so gimli must not borrow the
        //    neighbouring unit's bytes to finish it.
        let mut leb_section = truncated_unit(&[0x02, 0x80]);
        let leb_neighbour_offset = leb_section.len();
        leb_section.extend_from_slice(&neighbour);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let program = parse_program_le(&leb_section, 4, 0)?;
            let mut rows = program.rows();
            rows.next_row().map(|_| ())
        }));
        assert_rejects("truncated LEB128 via rows()", result);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let program = parse_program_le(&leb_section, 4, 0)?;
            program.sequences().map(|_| ())
        }));
        assert_rejects("truncated LEB128 via sequences()", result);

        // 4. Truncated DW_LNE_set_address: length promises eight address
        //    bytes but only three are present inside the unit.
        let mut addr_section = truncated_unit(&[0x00, 0x09, 0x02, 0xaa, 0xbb, 0xcc]);
        let addr_neighbour_offset = addr_section.len();
        addr_section.extend_from_slice(&neighbour);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let program = parse_program_le(&addr_section, 4, 0)?;
            let mut rows = program.rows();
            rows.next_row().map(|_| ())
        }));
        assert_rejects("truncated set_address", result);

        // The neighbouring units must still parse and execute independently,
        // proving the truncated unit never reached into their bytes.
        for (section, offset) in [
            (&leb_section, leb_neighbour_offset),
            (&addr_section, addr_neighbour_offset),
        ] {
            let program = parse_program_le(section, 4, offset)
                .expect("neighbour unit should still parse");
            let mut rows = program.rows();
            let mut end_rows = 0;
            while let Some((_, row)) = rows
                .next_row()
                .expect("neighbour program should execute")
            {
                if row.end_sequence() {
                    end_rows += 1;
                }
            }
            assert_eq!(end_rows, 1, "neighbour unit should contain one end row");
        }
    }
}

// ---------------------------------------------------------------------------
// The single regression entry point.
// ---------------------------------------------------------------------------

#[test]
fn line_contract() {
    let versions = [4u16, 5];
    let formats = [false, true]; // dwarf32, dwarf64
    let endians = [false, true]; // little, big
    let slot_shapes = [(1u8, 1u8), (3, 2), (5, 4)]; // (max_ops, min_insn_len)

    let mut cases = 0u64;
    for &version in &versions {
        for &dwarf64 in &formats {
            for &big_endian in &endians {
                for &(max_ops, min_insn_len) in &slot_shapes {
                    let combo = generator::Combo {
                        version,
                        dwarf64,
                        big_endian,
                        max_ops,
                        min_insn_len,
                    };

                    // Fixed seed derived only from the combination coordinates.
                    let seed = 0x51a7_e571_0000
                        ^ ((version as u64) << 40)
                        ^ ((dwarf64 as u64) << 32)
                        ^ ((big_endian as u64) << 24)
                        ^ ((max_ops as u64) << 16)
                        ^ min_insn_len as u64;

                    let program = generator::generate(&combo, seed);

                    // Independent expectation matrix, including its file table.
                    let initial_files: Vec<Vec<u8>> = if version <= 4 {
                        vec![b"main.c".to_vec(), b"hdr.c".to_vec()]
                    } else {
                        vec![b"main.c".to_vec(), b"extra.c".to_vec()]
                    };
                    let model = ir::ReferenceModel::new(
                        initial_files,
                        max_ops,
                        min_insn_len,
                        encode::LINE_RANGE,
                        encode::LINE_BASE,
                        encode::OPCODE_BASE,
                        true,
                    );
                    let (expected, model_files) = model.run(&program);
                    assert_eq!(expected.len(), 2, "two sequences are required");
                    assert!(expected.iter().all(|sequence| {
                        sequence.rows.iter().filter(|row| !row.end_sequence).count() >= 20
                    }));

                    let program_bytes = encode::encode_program(&program, big_endian);
                    let params = checks::header_params(&combo);
                    let unit_bytes = encode::encode_unit(&params, &program_bytes);

                    checks::run_combo(&combo, &unit_bytes, expected, model_files);
                    cases += 1;
                }
            }
        }
    }

    assert_eq!(cases, 24);
    checks::run_corrupt_cases();
    println!("LINE_CONTRACT_CASES=24");
}
