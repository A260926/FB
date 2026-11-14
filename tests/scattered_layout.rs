//! Regression tests for scattered relocations in little-endian 32-bit Mach-O.
//!
//! All object files used here are small, legal, synthetic `MH_OBJECT` files
//! built in this test. Their section data is inert; the tests only parse
//! relocation metadata and never execute any contents.

use object::read::macho::MachOFile32;
use object::{
    LittleEndian, Object, ObjectSection, RelocationFlags, RelocationKind, RelocationTarget,
    SectionIndex,
};

#[path = "../task_support/check.rs"]
mod check;
use check::{check, run};

type Relocs = Vec<(u32, u32)>;

/// A synthetic section specification: name, original address, size and the
/// raw relocation entries attached to the section.
struct Spec {
    name: &'static str,
    addr: u32,
    size: u32,
    relocs: Relocs,
}

fn word(v: &mut Vec<u8>, n: u32) {
    v.extend_from_slice(&n.to_le_bytes());
}

fn name(v: &mut Vec<u8>, s: &str) {
    assert!(s.len() <= 16);
    let start = v.len();
    v.extend_from_slice(s.as_bytes());
    v.resize(start + 16, 0);
}

/// Build a scattered relocation entry.
fn scattered(r_type: u8, r_length: u8, r_pcrel: bool, r_address: u32, r_value: u32) -> (u32, u32) {
    let word0 = 0x8000_0000
        | ((r_type as u32) << 24)
        | ((r_length as u32) << 28)
        | ((r_pcrel as u32) << 30)
        | (r_address & 0x00ff_ffff);
    (word0, r_value)
}

/// Build a plain local vanilla relocation entry against section ordinal 1.
fn plain_section(r_address: u32, r_length: u8) -> (u32, u32) {
    (r_address, (1u32) | ((r_length as u32) << 25))
}

/// Assemble a little-endian i386 `MH_OBJECT` containing the given sections.
fn build_object(specs: &[Spec], data: &[(usize, Vec<u8>)]) -> Vec<u8> {
    let nsections = specs.len() as u32;
    // A segment_command is a 56 byte header followed by 68 byte section headers.
    let sizeofcmds = 56 + nsections * 68;
    let mut v = Vec::new();
    // mach_header: magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags.
    for n in [0xfeedfaceu32, 7, 3, 1, 1, sizeofcmds, 0] {
        word(&mut v, n);
    }
    // LC_SEGMENT
    word(&mut v, 1);
    word(&mut v, sizeofcmds);
    name(&mut v, "__TEST");
    word(&mut v, 0); // vmaddr
    word(&mut v, 0); // vmsize
    word(&mut v, 0); // fileoff
    word(&mut v, 0); // filesize
    word(&mut v, 7); // maxprot
    word(&mut v, 7); // initprot
    word(&mut v, nsections);
    word(&mut v, 0); // flags

    // Section data starts immediately after the load commands.
    let mut cursor = v.len() as u32 + nsections * 68;
    let mut layout = Vec::new();
    for spec in specs {
        let offset = cursor;
        cursor += spec.size;
        layout.push((spec.name, spec.addr, spec.size, offset));
    }
    // Relocation tables follow the section data.
    let mut reloc_layout = Vec::new();
    for spec in specs {
        let reloff = if spec.relocs.is_empty() {
            0
        } else {
            let reloff = cursor;
            cursor += (spec.relocs.len() as u32) * 8;
            reloff
        };
        reloc_layout.push(reloff);
    }

    for (index, spec) in specs.iter().enumerate() {
        let (_name, addr, size, offset) = layout[index];
        name(&mut v, spec.name);
        name(&mut v, "__TEST");
        for n in [
            addr,
            size,
            offset,
            0, // align
            reloc_layout[index],
            spec.relocs.len() as u32,
            0, // flags
            0,
            0,
        ] {
            word(&mut v, n);
        }
    }
    assert_eq!(v.len() as u32, layout.first().map(|l| l.3).unwrap_or(0));

    // Section data, padded out to each section's size.
    for (index, (_name, _addr, size, offset)) in layout.iter().enumerate() {
        if v.len() < *offset as usize {
            v.resize(*offset as usize, 0);
        }
        let mut blob = data
            .iter()
            .find(|(which, _)| *which == index)
            .map(|(_, bytes)| bytes.clone())
            .unwrap_or_default();
        blob.resize(*size as usize, 0);
        v.extend_from_slice(&blob);
    }
    // Relocation entries.
    for spec in specs {
        for (word0, word1) in &spec.relocs {
            word(&mut v, *word0);
            word(&mut v, *word1);
        }
    }
    assert_eq!(v.len() as u32, cursor);
    v
}

/// The main synthetic object: vanilla (all three widths), a SECTDIFF/PAIR,
/// a LOCAL_SECTDIFF/PAIR and a trailing plain record.
fn diff_object() -> Vec<u8> {
    let relocs = vec![
        // 32-bit vanilla, non-PC-relative, target inside section 1.
        scattered(0, 2, false, 0, 0x1004),
        // 32-bit vanilla, PC-relative, target inside section 2.
        scattered(0, 2, true, 4, 0x2004),
        // SECTDIFF (section 1 end minus section 2 end) followed by its PAIR.
        scattered(2, 2, false, 8, 0x1010),
        scattered(1, 2, false, 0x00aabbcc, 0x2010),
        // LOCAL_SECTDIFF at 16-bit width, followed by its PAIR.
        scattered(4, 1, false, 12, 0x1020),
        scattered(1, 1, false, 0x00ddeeff, 0x2020),
        // 8-bit vanilla.
        scattered(0, 0, false, 16, 0x1008),
        // A plain record after the pairs must still surface normally.
        plain_section(20, 2),
    ];
    let mut debug_data = vec![0u8; 0x100];
    debug_data[0..4].copy_from_slice(&0x1009u32.to_le_bytes());
    debug_data[4..8].copy_from_slice(&0x10u32.to_le_bytes());
    debug_data[8..12].copy_from_slice(&0xffff_f000u32.to_le_bytes());
    debug_data[12..14].copy_from_slice(&0xf000u16.to_le_bytes());
    debug_data[16] = 0x09;
    debug_data[20..24].copy_from_slice(&7u32.to_le_bytes());
    let specs = vec![
        Spec {
            name: "__text",
            addr: 0x1000,
            size: 0x100,
            relocs: Vec::new(),
        },
        Spec {
            name: "__data",
            addr: 0x2000,
            size: 0x100,
            relocs: Vec::new(),
        },
        Spec {
            name: "__debug",
            addr: 0x3000,
            size: 0x100,
            relocs,
        },
    ];
    build_object(&specs, &[(2, debug_data)])
}

fn parse(bytes: &[u8]) -> MachOFile32<'_, LittleEndian> {
    MachOFile32::<LittleEndian>::parse(bytes).unwrap()
}

fn debug_relocs<'data>(
    file: &MachOFile32<'data, LittleEndian>,
) -> Vec<(u64, object::Relocation)> {
    let section = file.section_by_name("__debug").unwrap();
    section.relocations().collect()
}

#[test]
fn scattered_records() {
    run("scattered_records", || {
        // The delivered legal fixture exposes its scattered vanilla record.
        let delivered = std::fs::read("task_support/basic.macho").unwrap();
        let file = parse(&delivered);
        let section = file.section_by_name("__debug_info").unwrap();
        let relocs: Vec<_> = section.relocations().collect();
        let vanilla = relocs.iter().find(|(offset, _)| *offset == 0).unwrap();
        check(vanilla.1.size() == 32);
        check(vanilla.1.kind() == RelocationKind::Absolute);
        check(matches!(
            vanilla.1.target(),
            RelocationTarget::Address(0x1004)
        ));
        // r_value is retained as the target identity.
        check(vanilla.1.subtractor().is_none());
        check(vanilla.1.subtractor_target().is_none());
        check(vanilla.1.has_implicit_addend());
        check(matches!(
            vanilla.1.flags(),
            RelocationFlags::MachO {
                r_type,
                r_pcrel: false,
                r_length: 2
            } if r_type == object::macho::GENERIC_RELOC_VANILLA
        ));
        // The plain record in the same section is unchanged.
        let plain = relocs.iter().find(|(offset, _)| *offset == 4).unwrap();
        check(matches!(
            plain.1.target(),
            RelocationTarget::Section(SectionIndex(1))
        ));

        // The constructed object exercises pairs and every field width.
        let bytes = diff_object();
        let file = parse(&bytes);
        let relocs = debug_relocs(&file);
        // Eight raw entries collapse to six relocations because the two PAIRs
        // are merged into their preceding difference records.
        check(relocs.len() == 6);
        let offsets: Vec<u64> = relocs.iter().map(|(offset, _)| *offset).collect();
        check(offsets == vec![0, 4, 8, 12, 16, 20]);

        let pcrel = relocs.iter().find(|(offset, _)| *offset == 4).unwrap();
        check(pcrel.1.size() == 32);
        check(pcrel.1.kind() == RelocationKind::Relative);
        check(matches!(
            pcrel.1.flags(),
            RelocationFlags::MachO {
                r_pcrel: true,
                r_length: 2,
                ..
            }
        ));
        check(matches!(
            pcrel.1.target(),
            RelocationTarget::Address(0x2004)
        ));

        let sectdiff = relocs.iter().find(|(offset, _)| *offset == 8).unwrap();
        check(sectdiff.1.size() == 32);
        check(matches!(
            sectdiff.1.target(),
            RelocationTarget::Address(0x1010)
        ));
        check(matches!(
            sectdiff.1.subtractor_target(),
            Some(RelocationTarget::Address(0x2010))
        ));
        // Symbol subtractors remain a separate concept.
        check(sectdiff.1.subtractor().is_none());
        check(matches!(
            sectdiff.1.flags(),
            RelocationFlags::MachO {
                r_type,
                r_pcrel: false,
                r_length: 2
            } if r_type == object::macho::GENERIC_RELOC_SECTDIFF
        ));

        let local = relocs.iter().find(|(offset, _)| *offset == 12).unwrap();
        check(local.1.size() == 16);
        check(matches!(
            local.1.target(),
            RelocationTarget::Address(0x1020)
        ));
        check(matches!(
            local.1.subtractor_target(),
            Some(RelocationTarget::Address(0x2020))
        ));
        check(matches!(
            local.1.flags(),
            RelocationFlags::MachO { r_type, .. }
                if r_type == object::macho::GENERIC_RELOC_LOCAL_SECTDIFF
        ));

        let byte = relocs.iter().find(|(offset, _)| *offset == 16).unwrap();
        check(byte.1.size() == 8);
        check(matches!(byte.1.target(), RelocationTarget::Address(0x1008)));

        // The PAIR r_address values never become relocation field offsets.
        check(!offsets.contains(&0x00aabbcc));
        check(!offsets.contains(&0x00ddeeff));
        // The trailing plain record still appears.
        let trailing = relocs.iter().find(|(offset, _)| *offset == 20).unwrap();
        check(matches!(
            trailing.1.target(),
            RelocationTarget::Section(SectionIndex(1))
        ));
    });
}

#[test]
fn layout_values() {
    run("layout_values", || {
        let bytes = diff_object();
        let before = bytes.clone();
        let file = parse(&bytes);
        let section = file.section_by_name("__debug").unwrap();

        // Section 1 moves to 0x8000, section 2 to 0x5000 and the source
        // section 3 to 0x6000.
        let layout = [
            (SectionIndex(1), 0x8000u64),
            (SectionIndex(2), 0x5000),
            (SectionIndex(3), 0x6000),
        ];
        let map =
            object::RelocationMap::new_with_section_addresses(&file, &section, &layout).unwrap();

        // The worked example from the task: old target 0x1004 in a section that
        // moves from 0x1000 to 0x8000 turns field 0x1009 into 0x8009.
        check(map.relocate(0, 0x1009) == 0x8009);

        // PC-relative: target section 2 and source section 3 move by the same
        // delta here, so the field value is unchanged.
        check(map.relocate(4, 0x10) == 0x10);

        // Difference reloc: both ends follow their own section movement.
        // 0x8010 - 0x5010 = 0x3000.
        check(map.relocate(8, 0xffff_f000) == 0x3000);
        // 16-bit difference is truncated to its field width.
        check(map.relocate(12, 0xf000) == 0x3000);
        // 8-bit vanilla wraps inside its field.
        check(map.relocate(16, 0x09) == 0x09);
        // Plain section relocations keep their layout independent meaning.
        check(map.relocate(20, 7) == 0x1007);
        // Unknown offsets pass through untouched.
        check(map.relocate(99, 0x55) == 0x55);

        // A second layout where the source section moves more than the target
        // produces a negative residual that wraps in the 32-bit field.
        let alt_layout = [
            (SectionIndex(1), 0x8000u64),
            (SectionIndex(2), 0x5000),
            (SectionIndex(3), 0x7000),
        ];
        let alt =
            object::RelocationMap::new_with_section_addresses(&file, &section, &alt_layout).unwrap();
        // delta = 0x3000 - 0x4000 = -0x1000.
        check(alt.relocate(4, 0x10) == 0xffff_f010);
        // The two maps are independent and can be queried alternately.
        check(map.relocate(4, 0x10) == 0x10);
        check(alt.relocate(4, 0x10) == 0xffff_f010);
        check(map.relocate(8, 0xffff_f000) == 0x3000);
        check(alt.relocate(8, 0xffff_f000) == 0x3000);

        // The input bytes are never modified.
        check(bytes == before);

        // Missing layout for a section that owns a referenced address.
        let incomplete = [(SectionIndex(2), 0x5000u64), (SectionIndex(3), 0x6000)];
        let err = object::RelocationMap::new_with_section_addresses(&file, &section, &incomplete);
        check(err.is_err());

        // Duplicate section index in the caller layout.
        let duplicate = [
            (SectionIndex(1), 0x8000u64),
            (SectionIndex(1), 0x9000),
            (SectionIndex(2), 0x5000),
            (SectionIndex(3), 0x6000),
        ];
        let err = object::RelocationMap::new_with_section_addresses(&file, &section, &duplicate);
        check(err.is_err());

        // An address outside every non-empty section has no owner.
        let orphan = build_object(
            &[Spec {
                name: "__debug",
                addr: 0x1000,
                size: 0x100,
                relocs: vec![scattered(0, 2, false, 0, 0x9999)],
            }],
            &[],
        );
        let orphan_file = parse(&orphan);
        let orphan_section = orphan_file.section_by_name("__debug").unwrap();
        let err = object::RelocationMap::new_with_section_addresses(
            &orphan_file,
            &orphan_section,
            &[(SectionIndex(1), 0x8000)],
        );
        check(err.is_err());

        // An address inside two overlapping sections has ambiguous ownership.
        let ambiguous = build_object(
            &[
                Spec {
                    name: "__a",
                    addr: 0x1000,
                    size: 0x100,
                    relocs: Vec::new(),
                },
                Spec {
                    name: "__b",
                    addr: 0x1000,
                    size: 0x200,
                    relocs: Vec::new(),
                },
                Spec {
                    name: "__debug",
                    addr: 0x3000,
                    size: 0x100,
                    relocs: vec![scattered(0, 2, false, 0, 0x1050)],
                },
            ],
            &[],
        );
        let ambiguous_file = parse(&ambiguous);
        let ambiguous_section = ambiguous_file.section_by_name("__debug").unwrap();
        let err = object::RelocationMap::new_with_section_addresses(
            &ambiguous_file,
            &ambiguous_section,
            &[
                (SectionIndex(1), 0x8000),
                (SectionIndex(2), 0x9000),
                (SectionIndex(3), 0xa000),
            ],
        );
        check(err.is_err());
    });
}

#[test]
fn compatibility() {
    run("compatibility", || {
        let bytes = std::fs::read("task_support/basic.macho").unwrap();
        let snapshot = bytes.clone();
        let file = parse(&bytes);
        let section = file.section_by_name("__debug_info").unwrap();

        // The original constructor still works and leaves scattered offsets
        // unmapped, exactly as they were before scattered support existed.
        let map = object::RelocationMap::new(&file, &section).unwrap();
        check(map.relocate(4, 7) == 0x1007);
        check(map.relocate(0, 0x1009) == 0x1009);

        // The default map plus manual `add` behaves identically.
        let mut manual = object::RelocationMap::default();
        for (offset, reloc) in section.relocations() {
            manual.add(&file, offset, reloc).unwrap();
        }
        check(manual.relocate(4, 7) == 0x1007);
        check(manual.relocate(0, 0x1009) == 0x1009);

        // Non-scattered records keep their original fields and meaning.
        let (_, plain) = section
            .relocations()
            .find(|(offset, _)| *offset == 4)
            .unwrap();
        check(plain.size() == 32);
        check(matches!(
            plain.target(),
            RelocationTarget::Section(SectionIndex(1))
        ));
        check(plain.subtractor().is_none());
        check(plain.subtractor_target().is_none());

        // Two layouts over the same section stay independent.
        let first = object::RelocationMap::new_with_section_addresses(
            &file,
            &section,
            &[
                (SectionIndex(1), 0x8000u64),
                (SectionIndex(2), 0x2000),
                (SectionIndex(3), 0x3000),
            ],
        )
        .unwrap();
        let second = object::RelocationMap::new_with_section_addresses(
            &file,
            &section,
            &[
                (SectionIndex(1), 0x4000u64),
                (SectionIndex(2), 0x2000),
                (SectionIndex(3), 0x3000),
            ],
        )
        .unwrap();
        check(first.relocate(0, 0x1009) == 0x8009);
        check(second.relocate(0, 0x1009) == 0x4009);
        check(first.relocate(0, 0x1009) == 0x8009);
        // The plain relocation is layout independent in both maps.
        check(first.relocate(4, 7) == 0x1007);
        check(second.relocate(4, 7) == 0x1007);

        // The delivered file is untouched by all of the above.
        check(bytes == snapshot);
    });
}
