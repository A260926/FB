// Demonstrates that the two symbol tables keep their own identities: symbol
// number 1 in each table names a different section and is resolved through the
// table's own string table. Synthetic bytes only, never executed.
#[allow(dead_code)]
#[path = "../task_support/fixture.rs"]
mod fixture;

use fixture::{Format, Section};
use goblin::elf::section_header::{SHT_DYNSYM, SHT_PROGBITS, SHT_STRTAB, SHT_SYMTAB};

fn main() {
    let f = Format { wide: true, little: true };

    let symtab_strings = b"\0shared\0only_static\0".to_vec();
    let dynsym_strings = b"\0shared\0".to_vec();

    let symtab = Section {
        kind: SHT_SYMTAB,
        link: 3,
        info: 1,
        entsize: f.sym_size() as u64,
        // symbol 1: "shared", defined in section 1
        data: [f.symbol(0, 0, 0), f.symbol(1, 1, 0x1000), f.symbol(8, 1, 0x1008)].concat(),
        ..Section::default()
    };
    let dynsym = Section {
        kind: SHT_DYNSYM,
        link: 5,
        info: 2,
        entsize: f.sym_size() as u64,
        // symbol 1: "shared" again, but defined in section 2
        data: [f.symbol(0, 0, 0), f.symbol(1, 2, 0x2000)].concat(),
        ..Section::default()
    };

    let sections = vec![
        Section::default(),
        Section { kind: SHT_PROGBITS, data: vec![0; 8], ..Section::default() },
        Section { kind: SHT_PROGBITS, data: vec![0; 8], ..Section::default() },
        Section { kind: SHT_STRTAB, data: symtab_strings, ..Section::default() },
        symtab,
        Section { kind: SHT_STRTAB, data: dynsym_strings, ..Section::default() },
        dynsym,
    ];

    let bytes = f.image(&sections);
    let elf = goblin::elf::Elf::parse(&bytes).expect("synthetic image must parse");

    println!("physical symbol table sections:");
    for table in elf.symbol_tables() {
        println!(
            "  section {} with {} symbols and its own string table",
            table.section,
            table.symtab.len()
        );
    }

    for section in [4usize, 6usize] {
        let table = elf
            .symbol_table(section)
            .expect("the section is a symbol table");
        let symbol = table.symtab.get(1).expect("each table keeps a symbol 1");
        let name = table.strtab.get_at(symbol.st_name).unwrap_or("");
        let kind = table.symtab.section_kind(1).unwrap();
        println!(
            "table section {section}: symbol index 1 is `{name}` defining section = {:?} (st_shndx={:#x})",
            kind, symbol.st_shndx
        );
    }

    let static_one = elf.symbol_table(4).unwrap().symtab.get(1).unwrap();
    let dynamic_one = elf.symbol_table(6).unwrap().symtab.get(1).unwrap();
    assert_ne!(
        static_one.st_shndx, dynamic_one.st_shndx,
        "the same symbol index must not be merged across tables"
    );
    assert_eq!(elf.symbol_table(4).unwrap().symtab.len(), 3);
    assert_eq!(elf.symbol_table(6).unwrap().symtab.len(), 2);
    println!("EXAMPLE_OK: same index, two distinct definitions; tables were not merged");
}
