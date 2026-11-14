// Synthetic, never-executed ELF images exercising multiple symbol tables,
// SHT_SYMTAB_SHNDX attachments and relocation symbol resolution.
#[path = "../task_support/check.rs"]
mod check;
#[path = "../task_support/fixture.rs"]
mod fixture;

use check::run;
use fixture::{Format, Program, Section};
use goblin::container::{Container, Ctx};
use goblin::elf::section_header::{
    SHT_DYNSYM, SHT_PROGBITS, SHT_REL, SHT_RELA, SHT_STRTAB, SHT_SYMTAB, SHT_SYMTAB_SHNDX,
};
use goblin::elf::sym::SymbolSection;
use goblin::elf::{Elf, Symtab};
use goblin::options::ParseOptions;
use scroll::{Endian, Pread, Pwrite};

const SHN_ABS: u16 = 0xfff1;
const SHN_COMMON: u16 = 0xfff2;
const SHN_XINDEX: u16 = 0xffff;
const SHN_LOPROC: u16 = 0xff00;

fn formats() -> impl Iterator<Item = Format> {
    [false, true].into_iter().flat_map(|wide| {
        [false, true].into_iter().map(move |little| Format { wide, little })
    })
}

fn ctx_of(f: Format) -> Ctx {
    Ctx::new(
        if f.wide { Container::Big } else { Container::Little },
        Endian::from(f.little),
    )
}

fn symtab_section(f: Format, link: u32, info: u32, data: Vec<u8>) -> Section {
    Section {
        kind: SHT_SYMTAB,
        link,
        info,
        entsize: f.sym_size() as u64,
        data,
        ..Section::default()
    }
}

fn dynsym_section(f: Format, link: u32, info: u32, data: Vec<u8>) -> Section {
    Section {
        kind: SHT_DYNSYM,
        link,
        info,
        entsize: f.sym_size() as u64,
        data,
        ..Section::default()
    }
}

fn shndx_section(link: u32, entries: &[u32], f: Format) -> Section {
    Section {
        kind: SHT_SYMTAB_SHNDX,
        link,
        entsize: 4,
        data: f.words(entries),
        ..Section::default()
    }
}

/// The large image from the baseline: two symbol tables with attachments,
/// where the dynamic table's extended index (65521 == SHN_ABS numerically)
/// lands inside the reserved 16-bit range.
fn huge_sections(f: Format) -> Vec<Section> {
    let mut sections = fixture::ordinary(f);
    sections[3].data = [f.symbol(0, 0, 0), f.symbol(1, SHN_XINDEX, 7)].concat();
    sections.push(shndx_section(3, &[0, 65536], f));
    sections.push(Section {
        kind: SHT_STRTAB,
        data: b"\0beta\0".to_vec(),
        ..Section::default()
    });
    sections.push(dynsym_section(
        f,
        5,
        1,
        [f.symbol(0, 0, 0), f.symbol(1, SHN_XINDEX, 9)].concat(),
    ));
    sections.push(shndx_section(6, &[0, 65521], f));
    sections.resize(65537, Section::default());
    sections[65536].kind = SHT_PROGBITS;
    sections[65521].kind = SHT_PROGBITS;
    sections
}

#[test]
fn attachments() {
    run("attachments", |c| {
        for f in formats() {
            let tag = |msg: &str| format!("{msg} (wide={}, little={})", f.wide, f.little);

            // Valid attachments on a >65535-section image resolve XINDEX.
            let bytes = f.image(&huge_sections(f));
            let elf = Elf::parse(&bytes).unwrap();
            let sym = elf.syms.get(1).unwrap();
            c.check(sym.st_shndx == 65536, &tag("symtab XINDEX resolves through get"));
            c.check(
                elf.syms.iter().nth(1).unwrap().st_shndx == 65536,
                &tag("symtab XINDEX resolves through iter"),
            );
            c.check(
                elf.syms.to_vec()[1].st_shndx == 65536,
                &tag("symtab XINDEX resolves through to_vec"),
            );
            c.check(
                elf.syms.raw_shndx(1) == Some(SHN_XINDEX),
                &tag("raw 16-bit encoding is still SHN_XINDEX"),
            );
            c.check(
                elf.syms.section_kind(1) == Some(SymbolSection::Section(65536)),
                &tag("extended index is an ordinary Section"),
            );
            let dsym = elf.dynsyms.get(1).unwrap();
            c.check(
                dsym.st_shndx == 65521,
                &tag("dynsym XINDEX resolves through attachment"),
            );
            c.check(
                elf.dynsyms.raw_shndx(1) == Some(SHN_XINDEX),
                &tag("dynsym raw encoding is SHN_XINDEX"),
            );
            c.check(
                elf.dynsyms.section_kind(1) == Some(SymbolSection::Section(65521)),
                &tag("extended 65521 stays Section even though it equals SHN_ABS numerically"),
            );
            c.check(elf.syms.has_extended_sections(), &tag("symtab attachment visible"));

            // Small image: XINDEX pointing at an ordinary small section.
            let small = vec![
                Section::default(),
                Section { kind: SHT_PROGBITS, data: vec![0; 4], ..Section::default() },
                Section { kind: SHT_STRTAB, data: b"\0x\0".to_vec(), ..Section::default() },
                symtab_section(f, 2, 1, [f.symbol(0, 0, 0), f.symbol(1, SHN_XINDEX, 1)].concat()),
                shndx_section(3, &[0, 1], f),
            ];
            let small_bytes = f.image(&small);
            let elf = Elf::parse(&small_bytes).unwrap();
            c.check(
                elf.syms.get(1).unwrap().st_shndx == 1,
                &tag("small-image XINDEX resolves to section 1"),
            );
            c.check(
                elf.syms.section_kind(1) == Some(SymbolSection::Section(1)),
                &tag("small-image extended section kind"),
            );

            // XINDEX without an attachment is a strict parse error.
            let mut missing = small.clone();
            missing.remove(4);
            c.check(
                Elf::parse(&f.image(&missing)).is_err(),
                &tag("XINDEX without attachment rejected"),
            );

            // Attachment with the wrong number of entries.
            let mut bad_len = small.clone();
            bad_len[4] = shndx_section(3, &[0], f);
            c.check(
                Elf::parse(&f.image(&bad_len)).is_err(),
                &tag("wrong-length attachment rejected"),
            );

            // Extended value naming a section that does not exist.
            let mut bad_idx = small.clone();
            bad_idx[4] = shndx_section(3, &[0, 99], f);
            c.check(
                Elf::parse(&f.image(&bad_idx)).is_err(),
                &tag("out-of-bounds extended index rejected"),
            );

            // A non-zero extension entry for a symbol that is not XINDEX.
            let mut nonzero = small.clone();
            nonzero[3] = symtab_section(f, 2, 1, [f.symbol(0, 0, 0), f.symbol(1, 1, 1)].concat());
            nonzero[4] = shndx_section(3, &[0, 7], f);
            c.check(
                Elf::parse(&f.image(&nonzero)).is_err(),
                &tag("non-zero entry without XINDEX rejected"),
            );

            // Two attachments claiming the same symbol table.
            let mut dup = small.clone();
            dup.push(shndx_section(3, &[0, 1], f));
            c.check(
                Elf::parse(&f.image(&dup)).is_err(),
                &tag("duplicate attachment rejected"),
            );

            // Attachment whose sh_link does not name a symbol table.
            let mut bad_link = small.clone();
            bad_link[4] = shndx_section(2, &[0, 1], f);
            c.check(
                Elf::parse(&f.image(&bad_link)).is_err(),
                &tag("attachment with non-symbol-table sh_link rejected"),
            );

            // Permissive mode must not panic or error on the malformed files.
            let permissive = ParseOptions::permissive();
            c.check(
                Elf::parse_with_opts(&f.image(&missing), &permissive).is_ok(),
                &tag("permissive tolerates missing attachment"),
            );
            c.check(
                Elf::parse_with_opts(&f.image(&bad_len), &permissive).is_ok(),
                &tag("permissive tolerates bad attachment length"),
            );
            c.check(
                Elf::parse_with_opts(&f.image(&bad_idx), &permissive).is_ok(),
                &tag("permissive tolerates bad extended index"),
            );
        }
    });
}

#[test]
fn identities() {
    run("identities", |c| {
        for f in formats() {
            let tag = |msg: &str| format!("{msg} (wide={}, little={})", f.wide, f.little);

            // One symbol per defining-section kind.
            let strs = b"\0undef\0abs\0common\0reserved\0ordinary\0".to_vec();
            let syms = [
                f.symbol(0, 0, 0),
                f.symbol(1, 0, 0),             // SHN_UNDEF
                f.symbol(7, SHN_ABS, 3),       // SHN_ABS
                f.symbol(11, SHN_COMMON, 4),   // SHN_COMMON
                f.symbol(18, SHN_LOPROC, 5),   // reserved processor index
                f.symbol(27, 1, 6),            // ordinary section 1
            ]
            .concat();
            let sections = vec![
                Section::default(),
                Section { kind: SHT_PROGBITS, data: vec![0; 8], ..Section::default() },
                Section { kind: SHT_STRTAB, data: strs, ..Section::default() },
                symtab_section(f, 2, 1, syms),
            ];
            let identity_bytes = f.image(&sections);
            let elf = Elf::parse(&identity_bytes).unwrap();
            let expected = [
                (0u16, SymbolSection::Undefined),
                (0, SymbolSection::Undefined),
                (SHN_ABS, SymbolSection::Absolute),
                (SHN_COMMON, SymbolSection::Common),
                (SHN_LOPROC, SymbolSection::Reserved(SHN_LOPROC)),
                (1, SymbolSection::Section(1)),
            ];
            for (index, (raw, kind)) in expected.iter().enumerate() {
                let raw_msg = tag(&format!("raw shndx of symbol {index}"));
                c.check(elf.syms.raw_shndx(index) == Some(*raw), &raw_msg);
                let kind_msg = tag(&format!("section kind of symbol {index}"));
                c.check(elf.syms.section_kind(index) == Some(*kind), &kind_msg);
                let sym_msg = tag(&format!("Sym::symbol_section of symbol {index}"));
                c.check(
                    elf.syms.get(index).unwrap().symbol_section() == *kind,
                    &sym_msg,
                );
            }
            // The null symbol is retained.
            c.check(elf.syms.get(0).is_some(), &tag("null symbol retained"));
            c.check(elf.syms.len() == 6, &tag("all symbols counted including null"));
            c.check(
                elf.strtab.get_at(elf.syms.get(2).unwrap().st_name) == Some("abs"),
                &tag("name lookup through the table's own string table"),
            );

            // Two tables, distinct string tables; index 1 means different things.
            let str_a = b"\0shared\0only_a\0".to_vec();
            let str_b = b"\0shared\0".to_vec();
            let two = vec![
                Section::default(),
                Section { kind: SHT_PROGBITS, data: vec![1], ..Section::default() },
                Section { kind: SHT_PROGBITS, data: vec![2], ..Section::default() },
                Section { kind: SHT_STRTAB, data: str_a, ..Section::default() },
                symtab_section(
                    f,
                    3,
                    1,
                    [f.symbol(0, 0, 0), f.symbol(1, 1, 10), f.symbol(8, 1, 11)].concat(),
                ),
                Section { kind: SHT_STRTAB, data: str_b, ..Section::default() },
                dynsym_section(
                    f,
                    5,
                    2,
                    [f.symbol(0, 0, 0), f.symbol(1, 2, 20)].concat(),
                ),
            ];
            let two_bytes = f.image(&two);
            let elf = Elf::parse(&two_bytes).unwrap();
            let tables = elf.symbol_tables();
            c.check(tables.len() == 2, &tag("both physical tables registered"));
            c.check(
                tables.iter().all(|t| t.section == 4 || t.section == 6),
                &tag("registry keyed by physical section index"),
            );
            c.check(
                elf.symbol_table(4).is_some() && elf.symbol_table(6).is_some(),
                &tag("symbol_table accessor finds both"),
            );
            c.check(elf.symbol_table(3).is_none(), &tag("non-symbol section returns None"));

            let a = elf.symbol_table(4).unwrap();
            let b = elf.symbol_table(6).unwrap();
            let sa = a.symtab.get(1).unwrap();
            let sb = b.symtab.get(1).unwrap();
            c.check(sa.st_shndx == 1 && sb.st_shndx == 2, &tag("same index, different sections"));
            c.check(
                a.strtab.get_at(sa.st_name) == Some("shared")
                    && b.strtab.get_at(sb.st_name) == Some("shared"),
                &tag("same name resolved through distinct string tables"),
            );
            c.check(a.symtab.len() == 3 && b.symtab.len() == 2, &tag("table counts not merged"));
            c.check(
                elf.syms.len() == 3 && elf.dynsyms.len() == 2,
                &tag("syms/dynsyms keep their own identities"),
            );

            // A table without an attachment reports raw SHN_ABS as Absolute.
            c.check(
                elf.symbol_table(4)
                    .unwrap()
                    .symtab
                    .section_kind(0)
                    == Some(SymbolSection::Undefined),
                &tag("unattached table still classifies its symbols"),
            );
        }
    });
}

#[test]
fn relocations() {
    run("relocations", |c| {
        for f in formats() {
            let tag = |msg: &str| format!("{msg} (wide={}, little={})", f.wide, f.little);

            let str_a = b"\0alpha\0".to_vec();
            let str_b = b"\0beta\0".to_vec();
            let sections = vec![
                Section::default(),                                              // 0
                Section { kind: SHT_PROGBITS, data: vec![0; 8], ..Section::default() }, // 1 target A
                Section { kind: SHT_PROGBITS, data: vec![0; 8], ..Section::default() }, // 2 target B
                Section { kind: SHT_STRTAB, data: str_a, ..Section::default() }, // 3
                symtab_section(f, 3, 1, [f.symbol(0, 0, 0), f.symbol(1, 1, 7)].concat()), // 4
                Section { kind: SHT_STRTAB, data: str_b, ..Section::default() }, // 5
                dynsym_section(f, 5, 2, [f.symbol(0, 0, 0), f.symbol(1, 2, 9)].concat()), // 6
                Section { // 7 RELA -> symtab 4, applying to section 1
                    kind: SHT_RELA, link: 4, info: 1,
                    entsize: f.rel_size(true) as u64,
                    data: f.relocation(1, Some(11)),
                    ..Section::default()
                },
                Section { // 8 REL -> dynsym 6, applying to section 2
                    kind: SHT_REL, link: 6, info: 2,
                    entsize: f.rel_size(false) as u64,
                    data: f.relocation(1, None),
                    ..Section::default()
                },
                Section { // 9 RELA referencing the null symbol
                    kind: SHT_RELA, link: 4, info: 1,
                    entsize: f.rel_size(true) as u64,
                    data: f.relocation(0, Some(-3)),
                    ..Section::default()
                },
            ];
            let reloc_bytes = f.image(&sections);
            let elf = Elf::parse(&reloc_bytes).unwrap();

            let rela = elf.reloc_symbol(7, 0).unwrap().unwrap();
            c.check(rela.target_section == 1, &tag("RELA target section from sh_info"));
            c.check(rela.symtab_section == 4, &tag("RELA follows its own sh_link"));
            c.check(rela.symbol_index == 1, &tag("RELA carries r_sym"));
            c.check(rela.name == Some("alpha"), &tag("RELA symbol name"));
            c.check(
                rela.section == SymbolSection::Section(1),
                &tag("RELA symbol defining section kind"),
            );
            c.check(rela.reloc_section == 7 && rela.reloc_index == 0, &tag("RELA coordinates echoed"));

            let rel = elf.reloc_symbol(8, 0).unwrap().unwrap();
            c.check(rel.target_section == 2, &tag("REL target section"));
            c.check(rel.symtab_section == 6, &tag("REL follows dynsym sh_link, not elf.syms"));
            c.check(rel.name == Some("beta"), &tag("REL symbol name from dynstr"));
            c.check(
                rel.section == SymbolSection::Section(2),
                &tag("REL symbol defining section kind"),
            );

            let null_ref = elf.reloc_symbol(9, 0).unwrap().unwrap();
            c.check(null_ref.symbol_index == 0, &tag("null symbol reference kept"));
            c.check(
                null_ref.name.unwrap_or("").is_empty(),
                &tag("null symbol has an empty name"),
            );
            c.check(
                null_ref.section == SymbolSection::Undefined,
                &tag("null symbol is Undefined"),
            );

            // Unknown section/record coordinates are Ok(None), not errors.
            c.check(
                elf.reloc_symbol(7, 4).unwrap().is_none(),
                &tag("record past the end is Ok(None)"),
            );
            c.check(
                elf.reloc_symbol(40, 0).unwrap().is_none(),
                &tag("non-relocation section coordinate is Ok(None)"),
            );

            // Internally broken references are errors from the query, not panics.
            let mut bad_sym = sections.clone();
            bad_sym[7].data = f.relocation(9, Some(0)); // r_sym past table end
            let bad_sym_bytes = f.image(&bad_sym);
            let elf_bad = Elf::parse(&bad_sym_bytes).unwrap();
            c.check(
                elf_bad.reloc_symbol(7, 0).is_err(),
                &tag("r_sym past the table returns Error"),
            );

            let mut bad_link = sections.clone();
            bad_link[7].link = 3; // a string table, not a symbol table
            let bad_link_bytes = f.image(&bad_link);
            let elf_bad = Elf::parse(&bad_link_bytes).unwrap();
            c.check(
                elf_bad.reloc_symbol(7, 0).is_err(),
                &tag("sh_link to a non-symbol-table returns Error"),
            );

            // shdr_relocs keeps section order and the raw record contents.
            let order: Vec<_> = elf.shdr_relocs.iter().map(|(idx, _)| *idx).collect();
            c.check(order == vec![7, 8, 9], &tag("shdr_relocs section order preserved"));
            let (_, rela_records) = &elf.shdr_relocs[0];
            let record = rela_records.get(0).unwrap();
            c.check(record.r_offset == 4, &tag("r_offset preserved"));
            c.check(record.r_type == 1, &tag("r_type preserved"));
            c.check(record.r_addend == Some(11), &tag("RELA addend preserved"));
            let (_, rel_records) = &elf.shdr_relocs[1];
            c.check(
                rel_records.get(0).unwrap().r_addend.is_none(),
                &tag("REL addend stays absent"),
            );
        }
    });
}

#[test]
fn compatibility() {
    run("compatibility", |c| {
        for f in formats() {
            let tag = |msg: &str| format!("{msg} (wide={}, little={})", f.wide, f.little);

            // The original ordinary image reads exactly as before.
            let bytes = f.image(&fixture::ordinary(f));
            let elf = Elf::parse(&bytes).unwrap();
            let symbol = elf.syms.get(1).unwrap();
            c.check(symbol.st_shndx == 1, &tag("legacy st_shndx unchanged"));
            c.check(
                elf.strtab.get_at(symbol.st_name) == Some("alpha"),
                &tag("legacy strtab lookup unchanged"),
            );
            c.check(
                elf.syms.iter().nth(1).unwrap() == symbol && elf.syms.to_vec()[1] == symbol,
                &tag("get/iter/to_vec agree as before"),
            );
            c.check(
                elf.syms.raw_shndx(1) == Some(1) && !elf.syms.has_extended_sections(),
                &tag("legacy table has no attachment and raw equals resolved"),
            );
            c.check(elf.dynsyms.is_empty(), &tag("legacy image still has no dynamic symbols"));

            // A standalone Symtab::parse keeps the old raw XINDEX behaviour.
            let raw_syms = [f.symbol(0, 0, 0), f.symbol(1, SHN_XINDEX, 7)].concat();
            let table = Symtab::parse(&raw_syms, 0, 2, ctx_of(f)).unwrap();
            c.check(
                !table.has_extended_sections(),
                &tag("Symtab::parse produces an unattached table"),
            );
            c.check(
                table.get(1).unwrap().st_shndx == SHN_XINDEX as usize,
                &tag("unattached Symtab::parse returns raw SHN_XINDEX"),
            );
            c.check(
                table.section_kind(1) == Some(SymbolSection::Reserved(SHN_XINDEX)),
                &tag("unattached raw XINDEX classifies as Reserved"),
            );

            // Low-level unified Sym byte encoding round-trips the raw value.
            use goblin::elf::Sym;
            let sym_size = Sym::size(ctx_of(f).container);
            let mut buffer = vec![0u8; sym_size];
            let original = Sym {
                st_name: 5,
                st_info: 0x10,
                st_other: 0,
                st_shndx: SHN_XINDEX as usize,
                st_value: 42,
                st_size: 7,
            };
            buffer.pwrite_with(original, 0, ctx_of(f)).unwrap();
            let decoded: Sym = buffer.pread_with(0, ctx_of(f)).unwrap();
            c.check(
                decoded.st_shndx == SHN_XINDEX as usize && decoded.st_value == 42,
                &tag("low-level Sym codec preserves raw st_shndx"),
            );

            // No PT_DYNAMIC: dynsyms/dynstrtab come from the SHT_DYNSYM section.
            let huge = huge_sections(f);
            let huge_bytes = f.image(&huge);
            let elf = Elf::parse(&huge_bytes).unwrap();
            c.check(elf.dynsyms.len() == 2, &tag("section-sourced dynsyms count"));
            c.check(
                elf.dynstrtab.get_at(elf.dynsyms.get(1).unwrap().st_name) == Some("beta"),
                &tag("section-sourced dynstrtab"),
            );
            c.check(
                elf.dynsyms.get(1).unwrap().st_shndx == 65521,
                &tag("section-sourced dynsyms carry resolved indices"),
            );
        }

        // PT_DYNAMIC path: the attachment is reused only when the section
        // describes the exact same byte range and symbol count.
        for f in formats() {
            let tag = |msg: &str| format!("{msg} (wide={}, little={})", f.wide, f.little);
            let sym_size = f.sym_size();
            let dynstr = b"\0gamma\0".to_vec();
            let dynsym_data = [f.symbol(0, 0, 0), f.symbol(1, SHN_XINDEX, 3)].concat();
            let hash_data = f.hash(2);
            // Six dynamic entries (including the terminating DT_NULL); the
            // SHT_DYNAMIC section's size is fixed before offsets are computed.
            let dynamic_size = 6 * if f.wide { 16 } else { 8 };
            let mut sections = vec![
                Section::default(),
                Section { kind: SHT_STRTAB, data: dynstr, ..Section::default() },
                dynsym_section(f, 1, 1, dynsym_data.clone()),
                shndx_section(2, &[0, 1], f),
                Section { kind: 5, entsize: 4, data: hash_data, ..Section::default() },
                Section { kind: 6, data: vec![0u8; dynamic_size], ..Section::default() },
            ];
            let phnum = 2;
            let offsets = f.offsets(&sections, phnum);
            let (dynstr_off, dynsym_off, hash_off) = (offsets[1], offsets[2], offsets[4]);
            sections[5].data = [
                f.dynamic(4, hash_off as u64),
                f.dynamic(5, dynstr_off as u64),
                f.dynamic(10, sections[1].data.len() as u64),
                f.dynamic(6, dynsym_off as u64),
                f.dynamic(11, sym_size as u64),
                f.dynamic(0, 0),
            ]
            .concat();
            let dynamic_off = offsets[5];
            let file_size = f.file_size(&sections, phnum);
            let phdrs = [
                Program {
                    kind: 1, offset: 0, vaddr: 0, filesz: file_size as u64,
                    memsz: file_size as u64, align: 1, ..Program::default()
                },
                Program {
                    kind: 2, offset: dynamic_off as u64, vaddr: dynamic_off as u64,
                    filesz: sections[5].data.len() as u64, memsz: sections[5].data.len() as u64,
                    align: 1, ..Program::default()
                },
            ];
            let mut bytes = f.image_with(&sections, &phdrs);
            f.set_shstrndx(&mut bytes, 1);
            let elf = Elf::parse(&bytes).unwrap();
            c.check(elf.dynamic.is_some(), &tag("PT_DYNAMIC recognised"));
            c.check(elf.dynsyms.len() == 2, &tag("dynamic symbol count preserved"));
            c.check(
                elf.dynsyms.get(1).unwrap().st_shndx == 1,
                &tag("matching section attachment applied under PT_DYNAMIC"),
            );
            c.check(
                elf.dynsyms.has_extended_sections(),
                &tag("dynsym attachment visible under PT_DYNAMIC"),
            );

            // Mismatch: declare only one symbol in the section header while the
            // PT_DYNAMIC table still spans two, without moving any data. The
            // attachment section is shrunk consistently so the PT_DYNAMIC range
            // check itself is what rejects the image.
            let mut bad_bytes = f.image_with(&sections, &phdrs);
            f.set_shstrndx(&mut bad_bytes, 1);
            f.set_section_size(&mut bad_bytes, phnum, 2, sym_size as u64);
            f.set_section_size(&mut bad_bytes, phnum, 3, 4);
            c.check(
                Elf::parse(&bad_bytes).is_err(),
                &tag("PT_DYNAMIC byte-range mismatch rejected strictly"),
            );
            c.check(
                Elf::parse_with_opts(&bad_bytes, &ParseOptions::permissive()).is_ok(),
                &tag("PT_DYNAMIC mismatch tolerated in permissive mode"),
            );
        }
    });
}
