//! Inspect scattered relocations in a little-endian 32-bit Mach-O object.
//!
//! Usage: `scattered_layout <path-to-macho>`
//!
//! The file is only parsed; its contents are never executed or modified. The
//! program prints how scattered records relate to one another (including the
//! `GENERIC_RELOC_PAIR` records that are folded into difference relocations),
//! and then prints the field values under two caller supplied layouts: the
//! original layout and a layout in which every section is slid upwards.
use object::read::macho::MachOFile32;
use object::{LittleEndian, Object, ObjectSection, RelocationTarget, SectionIndex};

fn main() {
    let path = std::env::args()
        .nth(1)
        .expect("usage: scattered_layout <mach-o file>");
    let data = std::fs::read(&path).expect("failed to read file");
    let file = MachOFile32::<LittleEndian>::parse(data.as_slice())
        .expect("failed to parse a little-endian 32-bit Mach-O file");

    // The original section layout, keyed by the 1-based section index.
    let original: Vec<(SectionIndex, u64)> = file
        .sections()
        .map(|section| (section.index(), section.address()))
        .collect();

    println!("file: {path}");
    println!("sections:");
    for (index, address) in &original {
        if let Ok(section) = file.section_by_index(*index) {
            let name = section.name().unwrap_or("<non-utf8>");
            println!(
                "  [{index}] {name:<16} addr={address:#010x} size={:#x}",
                section.size()
            );
        }
    }

    println!("\nscattered relocations:");
    let mut any = false;
    for section in file.sections() {
        let section_name = section.name().unwrap_or("<non-utf8>").to_owned();
        let bytes = section.data().unwrap_or(&[]);
        for (offset, reloc) in section.relocations() {
            let RelocationTarget::Address(target) = reloc.target() else {
                continue;
            };
            any = true;
            let subtractor = match reloc.subtractor_target() {
                Some(RelocationTarget::Address(address)) => format!("{address:#x}"),
                Some(other) => format!("{other:?}"),
                None => "-".to_owned(),
            };
            let raw = read_field(bytes, offset, reloc.size());
            println!(
                "  section={section_name} field_offset={offset:#x} width={} pcrel={} \
                 target={target:#x} subtractor={subtractor} raw_field={raw:#x}",
                reloc.size(),
                matches!(reloc.flags(), object::RelocationFlags::MachO { r_pcrel: true, .. }),
            );
        }
    }
    if !any {
        println!("  (none)");
    }

    // Two independent layouts over the same sections.
    let identity = original.clone();
    let slid: Vec<(SectionIndex, u64)> = original
        .iter()
        .map(|(index, address)| (*index, address.wrapping_add(0x100_000)))
        .collect();

    for (label, layout) in [("identity", &identity), ("slid +0x100000", &slid)] {
        println!("\nfield values under {label} layout:");
        for section in file.sections() {
            let section_name = section.name().unwrap_or("<non-utf8>").to_owned();
            let map = object::RelocationMap::new_with_section_addresses(&file, &section, layout)
                .expect("failed to build relocation map");
            let bytes = section.data().unwrap_or(&[]);
            let mut printed = false;
            for (offset, reloc) in section.relocations() {
                if !matches!(reloc.target(), RelocationTarget::Address(_)) {
                    continue;
                }
                let raw = read_field(bytes, offset, reloc.size());
                let value = map.relocate(offset, raw);
                if !printed {
                    println!("  section {section_name}:");
                    printed = true;
                }
                println!(
                    "    field_offset={offset:#x} width={} raw={raw:#x} relocated={value:#x}",
                    reloc.size()
                );
            }
        }
    }
}

/// Read a little-endian field of the relocation's width from the section data.
fn read_field(bytes: &[u8], offset: u64, size: u8) -> u64 {
    let offset = offset as usize;
    match size {
        8 => *bytes.get(offset).unwrap_or(&0) as u64,
        16 => u16::from_le_bytes([
            *bytes.get(offset).unwrap_or(&0),
            *bytes.get(offset + 1).unwrap_or(&0),
        ]) as u64,
        32 => u32::from_le_bytes([
            *bytes.get(offset).unwrap_or(&0),
            *bytes.get(offset + 1).unwrap_or(&0),
            *bytes.get(offset + 2).unwrap_or(&0),
            *bytes.get(offset + 3).unwrap_or(&0),
        ]) as u64,
        _ => 0,
    }
}
