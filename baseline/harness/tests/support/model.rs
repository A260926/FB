//! Independent reference model for the DWARF line number state machine.
//!
//! This model is deliberately separate from the production reader:
//!
//! * it never calls `LineRow::execute`, the gimli writer, or any gimli
//!   internals — expectations are computed from the DWARF specification only;
//! * operation slots are advanced one slot at a time, and byte displacement is
//!   accounted separately, so there is no division/remainder carry formula to
//!   accidentally share with the production code;
//! * special opcodes are decoded by repeatedly subtracting `line_range`.
//!
//! The generator feeds the same decoded instruction stream both to this model
//! and to the byte encoder, so the expected matrix is never reverse-engineered
//! from production rows.

/// One decoded line program instruction, in a representation owned by this
/// harness rather than by gimli.
#[derive(Clone, Debug)]
pub(crate) enum ModelInstr {
    SetAddress(u64),
    EndSequence,
    Copy,
    Special(u8),
    AdvancePc(u64),
    ConstAddPc,
    FixedAdvance(u16),
    AdvanceLine(i64),
    SetFile(u64),
    SetColumn(u64),
    NegateStmt,
    SetBasicBlock,
    SetPrologueEnd,
    SetEpilogueBegin,
    SetIsa(u64),
    SetDiscriminator(u64),
    DefineFile(Vec<u8>),
}

/// One expected row of the line matrix.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ModelRow {
    pub address: u64,
    pub op_index: u64,
    pub file: u64,
    /// Raw line register; zero is mapped to `None` at comparison time.
    pub line: u64,
    /// Raw column register; zero means left edge.
    pub column: u64,
    pub is_stmt: bool,
    pub isa: u64,
    pub discriminator: u64,
    pub basic_block: bool,
    pub prologue_end: bool,
    pub epilogue_begin: bool,
    pub end_sequence: bool,
}

/// One expected sequence, including its terminating end-of-sequence row.
#[derive(Clone, Debug)]
pub(crate) struct ModelSequence {
    pub start: u64,
    pub end: u64,
    pub rows: Vec<ModelRow>,
}

/// Parameters of the line program header that the model has to honour.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ModelParams {
    pub version: u16,
    pub minimum_instruction_length: u64,
    pub maximum_operations_per_instruction: u64,
    pub default_is_stmt: bool,
    pub line_base: i64,
    pub line_range: u64,
    pub opcode_base: u64,
}

pub(crate) struct LineModel {
    params: ModelParams,
    // File table as exposed by `LineProgramHeader::file`: v5 indexes the
    // header entries directly, v4 treats index 0 as the compilation file and
    // indexes the remaining entries from 1.
    comp_file: Vec<u8>,
    file_names: Vec<Vec<u8>>,

    // State machine registers.
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

    current: Vec<ModelRow>,
    current_start: Option<u64>,
    pub sequences: Vec<ModelSequence>,
}

impl LineModel {
    pub fn new(params: ModelParams, comp_file: &[u8], initial_files: &[Vec<u8>]) -> Self {
        let mut model = LineModel {
            params,
            comp_file: comp_file.to_vec(),
            file_names: initial_files.to_vec(),
            address: 0,
            op_index: 0,
            file: 1,
            line: 1,
            column: 0,
            is_stmt: params.default_is_stmt,
            isa: 0,
            discriminator: 0,
            basic_block: false,
            prologue_end: false,
            epilogue_begin: false,
            current: Vec::new(),
            current_start: None,
            sequences: Vec::new(),
        };
        // DWARF 5 numbers header files from 0, so the initial file register 1
        // still points at the second declared entry; the first emitted row in
        // every sequence sets the file explicitly either way.
        if params.version >= 5 {
            model.file = 1;
        }
        model
    }

    /// Resolve a file register value to the actual source file name, mirroring
    /// the indexing rules of `LineProgramHeader::file`.
    pub fn file_name(&self, index: u64) -> Vec<u8> {
        if self.params.version <= 4 {
            if index == 0 {
                return self.comp_file.clone();
            }
            self.file_names[(index - 1) as usize].clone()
        } else {
            self.file_names[index as usize].clone()
        }
    }

    /// Run the full instruction stream and build the expected matrix.
    pub fn run(&mut self, instrs: &[ModelInstr]) {
        for instr in instrs {
            self.step(instr);
        }
        assert!(
            self.current.is_empty(),
            "model stream must finish on end_sequence"
        );
    }

    fn step(&mut self, instr: &ModelInstr) {
        match *instr {
            ModelInstr::SetAddress(addr) => {
                self.address = addr;
                self.op_index = 0;
            }
            ModelInstr::FixedAdvance(operand) => {
                // Raw byte displacement; the minimum instruction length does
                // not multiply this operand, and the slot is reset.
                self.address = self.address.wrapping_add(u64::from(operand));
                self.op_index = 0;
            }
            ModelInstr::AdvancePc(operations) => self.advance_operations(operations),
            ModelInstr::ConstAddPc => {
                let adjusted = 255 - self.params.opcode_base;
                let (_, operation_advance) = self.decode_special(adjusted);
                self.advance_operations(operation_advance);
            }
            ModelInstr::Special(opcode) => {
                let adjusted = u64::from(opcode) - self.params.opcode_base;
                let (line_advance, operation_advance) = self.decode_special(adjusted);
                self.apply_line_advance(line_advance);
                self.advance_operations(operation_advance);
                self.append_row(false);
                self.reset_transient();
            }
            ModelInstr::AdvanceLine(delta) => self.apply_line_advance(delta),
            ModelInstr::SetFile(file) => self.file = file,
            ModelInstr::SetColumn(column) => self.column = column,
            ModelInstr::NegateStmt => self.is_stmt = !self.is_stmt,
            ModelInstr::SetBasicBlock => self.basic_block = true,
            ModelInstr::SetPrologueEnd => self.prologue_end = true,
            ModelInstr::SetEpilogueBegin => self.epilogue_begin = true,
            ModelInstr::SetIsa(isa) => self.isa = isa,
            ModelInstr::SetDiscriminator(discriminator) => self.discriminator = discriminator,
            ModelInstr::DefineFile(ref name) => self.file_names.push(name.clone()),
            ModelInstr::Copy => {
                self.append_row(false);
                self.reset_transient();
            }
            ModelInstr::EndSequence => {
                self.append_row(true);
                let rows = core::mem::take(&mut self.current);
                let start = self.current_start.take().expect("sequence has rows");
                let end = self.address;
                self.sequences.push(ModelSequence { start, end, rows });
                self.reset_all();
            }
        }
    }

    /// Push the operation pointer forward one slot at a time. Every time the
    /// slot counter reaches the instruction's operation limit it rolls over to
    /// zero and exactly one minimum-instruction-length byte displacement is
    /// recorded. For single-slot instructions every operation moves the
    /// address by one minimum instruction length.
    fn advance_operations(&mut self, operations: u64) {
        let mop = self.params.maximum_operations_per_instruction;
        let mil = self.params.minimum_instruction_length;
        for _ in 0..operations {
            if mop == 1 {
                self.address = self.address.wrapping_add(mil);
                self.op_index = 0;
            } else {
                self.op_index += 1;
                if self.op_index == mop {
                    self.op_index = 0;
                    self.address = self.address.wrapping_add(mil);
                }
            }
        }
    }

    /// Decode a special opcode by repeated subtraction: no division or
    /// remainder is used anywhere in this model.
    fn decode_special(&self, mut adjusted: u64) -> (i64, u64) {
        let mut operation_advance = 0u64;
        while adjusted >= self.params.line_range {
            adjusted -= self.params.line_range;
            operation_advance += 1;
        }
        let line_advance = self.params.line_base + adjusted as i64;
        (line_advance, operation_advance)
    }

    fn apply_line_advance(&mut self, delta: i64) {
        if delta < 0 {
            let decrement = (-delta) as u64;
            // Match the documented saturation at zero; line zero later maps to
            // `None` through the public API.
            self.line = self.line.saturating_sub(decrement);
        } else {
            self.line = self.line.wrapping_add(delta as u64);
        }
    }

    fn append_row(&mut self, end_sequence: bool) {
        if self.current_start.is_none() {
            self.current_start = Some(self.address);
        }
        self.current.push(ModelRow {
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
        });
    }

    /// Reset after an ordinary row: only the transient flags clear, the
    /// persistent registers (file, line, column, is_stmt, isa, address, slot)
    /// keep their values.
    fn reset_transient(&mut self) {
        self.discriminator = 0;
        self.basic_block = false;
        self.prologue_end = false;
        self.epilogue_begin = false;
    }

    /// Reset after end_sequence: every register returns to its header initial
    /// value.
    fn reset_all(&mut self) {
        self.address = 0;
        self.op_index = 0;
        self.file = 1;
        self.line = 1;
        self.column = 0;
        self.is_stmt = self.params.default_is_stmt;
        self.isa = 0;
        self.discriminator = 0;
        self.basic_block = false;
        self.prologue_end = false;
        self.epilogue_begin = false;
    }
}
