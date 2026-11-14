//! The generic ELF module, which gives access to ELF constants and other helper functions, which are independent of ELF bithood.  Also defines an `Elf` struct which implements a unified parser that returns a wrapped `Elf64` or `Elf32` binary.
//!
//! To access the exact 32-bit or 64-bit versions, use [goblin::elf32::Header](header/header32/struct.Header.html)/[goblin::elf64::Header](header/header64/struct.Header.html), etc., for the various 32/64-bit structs.
//!
//! # Example
//!
//! ```rust
//! use std::fs::File;
//!
//! pub fn read (bytes: &[u8]) {
//!   match goblin::elf::Elf::parse(&bytes) {
//!     Ok(binary) => {
//!       let entry = binary.entry;
//!       for ph in binary.program_headers {
//!         if ph.p_type == goblin::elf::program_header::PT_LOAD {
//!           // TODO: you should validate p_filesz before allocating.
//!           let mut _buf = vec![0u8; ph.p_filesz as usize];
//!           // read responsibly
//!          }
//!       }
//!     },
//!     Err(_) => ()
//!   }
//! }
//! ```
//!
//! This will properly access the underlying 32-bit or 64-bit binary automatically. Note that since
//! 32-bit binaries typically have shorter 32-bit values in some cases (specifically for addresses and pointer
//! values), these values are upcasted to u64/i64s when appropriate.
//!
//! See [goblin::elf::Elf](struct.Elf.html) for more information.
//!
//! You are still free to use the specific 32-bit or 64-bit versions by accessing them through `goblin::elf64`, etc., but you will have to parse and/or construct the various components yourself.
//! In other words, there is no unified 32/64-bit `Elf` struct.
//!
//! # Note
//! To use the automagic ELF datatype union parser, you _must_ enable/opt-in to the  `elf64`, `elf32`, and
//! `endian_fd` features if you disable `default`.

#[macro_use]
pub(crate) mod gnu_hash;

// These are shareable values for the 32/64 bit implementations.
//
// They are publicly re-exported by the pub-using module
pub mod compression_header;
pub mod header;
pub mod program_header;
pub mod section_header;
#[macro_use]
pub mod sym;
pub mod dynamic;
#[macro_use]
pub mod reloc;
pub mod note;
#[cfg(all(any(feature = "elf32", feature = "elf64"), feature = "alloc"))]
pub mod symver;

macro_rules! if_sylvan {
    ($($i:item)*) => ($(
        #[cfg(all(feature = "elf32", feature = "elf64", feature = "endian_fd"))]
        $i
    )*)
}

if_sylvan! {
    use scroll::{ctx, Pread, Endian};
    use crate::strtab::Strtab;
    use crate::error;
    use crate::options::{Permissive, ParseOptions};
    use crate::container::{Container, Ctx};
    use alloc::vec::Vec;
    use core::cmp;

    pub use header::Header;
    pub use program_header::ProgramHeader;
    pub use section_header::SectionHeader;
    pub use sym::Symtab;
    pub use sym::Sym;
    pub use sym::SymbolSection;
    pub use dynamic::Dyn;
    pub use dynamic::Dynamic;
    pub use reloc::Reloc;
    pub use reloc::RelocSection;
    pub use symver::{VersymSection, VerdefSection, VerneedSection};

    pub type ProgramHeaders = Vec<ProgramHeader>;
    pub type SectionHeaders = Vec<SectionHeader>;
    pub type ShdrIdx = usize;

    /// A physical symbol table section (`SHT_SYMTAB` or `SHT_DYNSYM`) together
    /// with the string table its `sh_link` points at.
    #[derive(Debug, Clone)]
    pub struct SymbolTable<'a> {
        /// Section header index of the symbol table section.
        pub section: ShdrIdx,
        /// The parsed symbol table, with any `SHT_SYMTAB_SHNDX` attachment
        /// applied so `get`/`iter`/`to_vec` resolve `SHN_XINDEX` entries.
        pub symtab: Symtab<'a>,
        /// The string table section named by the symbol table's `sh_link`.
        pub strtab: Strtab<'a>,
    }

    /// A symbol reached through a relocation record.
    #[derive(Debug, Clone)]
    pub struct RelocSymbolRef<'a> {
        /// Section header index of the `SHT_REL`/`SHT_RELA` section.
        pub reloc_section: ShdrIdx,
        /// Record number inside the relocation section.
        pub reloc_index: usize,
        /// Section the relocations apply to, i.e. the relocation section's
        /// `sh_info`.
        pub target_section: ShdrIdx,
        /// Section header index of the symbol table named by the relocation
        /// section's `sh_link`.
        pub symtab_section: ShdrIdx,
        /// Symbol number inside that table, i.e. the relocation's `r_sym`.
        pub symbol_index: usize,
        /// The referenced symbol, with any extension-resolved `st_shndx`.
        pub symbol: Sym,
        /// The symbol name, looked up in the table's own string table.
        pub name: Option<&'a str>,
        /// Where the symbol is defined, according to its raw and extended
        /// section indices.
        pub section: SymbolSection,
    }

    #[derive(Debug)]
    /// An ELF binary. The underlying data structures are read according to the headers byte order and container size (32 or 64).
    pub struct Elf<'a> {
        /// The ELF header, which provides a rudimentary index into the rest of the binary
        pub header: Header,
        /// The program headers; they primarily tell the kernel and the dynamic linker
        /// how to load this binary
        pub program_headers: ProgramHeaders,
        /// The sections headers. These are strippable, never count on them being
        /// here unless you're a static linker!
        pub section_headers: SectionHeaders,
        /// The section header string table
        pub shdr_strtab: Strtab<'a>,
        /// The string table for the dynamically accessible symbols
        pub dynstrtab: Strtab<'a>,
        /// The dynamically accessible symbols, i.e., exports, imports.
        /// This is what the dynamic linker uses to dynamically load and link your binary,
        /// or find imported symbols for binaries which dynamically link against your library
        pub dynsyms: Symtab<'a>,
        /// The debugging symbol table
        pub syms: Symtab<'a>,
        /// The string table for the symbol table
        pub strtab: Strtab<'a>,
        /// Contains dynamic linking information, with the _DYNAMIC array + a preprocessed DynamicInfo for that array
        pub dynamic: Option<Dynamic>,
        /// The dynamic relocation entries (strings, copy-data, etc.) with an addend
        pub dynrelas: RelocSection<'a>,
        /// The dynamic relocation entries without an addend
        pub dynrels: RelocSection<'a>,
        /// The plt relocation entries (procedure linkage table). For 32-bit binaries these are usually Rel (no addend)
        pub pltrelocs: RelocSection<'a>,
        /// Section relocations by section index (only present if this is a relocatable object file)
        pub shdr_relocs: Vec<(ShdrIdx, RelocSection<'a>)>,
        /// The binary's soname, if it has one
        pub soname: Option<&'a str>,
        /// The binary's program interpreter (e.g., dynamic linker), if it has one
        pub interpreter: Option<&'a str>,
        /// A list of this binary's dynamic libraries it uses, if there are any
        pub libraries: Vec<&'a str>,
        /// A list of runtime search paths for this binary's dynamic libraries it uses, if there
        /// are any. (deprecated)
        pub rpaths: Vec<&'a str>,
        /// A list of runtime search paths for this binary's dynamic libraries it uses, if there
        /// are any.
        pub runpaths: Vec<&'a str>,
        /// Whether this is a 64-bit elf or not
        pub is_64: bool,
        /// Whether this is a shared object or not
        pub is_lib: bool,
        /// The binaries entry point address, if it has one
        pub entry: u64,
        /// Whether the binary is little endian or not
        pub little_endian: bool,
        /// Contains the symbol version information from the optional section
        /// [`SHT_GNU_VERSYM`][section_header::SHT_GNU_VERSYM] (GNU extenstion).
        pub versym : Option<VersymSection<'a>>,
        /// Contains the version definition information from the optional section
        /// [`SHT_GNU_VERDEF`][section_header::SHT_GNU_VERDEF] (GNU extenstion).
        pub verdef : Option<VerdefSection<'a>>,
        /// Contains the version needed information from the optional section
        /// [`SHT_GNU_VERNEED`][section_header::SHT_GNU_VERNEED] (GNU extenstion).
        pub verneed : Option<VerneedSection<'a>>,
        /// Every physical `SHT_SYMTAB`/`SHT_DYNSYM` section in the file, keyed
        /// by section header index, each paired with its own string table and
        /// any attached `SHT_SYMTAB_SHNDX` extension table.
        symbol_tables: Vec<SymbolTable<'a>>,
        ctx: Ctx,
    }

    impl<'a> Elf<'a> {
        /// Try to iterate notes in PT_NOTE program headers; returns `None` if there aren't any note headers in this binary
        pub fn iter_note_headers(&self, data: &'a [u8]) -> Option<note::NoteIterator<'a>> {
            let mut iters = vec![];
            for phdr in &self.program_headers {
                if phdr.p_type == program_header::PT_NOTE {
                    let offset = phdr.p_offset as usize;
                    let alignment = phdr.p_align as usize;

                    iters.push(note::NoteDataIterator {
                        data,
                        offset,
                        size: offset.saturating_add(phdr.p_filesz as usize),
                        ctx: (alignment, self.ctx)
                    });
                }
            }

            if iters.is_empty() {
                None
            } else {
                Some(note::NoteIterator {
                    iters: iters,
                    index: 0,
                })
            }
        }
        /// Try to iterate notes in SHT_NOTE sections; returns `None` if there aren't any note sections in this binary
        ///
        /// If a section_name is given, only the section with the according name is iterated.
        pub fn iter_note_sections(
            &self,
            data: &'a [u8],
            section_name: Option<&str>,
        ) -> Option<note::NoteIterator<'a>> {
            let mut iters = vec![];
            for sect in &self.section_headers {
                if sect.sh_type != section_header::SHT_NOTE {
                    continue;
                }

                if section_name.is_some() && self.shdr_strtab.get_at(sect.sh_name) != section_name {
                    continue;
                }

                let offset = sect.sh_offset as usize;
                let alignment = sect.sh_addralign as usize;
                iters.push(note::NoteDataIterator {
                    data,
                    offset,
                    size: offset.saturating_add(sect.sh_size as usize),
                    ctx: (alignment, self.ctx)
                });
            }

            if iters.is_empty() {
                None
            } else {
                Some(note::NoteIterator {
                    iters: iters,
                    index: 0,
                })
            }
        }
        pub fn is_object_file(&self) -> bool {
            self.header.e_type == header::ET_REL
        }

        /// Every physical symbol table section (`SHT_SYMTAB` and
        /// `SHT_DYNSYM`) found in the file, in section header order, each
        /// paired with the string table named by its `sh_link`.
        ///
        /// Symbol tables are never merged: two tables containing the same
        /// symbol index or the same symbol name stay distinct.
        #[inline]
        pub fn symbol_tables(&self) -> &[SymbolTable<'a>] {
            &self.symbol_tables
        }

        /// Access the symbol table stored in physical section header
        /// `section`, together with its own string table.
        ///
        /// Returns `None` when `section` is not an `SHT_SYMTAB`/`SHT_DYNSYM`
        /// section. The conventional [`Elf::syms`] field remains available for
        /// the regular symbol table.
        #[inline]
        pub fn symbol_table(&self, section: ShdrIdx) -> Option<&SymbolTable<'a>> {
            self.symbol_tables
                .iter()
                .find(|entry| entry.section == section)
        }

        /// Resolve the symbol referenced by relocation record `reloc_index`
        /// inside relocation section `reloc_section`.
        ///
        /// Both `SHT_REL` and `SHT_RELA` sections are followed through their
        /// own `sh_link` to find the symbol table; the regular [`Elf::syms`]
        /// table is never assumed. `Ok(None)` means the section or record
        /// number does not exist; an internally inconsistent reference (a
        /// `sh_link` that is not a symbol table, or an `r_sym` past the end of
        /// the table) returns an error. The relocation records themselves are
        /// left untouched, so [`Elf::shdr_relocs`] keeps its original order,
        /// offsets, types and addends.
        pub fn reloc_symbol(
            &self,
            reloc_section: ShdrIdx,
            reloc_index: usize,
        ) -> error::Result<Option<RelocSymbolRef<'a>>> {
            let relocs = match self
                .shdr_relocs
                .iter()
                .find_map(|(idx, relocs)| (*idx == reloc_section).then_some(relocs))
            {
                Some(relocs) => relocs,
                None => return Ok(None),
            };
            let Some(reloc) = relocs.get(reloc_index) else {
                return Ok(None);
            };
            let shdr = self.section_headers.get(reloc_section).ok_or_else(|| {
                error::Error::Malformed(format!(
                    "Relocation section {reloc_section} is missing from section headers"
                ))
            })?;
            if shdr.sh_type != section_header::SHT_REL
                && shdr.sh_type != section_header::SHT_RELA
            {
                return Err(error::Error::Malformed(format!(
                    "Section {reloc_section} is not an SHT_REL/SHT_RELA section"
                )));
            }

            let symtab_section = shdr.sh_link as usize;
            let entry = self.symbol_table(symtab_section).ok_or_else(|| {
                error::Error::Malformed(format!(
                    "Relocation section {reloc_section} sh_link ({symtab_section}) does not name a symbol table section"
                ))
            })?;
            let symbol_index = reloc.r_sym;
            let symbol = entry.symtab.get(symbol_index).ok_or_else(|| {
                error::Error::Malformed(format!(
                    "Relocation record {reloc_index} in section {reloc_section} references symbol {symbol_index}, \
                     but symbol table section {symtab_section} only holds {} symbols",
                    entry.symtab.len()
                ))
            })?;
            let section = entry.symtab.section_kind(symbol_index).ok_or_else(|| {
                error::Error::Malformed(format!(
                    "Failed to read section index of symbol {symbol_index} in table section {symtab_section}"
                ))
            })?;
            let name = entry.strtab.get_at(symbol.st_name);

            Ok(Some(RelocSymbolRef {
                reloc_section,
                reloc_index,
                target_section: shdr.sh_info as usize,
                symtab_section,
                symbol_index,
                symbol,
                name,
                section,
            }))
        }

        /// Parses the contents to get the Header only. This `bytes` buffer should contain at least the length for parsing Header.
        pub fn parse_header(bytes: &'a [u8]) -> error::Result<Header> {
            bytes.pread::<Header>(0)
        }

        /// Lazy parse the ELF contents. This function mainly just assembles an Elf struct. Once we have the struct, we can choose to parse whatever we want.
        pub fn lazy_parse(header: Header) -> error::Result<Self> {
            let misc = parse_misc(&header)?;

            Ok(Elf {
                header,
                program_headers: vec![],
                section_headers: Default::default(),
                shdr_strtab: Default::default(),
                dynamic: None,
                dynsyms: Default::default(),
                dynstrtab: Strtab::default(),
                syms: Default::default(),
                strtab: Default::default(),
                dynrelas: Default::default(),
                dynrels: Default::default(),
                pltrelocs: Default::default(),
                shdr_relocs: Default::default(),
                soname: None,
                interpreter: None,
                libraries: vec![],
                rpaths: vec![],
                runpaths: vec![],
                is_64: misc.is_64,
                is_lib: misc.is_lib,
                entry: misc.entry,
                little_endian: misc.little_endian,
                ctx: misc.ctx,
                versym: None,
                verdef: None,
                verneed: None,
                symbol_tables: vec![],
            })
        }

        /// Parses the contents of the byte stream in `bytes`, and maybe returns a unified binary
        pub fn parse(bytes: &'a [u8]) -> error::Result<Self> {
            Self::parse_with_opts(bytes, &ParseOptions::default())
        }

        /// Parses the contents of the byte stream in `bytes` with options, and maybe returns a unified binary
        pub fn parse_with_opts(bytes: &'a [u8], opts: &ParseOptions) -> error::Result<Self> {
            let header = Self::parse_header(bytes)?;
            let misc = parse_misc(&header)?;
            let ctx = misc.ctx;
            let permissive = opts.parse_mode.is_permissive();

            // MIPS64 little-endian uses a non-standard relocation info layout
            let is_mips64el = header.e_machine == header::EM_MIPS
                && ctx.is_big()
                && ctx.is_little_endian();

            let program_headers = ProgramHeader::parse(bytes, header.e_phoff as usize, header.e_phnum as usize, ctx)?;

            let mut interpreter = None;
            for ph in &program_headers {
                if ph.p_type == program_header::PT_INTERP && ph.p_filesz != 0 {
                    let count = (ph.p_filesz - 1) as usize;
                    let offset = ph.p_offset as usize;
                    interpreter = bytes.pread_with::<&str>(offset, ::scroll::ctx::StrCtx::Length(count)).ok();
                }
            }

            let section_headers = SectionHeader::parse(bytes, header.e_shoff as usize, header.e_shnum as usize, ctx)
                .or_permissive_and_default(permissive, "Failed to parse section headers")?;

            let get_strtab = |section_headers: &[SectionHeader], mut section_idx: usize| {
                if section_idx == section_header::SHN_XINDEX as usize {
                    if section_headers.is_empty() {
                        return Ok(Strtab::default())
                    }
                    section_idx = section_headers[0].sh_link as usize;
                }

                if section_idx >= section_headers.len() {
                    // FIXME: warn! here
                    Ok(Strtab::default())
                } else {
                    let shdr = &section_headers[section_idx];
                    shdr.check_size_with_opts(bytes.len(), permissive)?;
                    Strtab::parse_with_opts(bytes, shdr.sh_offset as usize, shdr.sh_size as usize, 0x0, opts)
                }
            };

            let strtab_idx = header.e_shstrndx as usize;
            let shdr_strtab = get_strtab(&section_headers, strtab_idx)?;

            // Build the registry of every physical SHT_SYMTAB/SHT_DYNSYM
            // section together with its own string table.
            let mut symbol_tables: Vec<SymbolTable<'a>> = vec![];
            for (idx, shdr) in section_headers.iter().enumerate() {
                if shdr.sh_type != section_header::SHT_SYMTAB
                    && shdr.sh_type != section_header::SHT_DYNSYM
                {
                    continue;
                }
                let size = shdr.sh_entsize;
                let initial_count = if size == 0 { 0 } else { shdr.sh_size / size };

                // Check for extremely large counts that exceed usize capacity
                let count = if initial_count > usize::MAX as u64 {
                    Err(crate::error::Error::Malformed(
                        format!(
                            "Symbol table count ({}) from section header exceeds maximum possible value",
                            initial_count
                        )
                    ))
                    .or_permissive_and_then(
                        permissive,
                        "Symbol table count exceeds maximum; truncating",
                        || usize::MAX as u64,
                    )?
                } else {
                    initial_count
                };

                let table = Symtab::parse_with_opts(bytes, shdr.sh_offset as usize, count as usize, ctx, opts)?;
                let table_strtab = get_strtab(&section_headers, shdr.sh_link as usize)?;
                symbol_tables.push(SymbolTable {
                    section: idx,
                    symtab: table,
                    strtab: table_strtab,
                });
            }

            // Every SHT_SYMTAB_SHNDX section must attach to exactly one of the
            // symbol tables above; these are strict validity checks. In
            // permissive mode a bad attachment is ignored instead of failing.
            if !permissive {
                for shndx_shdr in section_headers.iter() {
                    if shndx_shdr.sh_type != section_header::SHT_SYMTAB_SHNDX {
                        continue;
                    }
                    let link = shndx_shdr.sh_link as usize;
                    match section_headers.get(link) {
                        Some(linked)
                            if linked.sh_type == section_header::SHT_SYMTAB
                                || linked.sh_type == section_header::SHT_DYNSYM => {}
                        _ => {
                            return Err(crate::error::Error::Malformed(format!(
                                "SHT_SYMTAB_SHNDX section sh_link ({link}) does not name a symbol table section"
                            )));
                        }
                    }
                }
            }

            // Validate each attachment (uniqueness, length, entry contents and
            // index bounds) and attach its bytes to the matching table.
            for entry in symbol_tables.iter_mut() {
                let extended = validate_symtab_shndx(
                    bytes,
                    &section_headers,
                    entry.section,
                    entry.symtab.len(),
                    ctx,
                )
                .or_permissive_and_value(
                    permissive,
                    "Invalid SHT_SYMTAB_SHNDX attachment; leaving symbol table without extension table",
                    None,
                )?;
                if let Some(extended_bytes) = extended {
                    entry.symtab.with_extended_sections(extended_bytes);
                }
            }

            // The conventional syms/strtab fields keep pointing at the last
            // SHT_SYMTAB section, matching the previous rfind() behaviour.
            let mut syms = Symtab::default();
            let mut strtab = Strtab::default();
            if let Some(entry) = symbol_tables
                .iter()
                .rev()
                .find(|entry| section_headers[entry.section].sh_type == section_header::SHT_SYMTAB)
            {
                syms = entry.symtab.clone();
                strtab = entry.strtab.clone();
            }

            let mut is_pie = false;
            let mut soname = None;
            let mut libraries = vec![];
            let mut rpaths = vec![];
            let mut runpaths = vec![];
            let mut dynsyms = Symtab::default();
            let mut dynrelas = RelocSection::default();
            let mut dynrels = RelocSection::default();
            let mut pltrelocs = RelocSection::default();
            let mut dynstrtab = Strtab::default();
            let dynamic = Dynamic::parse(bytes, &program_headers, ctx)
                .or_permissive_and_default(permissive, "Failed to parse dynamic section")?;
            if let Some(ref dynamic) = dynamic {
                let dyn_info = &dynamic.info;

                is_pie = dyn_info.flags_1 & dynamic::DF_1_PIE != 0;
                dynstrtab = Strtab::parse_with_opts(bytes,
                                      dyn_info.strtab,
                                      dyn_info.strsz,
                                      0x0, opts)
                    .or_permissive_and_default(permissive, "Failed to parse dynamic string table")?;

                if dyn_info.soname != 0 {
                    // FIXME: warn! here
                    soname = dynstrtab.get_at(dyn_info.soname);
                }
                if dyn_info.needed_count > 0 {
                    libraries = dynamic.get_libraries(&dynstrtab);
                }
                for dyn_ in &dynamic.dyns {
                    if dyn_.d_tag == dynamic::DT_RPATH {
                        if let Some(path) = dynstrtab.get_at(dyn_.d_val as usize) {
                            rpaths.push(path);
                        }
                    } else if dyn_.d_tag == dynamic::DT_RUNPATH {
                        if let Some(path) = dynstrtab.get_at(dyn_.d_val as usize) {
                            runpaths.push(path);
                        }
                    }
                }
                // parse the dynamic relocations
                dynrelas = RelocSection::parse_inner(bytes, dyn_info.rela, dyn_info.relasz, true, ctx, is_mips64el)
                    .or_permissive_and_default(permissive, "Failed to parse dynamic RELA relocations")?;

                dynrels = RelocSection::parse_inner(bytes, dyn_info.rel, dyn_info.relsz, false, ctx, is_mips64el)
                    .or_permissive_and_default(permissive, "Failed to parse dynamic REL relocations")?;

                let is_rela = dyn_info.pltrel as u64 == dynamic::DT_RELA;
                pltrelocs = RelocSection::parse_inner(bytes, dyn_info.jmprel, dyn_info.pltrelsz, is_rela, ctx, is_mips64el)
                    .or_permissive_and_default(permissive, "Failed to parse PLT relocations")?;

                let mut num_syms = if let Some(gnu_hash) = dyn_info.gnu_hash {
                    gnu_hash_len(bytes, gnu_hash as usize, ctx)
                        .or_permissive_and_default(permissive, "Failed to parse GNU hash table")?
                } else if let Some(hash) = dyn_info.hash {
                    hash_len(bytes, hash as usize, header.e_machine, ctx)
                        .or_permissive_and_default(permissive, "Failed to parse hash table")?
                } else {
                    0
                };
                let max_reloc_sym = dynrelas.iter()
                    .chain(dynrels.iter())
                    .chain(pltrelocs.iter())
                    .fold(0, |num, reloc| cmp::max(num, reloc.r_sym));
                if max_reloc_sym != 0 {
                    num_syms = cmp::max(num_syms, max_reloc_sym + 1);
                }
                dynsyms = Symtab::parse_with_opts(bytes, dyn_info.symtab, num_syms, ctx, opts)?;

                // With PT_DYNAMIC the dynamic symbol source and count above are
                // preserved; the section's extension table is only attached
                // when that section describes the exact same byte range and
                // symbol count.
                if let Some(entry) = symbol_tables
                    .iter()
                    .find(|entry| section_headers[entry.section].sh_type == section_header::SHT_DYNSYM)
                {
                    let dynsym_shdr = &section_headers[entry.section];
                    let sym_size = Sym::size(ctx.container);
                    let section_end = (dynsym_shdr.sh_offset as usize)
                        .checked_add(dynsym_shdr.sh_size as usize);
                    let dynamic_end = num_syms
                        .checked_mul(sym_size)
                        .and_then(|span| dyn_info.symtab.checked_add(span));
                    let matches = match (section_end, dynamic_end) {
                        (Some(section_end), Some(dynamic_end)) => {
                            dynsym_shdr.sh_offset as usize == dyn_info.symtab
                                && section_end == dynamic_end
                                && entry.symtab.len() == num_syms
                        }
                        _ => false,
                    };
                    if !matches {
                        let err = crate::error::Error::Malformed(format!(
                            "SHT_DYNSYM section {} does not match the PT_DYNAMIC symbol table \
                             (section offset {:#x}, size {:#x}, {} symbols; dynamic offset {:#x}, {} symbols)",
                            entry.section,
                            dynsym_shdr.sh_offset,
                            dynsym_shdr.sh_size,
                            entry.symtab.len(),
                            dyn_info.symtab,
                            num_syms
                        ));
                        if permissive {
                            // Keep the dynamic symbol source/count as parsed
                            // above and simply leave the attachment off.
                            let _ = err;
                        } else {
                            return Err(err);
                        }
                    } else if let Some(extended_bytes) = entry.symtab.extended_sections_bytes() {
                        dynsyms.with_extended_sections(extended_bytes);
                    }
                }
            } else if let Some(entry) = symbol_tables
                .iter()
                .find(|entry| section_headers[entry.section].sh_type == section_header::SHT_DYNSYM)
            {
                // No PT_DYNAMIC: read the dynamic symbols straight from the
                // SHT_DYNSYM section and its own string table.
                dynsyms = entry.symtab.clone();
                dynstrtab = entry.strtab.clone();
            }

            let mut shdr_relocs = vec![];
            for (idx, section) in section_headers.iter().enumerate() {
                let is_rela = section.sh_type == section_header::SHT_RELA;
                if is_rela || section.sh_type == section_header::SHT_REL {

                    section.check_size_with_opts(bytes.len(), permissive)?;
                    let sh_relocs_opt = RelocSection::parse_inner(bytes, section.sh_offset as usize, section.sh_size as usize, is_rela, ctx, is_mips64el)
                        .map(Some)
                        .or_permissive_and_default(
                            permissive,
                            "Failed to parse section relocation; skipping",
                        )?;

                    if let Some(sh_relocs) = sh_relocs_opt {
                        shdr_relocs.push((idx, sh_relocs));
                    }
                }
            }

            let versym = symver::VersymSection::parse(bytes, &section_headers, ctx)
                .or_permissive_and_default(permissive, "Failed to parse version symbol section")?;

            let verdef = symver::VerdefSection::parse(bytes, &section_headers, ctx)
                .or_permissive_and_default(permissive, "Failed to parse version definition section")?;

            let verneed = symver::VerneedSection::parse(bytes, &section_headers, ctx)
                .or_permissive_and_default(permissive, "Failed to parse version need section")?;

            let is_lib = misc.is_lib && !is_pie;

            Ok(Elf {
                header,
                program_headers,
                section_headers,
                shdr_strtab,
                dynamic,
                dynsyms,
                dynstrtab,
                syms,
                strtab,
                dynrelas,
                dynrels,
                pltrelocs,
                shdr_relocs,
                soname,
                interpreter,
                libraries,
                rpaths,
                runpaths,
                is_64: misc.is_64,
                is_lib,
                entry: misc.entry,
                little_endian: misc.little_endian,
                ctx: ctx,
                versym,
                verdef,
                verneed,
                symbol_tables,
            })
        }
    }

    impl<'a> ctx::TryFromCtx<'a, (usize, Endian)> for Elf<'a> {
        type Error = crate::error::Error;
        fn try_from_ctx(src: &'a [u8], (_, _): (usize, Endian)) -> Result<(Elf<'a>, usize), Self::Error> {
            let elf = Elf::parse(src)?;
            Ok((elf, src.len()))
        }
    }

    /// Validate the (at most one) `SHT_SYMTAB_SHNDX` section attached to symbol
    /// table section `symtab_section` and return its backing bytes.
    ///
    /// Strict checks performed here, per the task contract:
    /// - the attachment is unique;
    /// - it names `symtab_section` through `sh_link`, which must be either
    ///   `SHT_SYMTAB` or `SHT_DYNSYM`;
    /// - it holds exactly one 32-bit entry per symbol;
    /// - an entry is only allowed to be non-zero when the corresponding
    ///   symbol's raw `st_shndx` is `SHN_XINDEX`;
    /// - every extended value is a valid section header index;
    /// - an `SHN_XINDEX` symbol must have an attachment to resolve it.
    ///
    /// Returns `Ok(None)` when no attachment section exists.
    fn validate_symtab_shndx<'a>(
        bytes: &'a [u8],
        section_headers: &[SectionHeader],
        symtab_section: ShdrIdx,
        symbol_count: usize,
        ctx: Ctx,
    ) -> error::Result<Option<&'a [u8]>> {
        use scroll::Pread;

        let mut attachment: Option<&SectionHeader> = None;
        for shdr in section_headers {
            if shdr.sh_type != section_header::SHT_SYMTAB_SHNDX {
                continue;
            }
            if shdr.sh_link as usize == symtab_section {
                if attachment.is_some() {
                    return Err(error::Error::Malformed(format!(
                        "Symbol table section {symtab_section} has more than one SHT_SYMTAB_SHNDX attachment"
                    )));
                }
                attachment = Some(shdr);
            }
        }

        let Some(shndx_shdr) = attachment else {
            // No attachment section: every raw st_shndx must therefore be a
            // directly encodable value; SHN_XINDEX has nowhere to resolve.
            let sym_shdr = &section_headers[symtab_section];
            let sym_size = Sym::size(ctx.container);
            let raw_field_offset = match ctx.container {
                Container::Little => sym::sym32::SIZEOF_SYM - 2,
                Container::Big => 6,
            };
            let sym_bytes = bytes.pread_with::<&[u8]>(
                sym_shdr.sh_offset as usize,
                sym_shdr.sh_size as usize,
            )?;
            for index in 0..symbol_count {
                let raw = sym_bytes
                    .pread_with::<u16>(index * sym_size + raw_field_offset, ctx.le)?;
                if u32::from(raw) == section_header::SHN_XINDEX {
                    return Err(error::Error::Malformed(format!(
                        "Symbol {index} in table {symtab_section} uses SHN_XINDEX but no SHT_SYMTAB_SHNDX section is attached"
                    )));
                }
            }
            return Ok(None);
        };

        shndx_shdr.check_size(bytes.len())?;
        let expected_len = symbol_count.checked_mul(4).ok_or_else(|| {
            error::Error::Malformed("SHT_SYMTAB_SHNDX entry count overflow".into())
        })?;
        if shndx_shdr.sh_entsize != 0 && shndx_shdr.sh_entsize != 4 {
            return Err(error::Error::Malformed(format!(
                "SHT_SYMTAB_SHNDX section attached to symbol table {symtab_section} has sh_entsize {}, expected 4",
                shndx_shdr.sh_entsize
            )));
        }
        if shndx_shdr.sh_size as usize != expected_len {
            return Err(error::Error::Malformed(format!(
                "SHT_SYMTAB_SHNDX section attached to symbol table {symtab_section} has {} bytes, expected {expected_len} \
                 (one 32-bit entry for each of {symbol_count} symbols)",
                shndx_shdr.sh_size
            )));
        }

        let extended_bytes = bytes.pread_with::<&[u8]>(
            shndx_shdr.sh_offset as usize,
            shndx_shdr.sh_size as usize,
        )?;

        // The raw 16-bit section encodings live in the symbol table section.
        let sym_shdr = &section_headers[symtab_section];
        let sym_size = Sym::size(ctx.container);
        let sym_bytes = bytes.pread_with::<&[u8]>(
            sym_shdr.sh_offset as usize,
            sym_shdr.sh_size as usize,
        )?;
        let raw_field_offset = match ctx.container {
            Container::Little => sym::sym32::SIZEOF_SYM - 2,
            Container::Big => 6,
        };

        for index in 0..symbol_count {
            let value = extended_bytes
                .pread_with::<u32>(index * 4, ctx.le)?;
            let raw = sym_bytes
                .pread_with::<u16>(index * sym_size + raw_field_offset, ctx.le)?;
            let is_xindex = u32::from(raw) == section_header::SHN_XINDEX;
            if is_xindex {
                if value as usize >= section_headers.len() {
                    return Err(error::Error::Malformed(format!(
                        "SHT_SYMTAB_SHNDX entry {index} for symbol table {symtab_section} names section {}, \
                         but the file only has {} sections",
                        value,
                        section_headers.len()
                    )));
                }
            } else if value != 0 {
                return Err(error::Error::Malformed(format!(
                    "SHT_SYMTAB_SHNDX entry {index} for symbol table {symtab_section} is {value}, \
                     but the symbol's raw st_shndx is {raw:#06x} rather than SHN_XINDEX"
                )));
            }
        }

        Ok(Some(extended_bytes))
    }

    fn gnu_hash_len(bytes: &[u8], offset: usize, ctx: Ctx) -> error::Result<usize> {
        let buckets_num = bytes.pread_with::<u32>(offset, ctx.le)? as usize;
        let min_chain = bytes.pread_with::<u32>(offset + 4, ctx.le)? as usize;
        let bloom_size = bytes.pread_with::<u32>(offset + 8, ctx.le)? as usize;
        // We could handle min_chain==0 if we really had to, but it shouldn't happen.
        if buckets_num == 0 || min_chain == 0 || bloom_size == 0 {
            return Err(error::Error::Malformed(format!("Invalid DT_GNU_HASH: buckets_num={} min_chain={} bloom_size={}",
                                                       buckets_num, min_chain, bloom_size)));
        }
        // Find the last bucket.
        let buckets_offset = offset + 16 + bloom_size * if ctx.container.is_big() { 8 } else { 4 };
        let mut max_chain = 0;
        for bucket in 0..buckets_num {
            let chain = bytes.pread_with::<u32>(buckets_offset + bucket * 4, ctx.le)? as usize;
            if max_chain < chain {
                max_chain = chain;
            }
        }
        if max_chain < min_chain {
            return Ok(0);
        }
        // Find the last chain within the bucket.
        let mut chain_offset = buckets_offset + buckets_num * 4 + (max_chain - min_chain) * 4;
        loop {
            let hash = bytes.pread_with::<u32>(chain_offset, ctx.le)?;
            max_chain += 1;
            chain_offset += 4;
            if hash & 1 != 0 {
                return Ok(max_chain);
            }
        }
    }

    fn hash_len(bytes: &[u8], offset: usize, machine: u16, ctx: Ctx) -> error::Result<usize> {
        // Based on readelf code.
        let nchain = if (machine == header::EM_FAKE_ALPHA || machine == header::EM_S390) && ctx.container.is_big() {
            bytes.pread_with::<u64>(offset.saturating_add(4), ctx.le)? as usize
        } else {
            bytes.pread_with::<u32>(offset.saturating_add(4), ctx.le)? as usize
        };
        Ok(nchain)
    }

    struct Misc {
        is_64: bool,
        is_lib: bool,
        entry: u64,
        little_endian: bool,
        ctx: Ctx,
    }

    fn parse_misc(header: &Header) -> error::Result<Misc> {
        let entry = header.e_entry as usize;
        let is_lib = header.e_type == header::ET_DYN;
        let is_lsb = header.e_ident[header::EI_DATA] == header::ELFDATA2LSB;
        let endianness = scroll::Endian::from(is_lsb);
        let class = header.e_ident[header::EI_CLASS];
        if class != header::ELFCLASS64 && class != header::ELFCLASS32 {
            return Err(error::Error::Malformed(format!("Unknown values in ELF ident header: class: {} endianness: {}",
                                                        class,
                                                        header.e_ident[header::EI_DATA])));
        }
        let is_64 = class == header::ELFCLASS64;
        let container = if is_64 { Container::Big } else { Container::Little };
        let ctx = Ctx::new(container, endianness);

        Ok(Misc{
            is_64,
            is_lib,
            entry: entry as u64,
            little_endian:is_lsb,
            ctx,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_crt1_64bit() {
        let crt1: Vec<u8> = include!("../../etc/crt1.rs");
        match Elf::parse(&crt1) {
            Ok(binary) => {
                assert!(binary.is_64);
                assert!(!binary.is_lib);
                assert_eq!(binary.entry, 0);
                assert!(binary.syms.get(1000).is_none());
                assert!(binary.syms.get(5).is_some());
                let syms = binary.syms.to_vec();
                assert!(!binary.section_headers.is_empty());
                for (i, sym) in syms.iter().enumerate() {
                    if i == 11 {
                        let symtab = binary.strtab;
                        println!("sym: {:?}", &sym);
                        assert_eq!(&symtab[sym.st_name], "_start");
                        break;
                    }
                }
                assert!(!syms.is_empty());
            }
            Err(err) => {
                panic!("failed: {}", err);
            }
        }
    }

    #[test]
    fn parse_crt1_32bit() {
        let crt1: Vec<u8> = include!("../../etc/crt132.rs");
        match Elf::parse(&crt1) {
            Ok(binary) => {
                assert!(!binary.is_64);
                assert!(!binary.is_lib);
                assert_eq!(binary.entry, 0);
                assert!(binary.syms.get(1000).is_none());
                assert!(binary.syms.get(5).is_some());
                let syms = binary.syms.to_vec();
                assert!(!binary.section_headers.is_empty());
                for (i, sym) in syms.iter().enumerate() {
                    if i == 11 {
                        let symtab = binary.strtab;
                        println!("sym: {:?}", &sym);
                        assert_eq!(&symtab[sym.st_name], "__libc_csu_fini");
                        break;
                    }
                }
                assert!(!syms.is_empty());
            }
            Err(err) => {
                panic!("failed: {}", err);
            }
        }
    }

    // See https://github.com/m4b/goblin/issues/257
    #[test]
    #[allow(unused)]
    fn no_use_statement_conflict() {
        use crate::elf::section_header::*;
        use crate::elf::*;

        fn f(_: SectionHeader) {}
    }
}
