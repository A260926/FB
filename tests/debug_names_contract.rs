//! Contract tests for the read-only `.debug_names` support.
//!
//! Every byte in these tests is constructed by hand. The expected offsets,
//! tags and hashes are fixed constants written out below; none of them are
//! produced by the parser under test. The name bytes passed to `lookup` are
//! taken from a separately built `.debug_str` style blob to exercise the full
//! name comparison rather than hash-only matching.

use gimli::read::Error;
use gimli::{
    BigEndian, DW_FORM_data1, DW_FORM_data2, DW_FORM_data4, DW_FORM_data8, DW_FORM_flag_present,
    DW_FORM_ref4, DW_FORM_string, DW_FORM_udata, DW_TAG_base_type, DW_TAG_namespace,
    DW_TAG_structure_type, DW_TAG_subprogram, DW_TAG_variable, DebugNames, DebugNamesEntryOffset,
    DebugStrOffset, EndianSlice, Format, LittleEndian, Reader, UnitOffset,
};

/// Tiny big/little-endian byte builder. All expected values elsewhere in this
/// file are written as plain literals, not computed through the parser.
struct Builder {
    be: bool,
    buf: Vec<u8>,
}

impl Builder {
    fn new(be: bool) -> Self {
        Builder {
            be,
            buf: Vec::new(),
        }
    }

    fn u8(&mut self, v: u8) -> &mut Self {
        self.buf.push(v);
        self
    }

    fn u16(&mut self, v: u16) -> &mut Self {
        self.buf.extend(if self.be {
            v.to_be_bytes()
        } else {
            v.to_le_bytes()
        });
        self
    }

    fn u32(&mut self, v: u32) -> &mut Self {
        self.buf.extend(if self.be {
            v.to_be_bytes()
        } else {
            v.to_le_bytes()
        });
        self
    }

    fn u64(&mut self, v: u64) -> &mut Self {
        self.buf.extend(if self.be {
            v.to_be_bytes()
        } else {
            v.to_le_bytes()
        });
        self
    }

    fn uleb(&mut self, mut v: u64) -> &mut Self {
        loop {
            let mut byte = (v & 0x7f) as u8;
            v >>= 7;
            if v != 0 {
                byte |= 0x80;
            }
            self.buf.push(byte);
            if v == 0 {
                break;
            }
        }
        self
    }

    fn bytes(&mut self, v: &[u8]) -> &mut Self {
        self.buf.extend_from_slice(v);
        self
    }

    fn cstr(&mut self, v: &[u8]) -> &mut Self {
        self.buf.extend_from_slice(v);
        self.buf.push(0);
        self
    }

    fn into_vec(self) -> Vec<u8> {
        self.buf
    }
}

/// Wrap a contribution body in an initial length field.
fn wrap_length(be: bool, dwarf64: bool, body: Vec<u8>) -> Vec<u8> {
    let mut out = Builder::new(be);
    if dwarf64 {
        out.u32(0xffff_ffff).u64(body.len() as u64);
    } else {
        out.u32(body.len() as u32);
    }
    out.bytes(&body);
    out.into_vec()
}

/// Build one complete name index contribution.
///
/// Table layout follows DWARF 5 section 6.1.1.5:
/// fixed header, CU list, bucket table, name table (hash + entry offset),
/// abbreviation table, entry pool, then the inline name strings.
#[allow(clippy::too_many_arguments)]
fn contribution(
    be: bool,
    dwarf64: bool,
    cu_offsets: &[u64],
    buckets: &[u32],
    name_records: &[(u32, u32)],
    abbrevs: &[u8],
    entry_pool: &[u8],
    names: &[&[u8]],
    local_tu_count: u32,
    foreign_tu_count: u32,
) -> Vec<u8> {
    let mut body = Builder::new(be);
    body.u16(5)
        .u32(cu_offsets.len() as u32)
        .u32(local_tu_count)
        .u32(foreign_tu_count)
        .u32(buckets.len() as u32)
        .u32(name_records.len() as u32)
        .u32(abbrevs.len() as u32)
        .u32(entry_pool.len() as u32);
    for cu in cu_offsets {
        if dwarf64 {
            body.u64(*cu);
        } else {
            body.u32(*cu as u32);
        }
    }
    for bucket in buckets {
        body.u32(*bucket);
    }
    for (hash, entry_offset) in name_records {
        body.u32(*hash).u32(*entry_offset);
    }
    body.bytes(abbrevs).bytes(entry_pool);
    for name in names {
        body.cstr(name);
    }
    wrap_length(be, dwarf64, body.into_vec())
}

/// Abbreviation table with two abbreviation codes followed by the terminator.
fn two_abbrevs(be: bool, code1: &[u8], code2: &[u8]) -> Vec<u8> {
    let mut b = Builder::new(be);
    b.bytes(code1).bytes(code2).u8(0);
    b.into_vec()
}

/// Abbreviation 1: compile_unit(data1) + die_offset(ref4), terminator pair.
fn abbrev_cu_die(be: bool, tag: u16) -> Vec<u8> {
    let mut b = Builder::new(be);
    b.uleb(1)
        .uleb(u64::from(tag))
        .uleb(1)
        .uleb(u64::from(DW_FORM_data1.0))
        .uleb(3)
        .uleb(u64::from(DW_FORM_ref4.0))
        .uleb(0)
        .uleb(0);
    b.into_vec()
}

fn parse<R: Reader>(section: &DebugNames<R>) -> Result<gimli::NamesUnit<R>, Error> {
    section.units().next()?.ok_or(Error::NoEntryAtGivenOffset)
}

/// Exercise the common `Section` trait wiring for the new section type.
fn assert_section_wiring<E: gimli::Endianity>(
    section: &DebugNames<EndianSlice<'_, E>>,
    expected_len: usize,
) {
    use gimli::Section;
    assert_eq!(
        <DebugNames<EndianSlice<'_, E>>>::id(),
        gimli::SectionId::DebugNames
    );
    assert_eq!(
        <DebugNames<EndianSlice<'_, E>>>::section_name(),
        ".debug_names"
    );
    assert_eq!(section.reader().len(), expected_len);
}

#[test]
fn debug_names_contract() {
    case_le_be();
    case_dwarf64();
    case_hash_collision();
    case_multi_entry();
    case_units_parent();
    case_malformed();
}

/// 1. Little and big endian parse to identical, hard-coded values.
fn case_le_be() {
    // Abbrev 1: subprogram, compile_unit(data1) + die_offset(ref4).
    let abbrev1 = {
        let mut b = Builder::new(false);
        b.uleb(1)
            .uleb(u64::from(DW_TAG_subprogram.0))
            .uleb(1)
            .uleb(u64::from(DW_FORM_data1.0))
            .uleb(3)
            .uleb(u64::from(DW_FORM_ref4.0))
            .uleb(0)
            .uleb(0);
        b.into_vec()
    };
    // Abbrev 2: variable, die_offset(data2) + parent(flag_present).
    let abbrev2 = {
        let mut b = Builder::new(false);
        b.uleb(2)
            .uleb(u64::from(DW_TAG_variable.0))
            .uleb(3)
            .uleb(u64::from(DW_FORM_data2.0))
            .uleb(4)
            .uleb(u64::from(DW_FORM_flag_present.0))
            .uleb(0)
            .uleb(0);
        b.into_vec()
    };
    let abbrevs = two_abbrevs(false, &abbrev1, &abbrev2);

    // Entry pool: E0 at 0 (6 bytes), E1 at 6 (3 bytes). Built per endianity.
    let build_pool = |be: bool| {
        let mut pool = Builder::new(be);
        pool
            // E0: abbrev 1, cu index 0, die offset 0x40.
            .u8(1)
            .u8(0)
            .u32(0x40)
            // E1: abbrev 2, die offset 0x60 (data2); flag_present has no bytes.
            .u8(2)
            .u16(0x60);
        pool.into_vec()
    };
    let pool = build_pool(false);
    assert_eq!(pool.len(), 9);

    // hash alpha = 0x11111111 -> bucket 1; hash beta = 0x22222222 -> bucket 2.
    let records = [(0x1111_1111u32, 0u32), (0x2222_2222, 6)];
    let names: [&[u8]; 2] = [b"alpha", b"beta"];

    let le = contribution(
        false,
        false,
        &[0x1000],
        &[0, 1, 2, 0],
        &records,
        &abbrevs,
        &build_pool(false),
        &names,
        0,
        0,
    );
    let be = contribution(
        true,
        false,
        &[0x1000],
        &[0, 1, 2, 0],
        &records,
        &abbrevs,
        &build_pool(true),
        &names,
        0,
        0,
    );

    le_be_check(&le, LittleEndian);
    le_be_check(&be, BigEndian);
    println!("PASS le_be");
}

/// All le_be expectations are the same fixed values regardless of endianity.
fn le_be_check<E: gimli::Endianity>(data: &[u8], endian: E) {
    let section = DebugNames::new(data, endian);
    // The common SectionId carries the DWARF 5 section name, and the Section
    // trait exposes it through the same path as the other read sections.
    assert_eq!(gimli::SectionId::DebugNames.name(), ".debug_names");
    assert_section_wiring(&section, data.len());
    let unit = parse(&section).unwrap();
    assert_eq!(unit.format(), Format::Dwarf32);
    assert_eq!(unit.version(), 5);
    assert_eq!(unit.bucket_count(), 4);
    assert_eq!(unit.name_count(), 2);
    assert_eq!(unit.compilation_unit_offsets().len(), 1);
    assert_eq!(unit.compilation_unit_offsets()[0].0, 0x1000);
    assert_eq!(unit.section_offset().0, 0);

    let mut iter = unit.names();
    let alpha = iter.next().unwrap().unwrap();
    assert_eq!(alpha.name_index(), 0);
    assert_eq!(alpha.name().to_slice().unwrap().as_ref(), b"alpha");
    assert_eq!(alpha.tag(), DW_TAG_subprogram);
    assert_eq!(alpha.unit_header_offset().0, 0x1000);
    assert_eq!(alpha.die_offset(), UnitOffset(0x40));
    assert_eq!(alpha.entry_offset(), DebugNamesEntryOffset(0));

    let beta = iter.next().unwrap().unwrap();
    assert_eq!(beta.name_index(), 1);
    assert_eq!(beta.name().to_slice().unwrap().as_ref(), b"beta");
    assert_eq!(beta.tag(), DW_TAG_variable);
    assert_eq!(beta.unit_header_offset().0, 0x1000);
    assert_eq!(beta.die_offset(), UnitOffset(0x60));
    assert_eq!(beta.entry_offset(), DebugNamesEntryOffset(6));
    assert!(iter.next().unwrap().is_none());

    // flag_present means "parent not in index", not offset zero.
    assert!(unit.entry_parent(&alpha).unwrap().is_none());
    assert!(unit.entry_parent(&beta).unwrap().is_none());
    assert!(unit.parent_chain(&beta).unwrap().is_empty());

    // Hash lookup must still compare the full name bytes.
    let mut hits = unit.lookup(0x1111_1111, b"alpha").unwrap();
    let hit = hits.next().unwrap().unwrap();
    assert_eq!(hit.die_offset(), UnitOffset(0x40));
    assert!(hits.next().unwrap().is_none());
    assert!(unit
        .lookup(0x1111_1111, b"beta")
        .unwrap()
        .next()
        .unwrap()
        .is_none());
    assert!(unit
        .lookup(0xdead_beef, b"alpha")
        .unwrap()
        .next()
        .unwrap()
        .is_none());

    // A fresh iterator restarts at the only contribution, and a second
    // `next` on the same iterator reports the section as exhausted.
    let mut units = section.units();
    assert!(units.next().unwrap().is_some());
    assert!(units.next().unwrap().is_none());
}

/// 2. DWARF64 framing, 64-bit CU offset, udata/data8/ref8 forms, zero buckets.
fn case_dwarf64() {
    // Abbrev 1: structure_type, compile_unit(udata) + die_offset(data8).
    let mut abbrev = Builder::new(false);
    abbrev
        .uleb(1)
        .uleb(u64::from(DW_TAG_structure_type.0))
        .uleb(1)
        .uleb(u64::from(DW_FORM_udata.0))
        .uleb(3)
        .uleb(u64::from(DW_FORM_data8.0))
        .uleb(0)
        .uleb(0);
    // Abbrev 2: variable, compile_unit(data2) + die_offset(ref8).
    abbrev
        .uleb(2)
        .uleb(u64::from(DW_TAG_variable.0))
        .uleb(1)
        .uleb(u64::from(DW_FORM_data2.0))
        .uleb(3)
        .uleb(u64::from(gimli::DW_FORM_ref8.0))
        .uleb(0)
        .uleb(0)
        .u8(0);
    let abbrevs = abbrev.into_vec();

    // E0 at 0: abbrev 1, cu index 0 (uleb), die offset 0x70 as data8.
    // E1 at 10: abbrev 2, cu index 0 (data2), die offset 0x80 as ref8.
    let mut pool = Builder::new(false);
    pool.u8(1).u8(0).u64(0x70);
    pool.u8(2).u16(0).u64(0x80);
    let pool = pool.into_vec();
    assert_eq!(pool.len(), 21);

    let records = [(0x3333_3333u32, 0u32), (0x4444_4444, 10)];
    let names: [&[u8]; 2] = [b"gamma", b"delta"];
    let data = contribution(
        false,
        true,
        &[0x1_0000_0000],
        &[],
        &records,
        &abbrevs,
        &pool,
        &names,
        0,
        0,
    );

    let section = DebugNames::new(&data, LittleEndian);
    let unit = parse(&section).unwrap();
    assert_eq!(unit.format(), Format::Dwarf64);
    assert_eq!(unit.bucket_count(), 0);
    assert_eq!(unit.name_count(), 2);
    assert_eq!(unit.compilation_unit_offsets()[0].0, 0x1_0000_0000);

    // Enumeration works with no buckets.
    let mut all = unit.names();
    let gamma = all.next().unwrap().unwrap();
    assert_eq!(gamma.name().to_slice().unwrap().as_ref(), b"gamma");
    assert_eq!(gamma.tag(), DW_TAG_structure_type);
    assert_eq!(gamma.unit_header_offset().0, 0x1_0000_0000);
    assert_eq!(gamma.die_offset(), UnitOffset(0x70));
    assert_eq!(gamma.entry_offset(), DebugNamesEntryOffset(0));
    let delta = all.next().unwrap().unwrap();
    assert_eq!(delta.name().to_slice().unwrap().as_ref(), b"delta");
    assert_eq!(delta.tag(), DW_TAG_variable);
    assert_eq!(delta.die_offset(), UnitOffset(0x80));
    assert_eq!(delta.entry_offset(), DebugNamesEntryOffset(10));
    assert!(all.next().unwrap().is_none());

    // Lookup falls back to a linear scan.
    let hit = unit
        .lookup(0x3333_3333, b"gamma")
        .unwrap()
        .next()
        .unwrap()
        .unwrap();
    assert_eq!(hit.die_offset(), UnitOffset(0x70));
    // The ref8 entry is found through the linear scan as well.
    let delta_hit = unit
        .lookup(0x4444_4444, b"delta")
        .unwrap()
        .next()
        .unwrap()
        .unwrap();
    assert_eq!(delta_hit.die_offset(), UnitOffset(0x80));
    assert!(unit
        .lookup(0x3333_3333, b"delta")
        .unwrap()
        .next()
        .unwrap()
        .is_none());
    println!("PASS dwarf64");
}

/// 3. Two names sharing one hash must stay distinguishable by full name.
fn case_hash_collision() {
    let abbrevs = {
        let mut b = Builder::new(false);
        b.bytes(&abbrev_cu_die(false, DW_TAG_base_type.0)).u8(0);
        b.into_vec()
    };
    // Two six-byte entries, E0 at 0, E1 at 6.
    let mut pool = Builder::new(false);
    pool.u8(1).u8(0).u32(0x10);
    pool.u8(1).u8(0).u32(0x20);
    let pool = pool.into_vec();
    assert_eq!(pool.len(), 12);

    let shared_hash = 0x7777_7777u32;
    let records = [(shared_hash, 0u32), (shared_hash, 6)];
    let names: [&[u8]; 2] = [b"cat", b"dog"];
    // One shared bucket; both hashes land in bucket 0.
    let data = contribution(
        false,
        false,
        &[0x2000],
        &[1],
        &records,
        &abbrevs,
        &pool,
        &names,
        0,
        0,
    );

    let section = DebugNames::new(&data, LittleEndian);
    let unit = parse(&section).unwrap();

    // Full name from a separate .debug_str-style blob.
    let debug_str = b"cat\0dog\0bird\0";
    let cat = &debug_str[..3];
    let dog = &debug_str[4..7];
    assert_eq!(cat, b"cat");
    assert_eq!(dog, b"dog");

    let mut cat_hits = unit.lookup(shared_hash, cat).unwrap();
    let only_cat = cat_hits.next().unwrap().unwrap();
    assert_eq!(only_cat.name_index(), 0);
    assert_eq!(only_cat.die_offset(), UnitOffset(0x10));
    assert!(cat_hits.next().unwrap().is_none());

    let mut dog_hits = unit.lookup(shared_hash, dog).unwrap();
    let only_dog = dog_hits.next().unwrap().unwrap();
    assert_eq!(only_dog.name_index(), 1);
    assert_eq!(only_dog.die_offset(), UnitOffset(0x20));
    assert!(dog_hits.next().unwrap().is_none());

    assert!(unit
        .lookup(shared_hash, &debug_str[9..13])
        .unwrap()
        .next()
        .unwrap()
        .is_none());

    // Enumeration still yields both, in name table order.
    let all: Vec<_> = {
        let mut iter = unit.names();
        let mut out = Vec::new();
        while let Some(entry) = iter.next().unwrap() {
            out.push(entry.die_offset().0);
        }
        out
    };
    assert_eq!(all, vec![0x10, 0x20]);
    println!("PASS hash_collision");
}

/// 4. One name pointing at multiple entries returns all of them, in order.
fn case_multi_entry() {
    // Abbrev 1: subprogram, compile_unit(data1) + die_offset(data4). Using
    // data4 here keeps every supported fixed-width form exercised somewhere.
    let abbrevs = {
        let mut b = Builder::new(false);
        b.uleb(1)
            .uleb(u64::from(DW_TAG_subprogram.0))
            .uleb(1)
            .uleb(u64::from(DW_FORM_data1.0))
            .uleb(3)
            .uleb(u64::from(DW_FORM_data4.0))
            .uleb(0)
            .uleb(0)
            .u8(0);
        b.into_vec()
    };
    let mut pool = Builder::new(false);
    pool.u8(1).u8(0).u32(0x30);
    pool.u8(1).u8(0).u32(0x31);
    let pool = pool.into_vec();

    let hash = 0x5555_5555u32;
    let records = [(hash, 0u32), (hash, 6)];
    let names: [&[u8]; 2] = [b"dup", b"dup"];
    let data = contribution(
        false,
        false,
        &[0x2100],
        &[1],
        &records,
        &abbrevs,
        &pool,
        &names,
        0,
        0,
    );

    let section = DebugNames::new(&data, LittleEndian);
    let unit = parse(&section).unwrap();

    let mut hits = unit.lookup(hash, b"dup").unwrap();
    let first = hits.next().unwrap().unwrap();
    let second = hits.next().unwrap().unwrap();
    assert!(hits.next().unwrap().is_none());
    assert_eq!(first.name_index(), 0);
    assert_eq!(first.entry_offset(), DebugNamesEntryOffset(0));
    assert_eq!(first.die_offset(), UnitOffset(0x30));
    assert_eq!(second.name_index(), 1);
    assert_eq!(second.entry_offset(), DebugNamesEntryOffset(6));
    assert_eq!(second.die_offset(), UnitOffset(0x31));
    println!("PASS multi_entry");
}

/// 5. Multiple CUs and explicit parent chains, including flag_present.
fn case_units_parent() {
    // Abbrev 1: namespace, cu(data1) + die(ref4) + parent(ref4).
    let mut a1 = Builder::new(false);
    a1.uleb(1)
        .uleb(u64::from(DW_TAG_namespace.0))
        .uleb(1)
        .uleb(u64::from(DW_FORM_data1.0))
        .uleb(3)
        .uleb(u64::from(DW_FORM_ref4.0))
        .uleb(4)
        .uleb(u64::from(DW_FORM_ref4.0))
        .uleb(0)
        .uleb(0);
    // Abbrev 2: subprogram, cu(data1) + die(ref4) + parent(flag_present).
    let mut a2 = Builder::new(false);
    a2.uleb(2)
        .uleb(u64::from(DW_TAG_subprogram.0))
        .uleb(1)
        .uleb(u64::from(DW_FORM_data1.0))
        .uleb(3)
        .uleb(u64::from(DW_FORM_ref4.0))
        .uleb(4)
        .uleb(u64::from(DW_FORM_flag_present.0))
        .uleb(0)
        .uleb(0);
    let abbrevs = two_abbrevs(false, &a1.into_vec(), &a2.into_vec());

    // E0 at 0  (6 bytes): abbrev 2, cu 0, die 0x0a, parent flag.
    // E1 at 6  (10 bytes): abbrev 1, cu 0, die 0x0b, parent -> 0.
    // E2 at 16 (10 bytes): abbrev 1, cu 1, die 0x0c, parent -> 6.
    // E3 at 26 (10 bytes): abbrev 1, cu 1, die 0x0d, parent -> 16.
    let mut pool = Builder::new(false);
    pool.u8(2).u8(0).u32(0x0a);
    pool.u8(1).u8(0).u32(0x0b).u32(0);
    pool.u8(1).u8(1).u32(0x0c).u32(6);
    pool.u8(1).u8(1).u32(0x0d).u32(16);
    let pool = pool.into_vec();
    assert_eq!(pool.len(), 36);

    let records = [(0xa1u32, 0u32), (0xa2, 6), (0xa3, 16), (0xa4, 26)];
    let names: [&[u8]; 4] = [b"outer", b"inner", b"leaf", b"orphan"];
    // Zero buckets: enumeration and linear lookup must still work.
    let data = contribution(
        false,
        false,
        &[0x3000, 0x4000],
        &[],
        &records,
        &abbrevs,
        &pool,
        &names,
        0,
        0,
    );

    let section = DebugNames::new(&data, LittleEndian);
    let unit = parse(&section).unwrap();
    assert_eq!(unit.compilation_unit_offsets().len(), 2);
    assert_eq!(unit.compilation_unit_offsets()[0].0, 0x3000);
    assert_eq!(unit.compilation_unit_offsets()[1].0, 0x4000);

    let mut iter = unit.names();
    let outer = iter.next().unwrap().unwrap();
    let inner = iter.next().unwrap().unwrap();
    let leaf = iter.next().unwrap().unwrap();
    let orphan = iter.next().unwrap().unwrap();
    assert!(iter.next().unwrap().is_none());

    assert_eq!(outer.tag(), DW_TAG_subprogram);
    assert_eq!(outer.unit_header_offset().0, 0x3000);
    assert_eq!(inner.unit_header_offset().0, 0x3000);
    assert_eq!(leaf.unit_header_offset().0, 0x4000);
    assert_eq!(orphan.unit_header_offset().0, 0x4000);
    assert_eq!(outer.die_offset(), UnitOffset(0x0a));
    assert_eq!(inner.die_offset(), UnitOffset(0x0b));
    assert_eq!(leaf.die_offset(), UnitOffset(0x0c));
    assert_eq!(orphan.die_offset(), UnitOffset(0x0d));

    // Parent resolution, all bases relative to the entry pool.
    assert!(unit.entry_parent(&outer).unwrap().is_none());
    assert_eq!(
        unit.entry_parent(&inner).unwrap(),
        Some(DebugNamesEntryOffset(0))
    );
    assert_eq!(
        unit.entry_parent(&leaf).unwrap(),
        Some(DebugNamesEntryOffset(6))
    );

    assert!(unit.parent_chain(&outer).unwrap().is_empty());
    assert_eq!(
        unit.parent_chain(&inner).unwrap(),
        vec![DebugNamesEntryOffset(0)]
    );
    assert_eq!(
        unit.parent_chain(&leaf).unwrap(),
        vec![DebugNamesEntryOffset(6), DebugNamesEntryOffset(0)]
    );
    assert_eq!(
        unit.parent_chain(&orphan).unwrap(),
        vec![
            DebugNamesEntryOffset(16),
            DebugNamesEntryOffset(6),
            DebugNamesEntryOffset(0),
        ]
    );

    // Linear lookup still finds a deeply nested entry by name + hash.
    let hit = unit.lookup(0xa3, b"leaf").unwrap().next().unwrap().unwrap();
    assert_eq!(hit.entry_offset(), DebugNamesEntryOffset(16));
    println!("PASS units_parent");
}

/// 6. Malformed data never panics, never borrows the next contribution, and
/// iteration stops after the first error while earlier results stay valid.
fn case_malformed() {
    let abbrevs = {
        let mut b = Builder::new(false);
        b.bytes(&abbrev_cu_die(false, DW_TAG_subprogram.0)).u8(0);
        b.into_vec()
    };
    let good_pool = {
        let mut p = Builder::new(false);
        p.u8(1).u8(0).u32(0x99);
        p.into_vec()
    };
    let good = contribution(
        false,
        false,
        &[0x5000],
        &[1],
        &[(0x1234u32, 0u32)],
        &abbrevs,
        &good_pool,
        &[b"ok"],
        0,
        0,
    );

    // (a) Empty section gives an empty iteration, not an error.
    let empty = DebugNames::new(&[], LittleEndian);
    assert!(empty.units().next().unwrap().is_none());

    // (b) Length claims bytes the section does not have.
    let mut bad_length = Builder::new(false);
    bad_length.u32(0x100).u16(5);
    expect_err(&bad_length.into_vec());

    // (c) Name table count that cannot fit: multiplication overflow.
    let mut overflow = Builder::new(false);
    overflow
        .u32(0x40)
        .u16(5)
        .u32(0)
        .u32(0)
        .u32(0)
        .u32(0)
        .u32(u32::MAX)
        .u32(0)
        .u32(0);
    expect_err(&overflow.into_vec());

    // (d) Bucket index outside the name table.
    let bad_bucket = contribution(
        false,
        false,
        &[0x5000],
        &[42],
        &[(1u32, 0u32)],
        &abbrevs,
        &good_pool,
        &[b"ok"],
        0,
        0,
    );
    expect_err(&bad_bucket);

    // (e) Unterminated name: the string area is cut off before the NUL.
    let mut unterminated = contribution(
        false,
        false,
        &[0x5000],
        &[],
        &[(1u32, 0u32)],
        &abbrevs,
        &good_pool,
        &[],
        0,
        0,
    );
    unterminated.extend_from_slice(b"ok");
    expect_err(&unterminated);

    // (f) Entry references an abbreviation that was never declared.
    let unknown_abbrev_pool = {
        let mut p = Builder::new(false);
        p.u8(9).u8(0).u32(0x11);
        p.into_vec()
    };
    let unknown_abbrev = contribution(
        false,
        false,
        &[0x5000],
        &[],
        &[(1u32, 0u32)],
        &abbrevs,
        &unknown_abbrev_pool,
        &[b"ok"],
        0,
        0,
    );
    let err = first_error(&unknown_abbrev);
    assert_eq!(err, Error::UnknownAbbreviation(9));

    // (g) Truncated attribute: ref4 needs four bytes, pool only has one.
    let truncated_pool = vec![1u8, 0u8, 0x11u8];
    let truncated = contribution(
        false,
        false,
        &[0x5000],
        &[],
        &[(1u32, 0u32)],
        &abbrevs,
        &truncated_pool,
        &[b"ok"],
        0,
        0,
    );
    expect_err(&truncated);

    // (h) Local type units are explicitly unsupported.
    let local_tu = contribution(
        false,
        false,
        &[0x5000],
        &[],
        &[(1u32, 0u32)],
        &abbrevs,
        &good_pool,
        &[b"ok"],
        1,
        0,
    );
    assert_eq!(first_error(&local_tu), Error::UnsupportedNameIndexTypeUnit);

    // (i) Foreign type units are explicitly unsupported.
    let foreign_tu = contribution(
        false,
        false,
        &[0x5000],
        &[],
        &[(1u32, 0u32)],
        &abbrevs,
        &good_pool,
        &[b"ok"],
        0,
        1,
    );
    assert_eq!(
        first_error(&foreign_tu),
        Error::UnsupportedNameIndexTypeUnit
    );

    // (j) Multiple CUs but no DW_IDX_compile_unit anywhere.
    let no_cu_attr_abbrev = {
        let mut b = Builder::new(false);
        b.uleb(1)
            .uleb(u64::from(DW_TAG_subprogram.0))
            .uleb(3)
            .uleb(u64::from(DW_FORM_ref4.0))
            .uleb(0)
            .uleb(0)
            .u8(0);
        b.into_vec()
    };
    let no_cu_pool = {
        let mut p = Builder::new(false);
        p.u8(1).u32(0x12);
        p.into_vec()
    };
    let multi_cu_no_index = contribution(
        false,
        false,
        &[0x5000, 0x6000],
        &[],
        &[(1u32, 0u32)],
        &no_cu_attr_abbrev,
        &no_cu_pool,
        &[b"ok"],
        0,
        0,
    );
    expect_err(&multi_cu_no_index);

    // (k) Parent reference lands in the middle of another entry's operands.
    let operand_parent = parent_fixture(0x02);
    let section = DebugNames::new(&operand_parent, LittleEndian);
    let unit = parse(&section).unwrap();
    let entry = unit.names().next().unwrap().unwrap();
    assert_eq!(unit.entry_parent(&entry), Err(Error::InvalidNameIndex));
    assert_eq!(unit.parent_chain(&entry), Err(Error::InvalidNameIndex));

    // (l) Parent chain forms a cycle.
    let cyclic = cycle_fixture();
    let section = DebugNames::new(&cyclic, LittleEndian);
    let unit = parse(&section).unwrap();
    let entry = unit.names().next().unwrap().unwrap();
    assert_eq!(unit.parent_chain(&entry), Err(Error::InvalidNameIndex));

    // (m) Good contribution followed by a truncated one. The good unit parses,
    // the second one errors once, then iteration is over: it never reaches
    // past the section, and earlier results stay readable.
    let mut combined = good.clone();
    let mut tail = Builder::new(false);
    tail.u32(0x20).u16(5);
    combined.extend_from_slice(&tail.into_vec());
    let section = DebugNames::new(&combined, LittleEndian);
    let mut units = section.units();
    let first = units.next().unwrap().unwrap();
    assert_eq!(first.name_count(), 1);
    let still_readable = first.names().next().unwrap().unwrap();
    assert_eq!(still_readable.name().to_slice().unwrap().as_ref(), b"ok");
    assert!(units.next().is_err());
    assert!(units.next().unwrap().is_none());

    // (n) Two well-formed contributions keep independent abbreviation tables
    // and entry pools: the same entry offset 0 decodes with each unit's own
    // abbreviation.
    let mut two = good.clone();
    let other_abbrev = {
        let mut b = Builder::new(false);
        b.uleb(1)
            .uleb(u64::from(DW_TAG_variable.0))
            .uleb(1)
            .uleb(u64::from(DW_FORM_data1.0))
            .uleb(3)
            .uleb(u64::from(DW_FORM_ref4.0))
            .uleb(0)
            .uleb(0)
            .u8(0);
        b.into_vec()
    };
    let second = contribution(
        false,
        false,
        &[0x7000],
        &[1],
        &[(0x9999u32, 0u32)],
        &other_abbrev,
        &good_pool,
        &[b"ok"],
        0,
        0,
    );
    two.extend_from_slice(&second);
    let section = DebugNames::new(&two, LittleEndian);
    let mut units = section.units();
    let first = units.next().unwrap().unwrap();
    let second_unit = units.next().unwrap().unwrap();
    assert!(units.next().unwrap().is_none());
    assert_eq!(
        first.names().next().unwrap().unwrap().tag(),
        DW_TAG_subprogram
    );
    assert_eq!(
        second_unit.names().next().unwrap().unwrap().tag(),
        DW_TAG_variable
    );
    assert_eq!(second_unit.section_offset().0, good.len());

    // (o) An attribute with an unsupported form fails explicitly rather than
    // being skipped. DW_FORM_string is not part of the supported integer set.
    let string_form_abbrev = {
        let mut b = Builder::new(false);
        b.uleb(1)
            .uleb(u64::from(DW_TAG_subprogram.0))
            .uleb(1)
            .uleb(u64::from(DW_FORM_data1.0))
            .uleb(3)
            .uleb(u64::from(DW_FORM_string.0))
            .uleb(0)
            .uleb(0)
            .u8(0);
        b.into_vec()
    };
    let string_form_pool = {
        let mut p = Builder::new(false);
        p.u8(1).u8(0).cstr(b"not-an-offset");
        p.into_vec()
    };
    let unsupported_form = contribution(
        false,
        false,
        &[0x5000],
        &[],
        &[(1u32, 0u32)],
        &string_form_abbrev,
        &string_form_pool,
        &[b"ok"],
        0,
        0,
    );
    assert_eq!(
        first_error(&unsupported_form),
        Error::UnknownForm(DW_FORM_string)
    );

    // (p) The declared abbreviation table size disagrees with the abbreviations
    // actually present: the extra declared byte leaves the table unbalanced.
    let mut oversized_abbrev = abbrev_cu_die(false, DW_TAG_subprogram.0);
    oversized_abbrev.push(0);
    oversized_abbrev.push(0);
    let oversized = contribution(
        false,
        false,
        &[0x5000],
        &[],
        &[(1u32, 0u32)],
        &oversized_abbrev,
        &good_pool,
        &[b"ok"],
        0,
        0,
    );
    expect_err(&oversized);

    println!("PASS malformed");
}

/// Build a unit whose sole entry's `DW_IDX_parent` points at a chosen pool
/// offset. Entry E0 occupies bytes 0..=5, so pointing at 2 lands inside its
/// operands.
fn parent_fixture(parent_value: u32) -> Vec<u8> {
    // Abbrev 1: subprogram, cu(data1) + die(ref4) + parent(ref4).
    let mut abbrev = Builder::new(false);
    abbrev
        .uleb(1)
        .uleb(u64::from(DW_TAG_subprogram.0))
        .uleb(1)
        .uleb(u64::from(DW_FORM_data1.0))
        .uleb(3)
        .uleb(u64::from(DW_FORM_ref4.0))
        .uleb(4)
        .uleb(u64::from(DW_FORM_ref4.0))
        .uleb(0)
        .uleb(0)
        .u8(0);
    let mut pool = Builder::new(false);
    pool.u8(1).u8(0).u32(0x21).u32(parent_value);
    contribution(
        false,
        false,
        &[0x8000],
        &[],
        &[(7u32, 0u32)],
        &abbrev.into_vec(),
        &pool.into_vec(),
        &[b"node"],
        0,
        0,
    )
}

/// Two entries that reference each other as parents.
fn cycle_fixture() -> Vec<u8> {
    let mut abbrev = Builder::new(false);
    abbrev
        .uleb(1)
        .uleb(u64::from(DW_TAG_subprogram.0))
        .uleb(1)
        .uleb(u64::from(DW_FORM_data1.0))
        .uleb(3)
        .uleb(u64::from(DW_FORM_ref4.0))
        .uleb(4)
        .uleb(u64::from(DW_FORM_ref4.0))
        .uleb(0)
        .uleb(0)
        .u8(0);
    // E0 at 0 (10 bytes): cu 0, die 0x31, parent -> 10.
    // E1 at 10 (10 bytes): cu 0, die 0x32, parent -> 0.
    let mut pool = Builder::new(false);
    pool.u8(1).u8(0).u32(0x31).u32(10);
    pool.u8(1).u8(0).u32(0x32).u32(0);
    contribution(
        false,
        false,
        &[0x8100],
        &[],
        &[(8u32, 0u32), (9u32, 10)],
        abbrev.into_vec().as_slice(),
        pool.into_vec().as_slice(),
        &[b"a", b"b"],
        0,
        0,
    )
}

fn first_error(data: &[u8]) -> Error {
    let section = DebugNames::new(data, LittleEndian);
    section.units().next().unwrap_err()
}

fn expect_err(data: &[u8]) {
    let section = DebugNames::new(data, LittleEndian);
    let mut units = section.units();
    let result = units.next();
    assert!(
        result.is_err(),
        "{}",
        format!("expected error, got {result:?}")
    );
    // The iterator is exhausted after its single error.
    assert!(units.next().unwrap().is_none());
}

// Keep the DebugStrOffset import meaningful for documentation-style usage:
// callers resolve .debug_str themselves and pass the bytes in.
#[allow(dead_code)]
fn debug_str_usage(debug_str: &gimli::DebugStr<EndianSlice<'_, LittleEndian>>) -> Vec<u8> {
    debug_str
        .get_str(DebugStrOffset(0))
        .unwrap()
        .to_slice()
        .unwrap()
        .into_owned()
}
