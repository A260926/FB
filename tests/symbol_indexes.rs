// Integration tests for the global and public symbol indexes.
//
// Every MSF/PDB below is synthesized in memory from known symbol records; nothing is loaded from
// disk, executed, or fetched from a symbol server. Bucket numbers are computed from the library's
// hash, so the expected lookup results are exactly the records that were placed in those buckets.

#[path = "../task_support/check.rs"]
mod check;
#[path = "../task_support/fixture.rs"]
mod fixture;

use pdb::{FallibleIterator, PdbInternalSectionOffset, SymbolIndex, SymbolIndexKind, PDB};

const S_GDATA32: u16 = 0x110d;
const S_PUB32: u16 = 0x110e;

/// A symbol record to be synthesized into the symbol records stream.
#[derive(Clone, Debug)]
struct RecordSpec {
    name: &'static [u8],
    public: bool,
    section: u16,
    offset: u32,
    /// Flags stored in the record. Public symbols interpret them as public flags.
    flags: u32,
}

fn spec(name: &'static [u8], public: bool, section: u16, offset: u32) -> RecordSpec {
    RecordSpec {
        name,
        public,
        section,
        offset,
        flags: if public { 3 } else { 0x74 },
    }
}

/// Serializes a list of symbol records and returns the stream bytes and each record's offset.
fn symbol_stream(records: &[RecordSpec]) -> (Vec<u8>, Vec<u32>) {
    let mut out = Vec::new();
    let mut offsets = Vec::new();
    for record in records {
        offsets.push(out.len() as u32);
        let mut rec = Vec::new();
        rec.extend_from_slice(&(if record.public { S_PUB32 } else { S_GDATA32 }).to_le_bytes());
        rec.extend_from_slice(&record.flags.to_le_bytes());
        rec.extend_from_slice(&record.offset.to_le_bytes());
        rec.extend_from_slice(&record.section.to_le_bytes());
        rec.extend_from_slice(record.name);
        rec.push(0);
        while (rec.len() + 2) % 4 != 0 {
            rec.push(0);
        }
        out.extend_from_slice(&(rec.len() as u16).to_le_bytes());
        out.extend_from_slice(&rec);
    }
    (out, offsets)
}

/// Builds a GSI/PSI name table from `(bucket, symbol offset)` pairs.
///
/// This is the same format used by `task_support/fixture.rs`, including the historical 12-byte
/// scale for chain starts.
fn name_table(entries: &[(u32, u32)]) -> Vec<u8> {
    fixture::gsi(entries.to_vec())
}

/// Builds a PSI stream with the given nested name table and address-map offsets.
fn psi_stream(name_table: Vec<u8>, addresses: &[u32]) -> Vec<u8> {
    let mut psi = vec![0u8; 28];
    psi[0..4].copy_from_slice(&(name_table.len() as u32).to_le_bytes());
    psi[4..8].copy_from_slice(&((addresses.len() * 4) as u32).to_le_bytes());
    psi.extend(name_table);
    for offset in addresses {
        psi.extend_from_slice(&offset.to_le_bytes());
    }
    psi
}

/// Builds a DBI stream declaring the given GSI, PSI and symbol record stream numbers.
fn dbi_stream(gs: u16, ps: u16, records: u16) -> Vec<u8> {
    dbi_stream_with_modules(gs, ps, records, &[])
}

/// Builds a DBI stream with an optional module list. Each module is `(name, info_stream, symbols)`.
fn dbi_stream_with_modules(
    gs: u16,
    ps: u16,
    records: u16,
    modules: &[(&str, u16, u32)],
) -> Vec<u8> {
    let mut module_list = Vec::new();
    for (name, stream, symbols_size) in modules {
        module_list.extend_from_slice(&u32::to_le_bytes(0)); // opened
        module_list.extend_from_slice(&u16::to_le_bytes(1)); // section
        module_list.extend_from_slice(&u16::to_le_bytes(0)); // padding
        module_list.extend_from_slice(&u32::to_le_bytes(0)); // offset
        module_list.extend_from_slice(&u32::to_le_bytes(0)); // size
        module_list.extend_from_slice(&u32::to_le_bytes(0)); // characteristics
        module_list.extend_from_slice(&u16::to_le_bytes(0)); // module
        module_list.extend_from_slice(&u16::to_le_bytes(0)); // padding
        module_list.extend_from_slice(&u32::to_le_bytes(0)); // data crc
        module_list.extend_from_slice(&u32::to_le_bytes(0)); // reloc crc
        module_list.extend_from_slice(&u16::to_le_bytes(0)); // flags
        module_list.extend_from_slice(&u16::to_le_bytes(*stream)); // info stream
        module_list.extend_from_slice(&u32::to_le_bytes(*symbols_size));
        module_list.extend_from_slice(&u32::to_le_bytes(0)); // lines_size
        module_list.extend_from_slice(&u32::to_le_bytes(0)); // c13_lines_size
        module_list.extend_from_slice(&u16::to_le_bytes(0)); // files
        module_list.extend_from_slice(&u16::to_le_bytes(0)); // padding
        module_list.extend_from_slice(&u32::to_le_bytes(0)); // filename offsets
        module_list.extend_from_slice(&u32::to_le_bytes(0)); // source
        module_list.extend_from_slice(&u32::to_le_bytes(0)); // compiler
        module_list.extend_from_slice(name.as_bytes());
        module_list.push(0);
        module_list.extend_from_slice(name.as_bytes());
        module_list.push(0);
        while module_list.len() % 4 != 0 {
            module_list.push(0);
        }
    }

    let mut dbi = vec![0u8; 64];
    dbi[0..4].copy_from_slice(&0xffff_ffffu32.to_le_bytes());
    dbi[4..8].copy_from_slice(&19990903u32.to_le_bytes());
    dbi[8..12].copy_from_slice(&1u32.to_le_bytes());
    dbi[12..14].copy_from_slice(&gs.to_le_bytes());
    dbi[16..18].copy_from_slice(&ps.to_le_bytes());
    dbi[20..22].copy_from_slice(&records.to_le_bytes());
    dbi[24..28].copy_from_slice(&(module_list.len() as u32).to_le_bytes());
    dbi[58..60].copy_from_slice(&0x8664u16.to_le_bytes());
    dbi.extend(module_list);
    dbi
}

fn info_stream() -> Vec<u8> {
    let mut info = vec![0u8; 48];
    info[0..4].copy_from_slice(&20000404u32.to_le_bytes());
    info[8..12].copy_from_slice(&1u32.to_le_bytes());
    for (i, byte) in info[12..28].iter_mut().enumerate() {
        *byte = (i + 1) as u8;
    }
    info[36..40].copy_from_slice(&1u32.to_le_bytes());
    info
}

fn tpi_stream() -> Vec<u8> {
    let mut tpi = vec![0u8; 56];
    tpi[0..4].copy_from_slice(&20040203u32.to_le_bytes());
    tpi[4..8].copy_from_slice(&56u32.to_le_bytes());
    tpi[8..12].copy_from_slice(&0x1000u32.to_le_bytes());
    tpi[12..16].copy_from_slice(&0x1000u32.to_le_bytes());
    tpi[20..22].copy_from_slice(&0xffffu16.to_le_bytes());
    tpi[22..24].copy_from_slice(&0xffffu16.to_le_bytes());
    tpi[24..28].copy_from_slice(&4u32.to_le_bytes());
    tpi[28..32].copy_from_slice(&4096u32.to_le_bytes());
    tpi
}

/// A complete PDB image over the given symbol records and optional indexes.
fn image(records: &[RecordSpec], gsi: Option<Vec<u8>>, psi: Option<Vec<u8>>) -> Vec<u8> {
    image_with(records, gsi, psi, dbi_stream(6, 7, 5), &[], &[], &[])
}

/// A complete PDB image with full control over the DBI header, extra streams and module info.
fn image_with(
    records: &[RecordSpec],
    gsi: Option<Vec<u8>>,
    psi: Option<Vec<u8>>,
    dbi: Vec<u8>,
    extra_streams: &[(u16, Option<Vec<u8>>)],
    base_streams: &[(u16, Option<Vec<u8>>)],
    module_info: &[Option<Vec<u8>>],
) -> Vec<u8> {
    let (symbols, _) = symbol_stream(records);

    let mut streams: Vec<Option<Vec<u8>>> = vec![
        None,
        Some(info_stream()),
        Some(tpi_stream()),
        Some(dbi),
        None,
        Some(symbols),
        gsi,
        psi,
        None,
    ];
    for (index, stream) in base_streams {
        while streams.len() <= *index as usize {
            streams.push(None);
        }
        streams[*index as usize] = stream.clone();
    }
    for (index, stream) in extra_streams {
        while streams.len() <= *index as usize {
            streams.push(None);
        }
        streams[*index as usize] = stream.clone();
    }
    for (i, stream) in module_info.iter().enumerate() {
        let index = 8 + i;
        while streams.len() <= index {
            streams.push(None);
        }
        streams[index] = stream.clone();
    }
    fixture::msf(&streams)
}

fn open(image: Vec<u8>) -> PDB<'static, std::io::Cursor<Vec<u8>>> {
    PDB::open(std::io::Cursor::new(image)).expect("image parses")
}

/// Builds a global symbol index that places each of the given records in its hash bucket.
fn index_for(records: &[RecordSpec], offsets: &[u32], public_only: bool) -> Vec<u8> {
    let mut entries = Vec::new();
    for (spec, &offset) in records.iter().zip(offsets) {
        if public_only && !spec.public {
            continue;
        }
        entries.push((pdb::gsi_bucket(spec.name), offset));
    }
    name_table(&entries)
}

#[test]
fn buckets() {
    check::run("buckets", |c| {
        // Find two distinct names that hash to the same bucket and one that does not.
        let mut collision = None;
        'outer: for i in 0..2048u32 {
            let a = format!("Sym{}", i);
            let bucket = pdb::gsi_bucket(a.as_bytes());
            for j in (i + 1)..2048u32 {
                let b = format!("Sym{}", j);
                if pdb::gsi_bucket(b.as_bytes()) == bucket {
                    collision = Some((a, b));
                    break 'outer;
                }
            }
        }
        let (name_a, name_b) = collision.expect("a synthetic bucket collision exists");
        let name_c: &'static [u8] = b"Unrelated";
        assert_ne!(pdb::gsi_bucket(name_c), pdb::gsi_bucket(name_a.as_bytes()));

        let leaked_a: &'static [u8] = Box::leak(name_a.clone().into_boxed_str()).as_bytes();
        let leaked_b: &'static [u8] = Box::leak(name_b.clone().into_boxed_str()).as_bytes();
        let records = vec![
            spec(leaked_a, false, 1, 0x10),
            spec(leaked_b, false, 1, 0x20),
            spec(name_c, false, 1, 0x30),
        ];
        let (_, offsets) = symbol_stream(&records);

        // The two colliding records live in one bucket; the unrelated name lives in another.
        let bucket_a = pdb::gsi_bucket(name_a.as_bytes());
        let bucket_c = pdb::gsi_bucket(name_c);
        let gsi = name_table(&[
            (bucket_a, offsets[0]),
            (bucket_a, offsets[1]),
            (bucket_c, offsets[2]),
        ]);
        let mut pdb = open(image(&records, Some(gsi), None));
        let symbols = pdb.global_symbols().expect("symbol table");

        let found_a = symbols
            .find_by_name(name_a.as_bytes(), SymbolIndexKind::Global)
            .expect("lookup succeeds");
        c.check(
            found_a.len() == 1,
            "colliding bucket holds one record for name A",
        );
        c.check(
            found_a[0].index() == SymbolIndex(offsets[0]),
            "collision lookup returns record A by its index",
        );
        c.check(
            found_a[0]
                .parse()
                .expect("parse")
                .name()
                .unwrap()
                .as_bytes()
                == name_a.as_bytes(),
            "collision lookup returns the exact name A",
        );

        let found_b = symbols
            .find_by_name(name_b.as_bytes(), SymbolIndexKind::Global)
            .expect("lookup succeeds");
        c.check(
            found_b.len() == 1,
            "the same bucket yields one record for name B",
        );
        c.check(
            found_b[0].index() == SymbolIndex(offsets[1]),
            "collision bucket lookup returns record B, not A",
        );

        // A missing name whose bucket is not occupied yields an empty result, not an error.
        let found_c = symbols
            .find_by_name(b"NeverPresent", SymbolIndexKind::Global)
            .expect("empty buckets are a valid miss");
        c.check(found_c.is_empty(), "an unoccupied bucket yields no symbols");

        // The exact name in a different bucket resolves without seeing the collision bucket.
        let found_unrelated = symbols
            .find_by_name(name_c, SymbolIndexKind::Global)
            .expect("lookup succeeds");
        c.check(
            found_unrelated.len() == 1,
            "the unrelated bucket yields exactly one record",
        );
        c.check(
            found_unrelated[0].index() == SymbolIndex(offsets[2]),
            "the unrelated bucket does not leak colliding records",
        );

        // A record whose name differs only in case is a different record. Hash V1 is
        // case-sensitive, so the spellings hash to different buckets; a query for one spelling
        // must return only its own record.
        let mut upper: Vec<u8> = name_a.as_bytes().to_vec();
        for byte in &mut upper {
            *byte = byte.to_ascii_uppercase();
        }
        let upper_name: &'static [u8] = Box::leak(upper.clone().into_boxed_slice());
        let case_records = vec![
            spec(leaked_a, false, 1, 0x10),
            spec(leaked_b, false, 1, 0x20),
            spec(name_c, false, 1, 0x30),
            spec(upper_name, false, 1, 0x40),
        ];
        let (_, case_offsets) = symbol_stream(&case_records);
        let case_gsi = name_table(&[
            (bucket_a, case_offsets[0]),
            (bucket_a, case_offsets[1]),
            (bucket_c, case_offsets[2]),
            (pdb::gsi_bucket(&upper), case_offsets[3]),
        ]);
        let mut case_pdb = open(image(&case_records, Some(case_gsi), None));
        let case_symbols = case_pdb.global_symbols().expect("symbol table");
        let found_upper = case_symbols
            .find_by_name(&upper, SymbolIndexKind::Global)
            .expect("lookup succeeds");
        c.check(
            found_upper.len() == 1 && found_upper[0].index() == SymbolIndex(case_offsets[3]),
            "a case-variant query returns only its exactly matching record",
        );
        c.check(
            pdb::gsi_bucket(&upper) != pdb::gsi_bucket(name_a.as_bytes()),
            "Hash V1 treats case variants as distinct names",
        );

        // Querying the public index must not fall back to the global index.
        c.check(
            matches!(
                symbols.find_by_name(name_a.as_bytes(), SymbolIndexKind::Public),
                Err(pdb::Error::SymbolIndexUnavailable("public"))
            ),
            "public lookup without a public index reports unavailability",
        );
    });
}

#[test]
fn names() {
    check::run("names", |c| {
        let records = vec![
            spec(b"Same", false, 1, 0x10),
            spec(b"Same", false, 2, 0x10),
            spec(b"Other", false, 1, 0x20),
            spec(b"PublicSame", true, 1, 0x30),
            spec(b"PublicSame", true, 2, 0x30),
            spec(b"PGlobalOnly", true, 1, 0x40),
        ];
        let (_, offsets) = symbol_stream(&records);

        let bucket_same = pdb::gsi_bucket(b"Same");
        let bucket_other = pdb::gsi_bucket(b"Other");
        let bucket_pub = pdb::gsi_bucket(b"PublicSame");
        let bucket_pglobal = pdb::gsi_bucket(b"PGlobalOnly");

        // Both "Same" records live in one bucket, with a duplicate reference to the first record.
        let mut global_entries = vec![
            (bucket_same, offsets[0]),
            (bucket_same, offsets[1]),
            (bucket_same, offsets[0]), // duplicate reference must not duplicate the symbol
            (bucket_other, offsets[2]),
            (bucket_pglobal, offsets[5]),
        ];
        // A case variant in the same bucket must not be reported as an exact match.
        global_entries.push((bucket_same, offsets[2]));
        let gsi = name_table(&global_entries);

        let public_entries = vec![(bucket_pub, offsets[3]), (bucket_pub, offsets[4])];
        let psi = psi_stream(name_table(&public_entries), &[offsets[3], offsets[4]]);

        let image = image(&records, Some(gsi), Some(psi));
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");

        let same = symbols
            .find_by_name(b"Same", SymbolIndexKind::Global)
            .expect("global lookup");
        c.check(
            same.len() == 2,
            "both distinct same-named records are returned",
        );
        c.check(
            same[0].index() == SymbolIndex(offsets[0])
                && same[1].index() == SymbolIndex(offsets[1]),
            "same-named records are sorted by SymbolIndex",
        );
        c.check(
            same[0].parse().expect("parse").name().unwrap().as_bytes() == b"Same"
                && same[1].parse().expect("parse").name().unwrap().as_bytes() == b"Same",
            "each returned symbol parses back to the requested name",
        );
        c.check(
            same.iter()
                .filter(|s| s.index() == SymbolIndex(offsets[0]))
                .count()
                == 1,
            "a repeated reference is not expanded into duplicate symbols",
        );
        c.check(
            same[0].parse().expect("parse") != same[1].parse().expect("parse"),
            "the two same-named records are distinct records",
        );

        let other = symbols
            .find_by_name(b"Other", SymbolIndexKind::Global)
            .expect("global lookup");
        c.check(
            other.len() == 1 && other[0].index() == SymbolIndex(offsets[2]),
            "other name resolves",
        );
        c.check(
            other[0].parse().expect("parse").name().unwrap().as_bytes() == b"Other",
            "other name is exact",
        );

        let pub_same = symbols
            .find_by_name(b"PublicSame", SymbolIndexKind::Public)
            .expect("public lookup");
        c.check(
            pub_same.len() == 2,
            "public name table returns both aliases",
        );
        c.check(
            pub_same[0].index() == SymbolIndex(offsets[3])
                && pub_same[1].index() == SymbolIndex(offsets[4]),
            "public name results are sorted by SymbolIndex",
        );
        c.check(
            pub_same[0]
                .parse()
                .expect("parse")
                .name()
                .unwrap()
                .as_bytes()
                == b"PublicSame",
            "public name table resolves to public records",
        );

        // The public index must not answer for a global-only record, even though it is present in
        // the same symbol record stream.
        let global_only = symbols
            .find_by_name(b"PGlobalOnly", SymbolIndexKind::Public)
            .expect("public lookup");
        c.check(
            global_only.is_empty(),
            "public lookup only consults its own bucket/name table",
        );

        // The global index must not answer for records that are only in the public table.
        let global_same_bucket = symbols
            .find_by_name(b"PublicSame", SymbolIndexKind::Global)
            .expect("global lookup");
        c.check(
            global_same_bucket.is_empty(),
            "global lookup does not fall back to the public table",
        );

        // Returned symbols remain usable through the ordinary parse interface.
        let public_symbol = pub_same[0].parse().expect("parse");
        match public_symbol {
            pdb::SymbolData::Public(data) => {
                c.check(data.offset.section == 1, "public symbol parses its section");
                c.check(
                    data.offset.offset == 0x30,
                    "public symbol parses its offset",
                );
                c.check(
                    data.name.as_bytes() == b"PublicSame",
                    "public symbol parses its name",
                );
            }
            _ => c.check(false, "public symbol data kind"),
        }
    });
}

#[test]
fn public_addresses() {
    check::run("public_addresses", |c| {
        let records = vec![
            spec(b"Alias", true, 1, 0x20),
            spec(b"BetaAlias", true, 1, 0x20),
            spec(b"Gamma", true, 2, 0x20),
            spec(b"Delta", true, 1, 0x10),
            spec(b"NotPublic", false, 1, 0x20),
        ];
        let (_, offsets) = symbol_stream(&records);

        let bucket_alias = pdb::gsi_bucket(b"Alias");
        let names = name_table(&[(bucket_alias, offsets[0]), (bucket_alias, offsets[1])]);
        // Address map order: (1,0x10) first, then the two (1,0x20) aliases ordered by name, then
        // (2,0x20). The name table order deliberately differs from the address order.
        let psi = psi_stream(names, &[offsets[3], offsets[0], offsets[1], offsets[2]]);

        let image_bytes = image(&records, None, Some(psi));
        let mut pdb = open(image_bytes);
        let symbols = pdb.global_symbols().expect("symbol table");

        let at_1_20 = symbols
            .publics_at(PdbInternalSectionOffset {
                section: 1,
                offset: 0x20,
            })
            .expect("address lookup");
        c.check(at_1_20.len() == 2, "both same-address aliases are returned");
        c.check(
            at_1_20[0].index() == SymbolIndex(offsets[0])
                && at_1_20[1].index() == SymbolIndex(offsets[1]),
            "aliases keep their address-table order",
        );
        c.check(
            at_1_20[0]
                .parse()
                .expect("parse")
                .name()
                .unwrap()
                .as_bytes()
                == b"Alias"
                && at_1_20[1]
                    .parse()
                    .expect("parse")
                    .name()
                    .unwrap()
                    .as_bytes()
                    == b"BetaAlias",
            "address lookups return the original records",
        );

        let at_1_10 = symbols
            .publics_at(PdbInternalSectionOffset {
                section: 1,
                offset: 0x10,
            })
            .expect("address lookup");
        c.check(
            at_1_10.len() == 1 && at_1_10[0].index() == SymbolIndex(offsets[3]),
            "exact preceding address resolves",
        );
        c.check(
            matches!(
                at_1_10[0].parse().expect("parse"),
                pdb::SymbolData::Public(ref data) if data.offset == PdbInternalSectionOffset { section: 1, offset: 0x10 }
            ),
            "address lookup returns the exact address",
        );

        // An address absent from the table is a miss, not the previous address's symbol.
        let at_1_30 = symbols
            .publics_at(PdbInternalSectionOffset {
                section: 1,
                offset: 0x30,
            })
            .expect("address lookup");
        c.check(
            at_1_30.is_empty(),
            "an absent address does not return its predecessor",
        );

        // Section 2 has the same numeric offset as section 1 but must not be merged.
        let at_2_20 = symbols
            .publics_at(PdbInternalSectionOffset {
                section: 2,
                offset: 0x20,
            })
            .expect("address lookup");
        c.check(
            at_2_20.len() == 1 && at_2_20[0].index() == SymbolIndex(offsets[2]),
            "section 2 offset is separate",
        );
        c.check(
            at_2_20[0]
                .parse()
                .expect("parse")
                .name()
                .unwrap()
                .as_bytes()
                == b"Gamma",
            "section 2 address returns its own symbol",
        );

        // Address-table order is preserved even when it disagrees with SymbolIndex order. Here the
        // address map lists "Alias" before "alpha" (alphabetical order), while "alpha" appears
        // earlier in the symbol record stream.
        let order_records = vec![
            spec(b"alpha", true, 1, 0x40),
            spec(b"Alias", true, 1, 0x40),
        ];
        let order_offsets = symbol_stream(&order_records).1;
        let order_psi = psi_stream(
            name_table(&[(pdb::gsi_bucket(b"Alias"), order_offsets[1])]),
            &[order_offsets[1], order_offsets[0]],
        );
        let mut order_pdb = open(image(&order_records, None, Some(order_psi)));
        let order_symbols = order_pdb.global_symbols().expect("symbol table");
        let ordered = order_symbols
            .publics_at(PdbInternalSectionOffset { section: 1, offset: 0x40 })
            .expect("address lookup");
        c.check(ordered.len() == 2, "both aliases at the address are returned");
        c.check(
            ordered[0].index() == SymbolIndex(order_offsets[1])
                && ordered[1].index() == SymbolIndex(order_offsets[0]),
            "aliases keep address-table order instead of SymbolIndex order",
        );

        // The non-public record at the same address is not part of the address map.
        c.check(
            symbols
                .publics_at(PdbInternalSectionOffset {
                    section: 1,
                    offset: 0x20,
                })
                .expect("address lookup")
                .iter()
                .all(|s| s.index() != SymbolIndex(offsets[4])),
            "non-public records are not exposed through publics_at",
        );

        // The public name table still resolves independently of the address map.
        let alias = symbols
            .find_by_name(b"Alias", SymbolIndexKind::Public)
            .expect("public name lookup");
        c.check(
            alias.len() == 1 && alias[0].index() == SymbolIndex(offsets[0]),
            "public name lookup independent of address order",
        );
    });
}

#[test]
fn compatibility() {
    check::run("compatibility", |c| {
        // 1. A missing optional index (0xffff) reports unavailability.
        let records = vec![spec(b"Alpha", false, 1, 0x10), spec(b"Beta", true, 1, 0x20)];
        let (_, offsets) = symbol_stream(&records);
        let gsi = name_table(&[(pdb::gsi_bucket(b"Alpha"), offsets[0])]);
        let psi = psi_stream(
            name_table(&[(pdb::gsi_bucket(b"Beta"), offsets[1])]),
            &[offsets[1]],
        );

        let image = image_with(
            &records,
            Some(gsi.clone()),
            Some(psi.clone()),
            dbi_stream(0xffff, 0xffff, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        c.check(
            matches!(
                symbols.find_by_name(b"Alpha", SymbolIndexKind::Global),
                Err(pdb::Error::SymbolIndexUnavailable("global"))
            ),
            "a missing global index is unavailable",
        );
        c.check(
            matches!(
                symbols.find_by_name(b"Beta", SymbolIndexKind::Public),
                Err(pdb::Error::SymbolIndexUnavailable("public"))
            ),
            "a missing public index is unavailable",
        );
        c.check(
            matches!(
                symbols.publics_at(PdbInternalSectionOffset {
                    section: 1,
                    offset: 0x20
                }),
                Err(pdb::Error::SymbolIndexUnavailable("public"))
            ),
            "publics_at reports a missing public index",
        );
        // The ordinary iteration still works without indexes.
        let mut iter = symbols.iter();
        let mut seen = Vec::new();
        while let Some(symbol) = iter.next().expect("iteration") {
            seen.push(symbol.index());
        }
        c.check(
            seen == offsets.iter().map(|&o| SymbolIndex(o)).collect::<Vec<_>>(),
            "iteration is unaffected by missing indexes",
        );

        // 2. A declared but zero-length index is a valid empty index.
        let image = image_with(
            &records,
            Some(Vec::new()),
            Some(Vec::new()),
            dbi_stream(6, 7, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        c.check(
            symbols
                .find_by_name(b"Alpha", SymbolIndexKind::Global)
                .expect("empty index is valid")
                .is_empty(),
            "an empty global index yields no symbols",
        );
        c.check(
            symbols
                .find_by_name(b"Beta", SymbolIndexKind::Public)
                .expect("empty index is valid")
                .is_empty(),
            "an empty public name table yields no symbols",
        );
        c.check(
            symbols
                .publics_at(PdbInternalSectionOffset {
                    section: 1,
                    offset: 0x20,
                })
                .expect("empty index is valid")
                .is_empty(),
            "an empty address map yields no symbols",
        );

        // A declared, structurally valid but empty index (header + empty bitmap) is valid too.
        let image = image_with(
            &records,
            Some(name_table(&[])),
            Some(psi_stream(name_table(&[]), &[])),
            dbi_stream(6, 7, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        c.check(
            symbols
                .find_by_name(b"Alpha", SymbolIndexKind::Global)
                .expect("well-formed empty global index")
                .is_empty(),
            "a well-formed empty global index is a miss, not an error"
        );
        c.check(
            symbols
                .publics_at(PdbInternalSectionOffset { section: 1, offset: 0x10 })
                .expect("well-formed empty public index")
                .is_empty(),
            "a well-formed empty public index is a miss, not an error"
        );

        // 3. Declared stream number present in the DBI but missing from the MSF: unavailable.
        let image = image_with(&records, None, None, dbi_stream(6, 7, 5), &[], &[], &[]);
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        c.check(
            matches!(
                symbols.find_by_name(b"Alpha", SymbolIndexKind::Global),
                Err(pdb::Error::SymbolIndexUnavailable("global"))
            ),
            "a declared index whose stream is absent is unavailable",
        );

        // 4. Structural corruption is an error carrying the stream number and logical offset.
        let good_gsi = name_table(&[(pdb::gsi_bucket(b"Alpha"), offsets[0])]);
        let mut bad_signature = good_gsi.clone();
        bad_signature[0] = 0;
        let image = image_with(
            &records,
            Some(bad_signature),
            None,
            dbi_stream(6, 0xffff, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        match symbols.find_by_name(b"Alpha", SymbolIndexKind::Global) {
            Err(pdb::Error::InvalidSymbolIndex { stream, offset, .. }) => {
                c.check(stream == 6, "bad signature error names the index stream");
                c.check(offset == 0, "bad signature error names the logical offset");
            }
            other => c.check(
                false,
                &format!("expected invalid index error, got {:?}", other),
            ),
        }

        // Truncated table.
        let mut truncated = good_gsi.clone();
        truncated.truncate(8);
        let image = image_with(
            &records,
            Some(truncated),
            None,
            dbi_stream(6, 0xffff, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        match symbols.find_by_name(b"Alpha", SymbolIndexKind::Global) {
            Err(pdb::Error::InvalidSymbolIndex { stream, offset, .. }) => {
                c.check(stream == 6, "truncated table error names the stream");
                c.check(
                    offset == 8,
                    "truncated table error names the truncation offset",
                );
            }
            other => c.check(false, &format!("expected truncated error, got {:?}", other)),
        }

        // Zero symbol reference.
        let mut zero_ref = good_gsi.clone();
        zero_ref[16..20].copy_from_slice(&0u32.to_le_bytes());
        let image = image_with(
            &records,
            Some(zero_ref),
            None,
            dbi_stream(6, 0xffff, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        match symbols.find_by_name(b"Alpha", SymbolIndexKind::Global) {
            Err(pdb::Error::InvalidSymbolIndex { stream, offset, .. }) => {
                c.check(stream == 6, "zero reference error names the stream");
                c.check(offset == 16, "zero reference error names the record offset");
            }
            other => c.check(
                false,
                &format!("expected zero-reference error, got {:?}", other),
            ),
        }

        // Reference into the middle of a symbol record.
        let mid = offsets[0] + 2;
        let gsi_mid = name_table(&[(pdb::gsi_bucket(b"Alpha"), mid)]);
        let image = image_with(
            &records,
            Some(gsi_mid),
            None,
            dbi_stream(6, 0xffff, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        match symbols.find_by_name(b"Alpha", SymbolIndexKind::Global) {
            Err(pdb::Error::InvalidSymbolIndex { stream, offset, .. }) => {
                c.check(stream == 6, "mid-record error names the stream");
                c.check(offset == 16, "mid-record error names the record offset");
            }
            other => c.check(
                false,
                &format!("expected mid-record error, got {:?}", other),
            ),
        }

        // Out-of-range reference.
        let gsi_oor = name_table(&[(pdb::gsi_bucket(b"Alpha"), 0x10000)]);
        let image = image_with(
            &records,
            Some(gsi_oor),
            None,
            dbi_stream(6, 0xffff, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        match symbols.find_by_name(b"Alpha", SymbolIndexKind::Global) {
            Err(pdb::Error::InvalidSymbolIndex { stream, offset, .. }) => {
                c.check(stream == 6, "out-of-range error names the stream");
                c.check(offset == 16, "out-of-range error names the record offset");
            }
            other => c.check(
                false,
                &format!("expected out-of-range error, got {:?}", other),
            ),
        }

        // Illegal bucket chain start: not a multiple of the historical 12-byte stride.
        let mut bad_start = name_table(&[(pdb::gsi_bucket(b"Alpha"), offsets[0])]);
        let starts_start = 16 + 8 + 516;
        bad_start[starts_start..starts_start + 4].copy_from_slice(&5u32.to_le_bytes());
        let image = image_with(
            &records,
            Some(bad_start),
            None,
            dbi_stream(6, 0xffff, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        match symbols.find_by_name(b"Alpha", SymbolIndexKind::Global) {
            Err(pdb::Error::InvalidSymbolIndex { stream, offset, .. }) => {
                c.check(stream == 6, "bad chain start error names the stream");
                c.check(
                    offset == starts_start as u32,
                    "bad chain start error names the bucket-start offset",
                );
            }
            other => c.check(
                false,
                &format!("expected bad chain start error, got {:?}", other),
            ),
        }

        // A bad reference in a chain that is not the first must report that record's offset.
        let records2 = vec![
            spec(b"First", false, 1, 0x10),
            spec(b"Second", false, 1, 0x20),
            spec(b"Third", false, 1, 0x30),
        ];
        let (_, offsets2) = symbol_stream(&records2);
        let bucket_first = pdb::gsi_bucket(b"First");
        let bucket_third = pdb::gsi_bucket(b"Third");
        let mut gsi_second_bad =
            name_table(&[(bucket_first, offsets2[0]), (bucket_third, offsets2[2])]);
        // Corrupt the second serialized hash record with a zero reference. Its logical offset is
        // 16 (header) + 1 * 8 = 24.
        gsi_second_bad[24..28].copy_from_slice(&0u32.to_le_bytes());
        let image = image_with(
            &records2,
            Some(gsi_second_bad),
            None,
            dbi_stream(6, 0xffff, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        match symbols.find_by_name(b"Third", SymbolIndexKind::Global) {
            Err(pdb::Error::InvalidSymbolIndex { stream, offset, .. }) => {
                c.check(stream == 6, "later-chain error names the stream");
                c.check(
                    offset == 24,
                    "later-chain error names the actual hash record offset",
                );
            }
            other => c.check(
                false,
                &format!("expected later-chain error, got {:?}", other),
            ),
        }
        // The untouched bucket still resolves, and iteration still works.
        let found_first = symbols
            .find_by_name(b"First", SymbolIndexKind::Global)
            .expect("first bucket resolves");
        c.check(
            found_first.len() == 1 && found_first[0].index() == SymbolIndex(offsets2[0]),
            "an error in one chain does not break another bucket",
        );

        // Bitmap claiming a bucket that has no chain start.
        let mut bad_bitmap = name_table(&[(pdb::gsi_bucket(b"Alpha"), offsets[0])]);
        let bitmap_start = 16 + 8;
        // Set bit 1, which is not the occupied bucket (its bucket is not 1 for "Alpha").
        if pdb::gsi_bucket(b"Alpha") != 1 {
            bad_bitmap[bitmap_start + 1 / 8] |= 1 << (1 % 8);
        }
        let image = image_with(
            &records,
            Some(bad_bitmap),
            None,
            dbi_stream(6, 0xffff, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        match symbols.find_by_name(b"Alpha", SymbolIndexKind::Global) {
            Err(pdb::Error::InvalidSymbolIndex { stream, offset, .. }) => {
                c.check(stream == 6, "bitmap error names the stream");
                c.check(
                    offset == bitmap_start as u32,
                    "bitmap error names the bitmap offset",
                );
            }
            other => c.check(false, &format!("expected bitmap error, got {:?}", other)),
        }

        // 5. A public index must not accept a non-public address entry.
        let psi_bad = psi_stream(Vec::new(), &[offsets[0]]);
        let image = image_with(
            &records,
            None,
            Some(psi_bad),
            dbi_stream(0xffff, 7, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        match symbols.publics_at(PdbInternalSectionOffset {
            section: 1,
            offset: 0x10,
        }) {
            Err(pdb::Error::InvalidSymbolIndex { stream, offset, .. }) => {
                c.check(
                    stream == 7,
                    "non-public address entry error names the psi stream",
                );
                c.check(
                    offset == 28,
                    "non-public address entry error names the first address entry",
                );
            }
            other => c.check(
                false,
                &format!("expected non-public address error, got {:?}", other),
            ),
        }

        // 6. A public index whose name table references a non-public record is invalid.
        let bad_names = name_table(&[(pdb::gsi_bucket(b"Beta"), offsets[0])]);
        let psi_bad_names = psi_stream(bad_names, &[]);
        let image = image_with(
            &records,
            None,
            Some(psi_bad_names),
            dbi_stream(0xffff, 7, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        match symbols.find_by_name(b"Beta", SymbolIndexKind::Public) {
            Err(pdb::Error::InvalidSymbolIndex { stream, offset, .. }) => {
                c.check(
                    stream == 7,
                    "non-public name entry error names the psi stream",
                );
                c.check(
                    offset == 44,
                    "non-public name entry error names the nested record",
                );
            }
            other => c.check(
                false,
                &format!("expected non-public name error, got {:?}", other),
            ),
        }

        // 7. An address map that is not ordered by (section, offset, name) is invalid.
        let psi_unordered = psi_stream(Vec::new(), &[offsets[1], offsets[0]]);
        let image = image_with(
            &records,
            None,
            Some(psi_unordered),
            dbi_stream(0xffff, 7, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        match symbols.publics_at(PdbInternalSectionOffset {
            section: 1,
            offset: 0x20,
        }) {
            Err(pdb::Error::InvalidSymbolIndex { stream, offset, .. }) => {
                c.check(
                    stream == 7,
                    "unordered address map error names the psi stream",
                );
                c.check(
                    offset == 32,
                    "unordered address map error names the offending entry",
                );
            }
            other => c.check(
                false,
                &format!("expected unordered address error, got {:?}", other),
            ),
        }

        // 8. Declared thunk tables are unsupported rather than ignored.
        let mut psi_thunks = psi_stream(
            name_table(&[(pdb::gsi_bucket(b"Beta"), offsets[1])]),
            &[offsets[1]],
        );
        psi_thunks[8..12].copy_from_slice(&1u32.to_le_bytes());
        psi_thunks[12..16].copy_from_slice(&4u32.to_le_bytes());
        let image = image_with(
            &records,
            None,
            Some(psi_thunks),
            dbi_stream(0xffff, 7, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb = open(image);
        let symbols = pdb.global_symbols().expect("symbol table");
        c.check(
            matches!(
                symbols.find_by_name(b"Beta", SymbolIndexKind::Public),
                Err(pdb::Error::UnimplementedFeature(_))
            ),
            "declared thunk tables are reported as unsupported",
        );

        // 9. Corrupt indexes never disturb sequential iteration, iter_at, or module symbol reads.
        let corrupt_gsi = {
            let mut bytes = name_table(&[(pdb::gsi_bucket(b"Alpha"), offsets[0])]);
            bytes[0] = 0;
            bytes
        };
        let module_symbols = {
            let mut stream = vec![4u8, 0, 0, 0]; // CV_SIGNATURE_C13
            let (bytes, _) = symbol_stream(&[
                spec(b"LocalOne", false, 1, 0x10),
                spec(b"LocalTwo", false, 1, 0x20),
            ]);
            stream.extend(bytes);
            stream
        };
        let symbols_size = module_symbols.len() as u32;
        let dbi = dbi_stream_with_modules(6, 0xffff, 5, &[("module.obj", 8, symbols_size)]);
        let image = image_with(
            &records,
            Some(corrupt_gsi),
            Some(psi_stream(Vec::new(), &[])),
            dbi,
            &[],
            &[],
            &[Some(module_symbols)],
        );
        let mut pdb = open(image);
        {
            let symbols = pdb.global_symbols().expect("symbol table");
            c.check(
                matches!(
                    symbols.find_by_name(b"Alpha", SymbolIndexKind::Global),
                    Err(pdb::Error::InvalidSymbolIndex { .. })
                ),
                "corrupt index reports an error",
            );
            let mut iter = symbols.iter();
            let mut seen = Vec::new();
            while let Some(symbol) = iter.next().expect("iteration") {
                seen.push(symbol.index());
            }
            c.check(
                seen == offsets.iter().map(|&o| SymbolIndex(o)).collect::<Vec<_>>(),
                "iteration order survives a corrupt index",
            );
            let mut at = symbols.iter_at(SymbolIndex(offsets[1]));
            let next = at.next().expect("iter_at").expect("symbol");
            c.check(
                next.index() == SymbolIndex(offsets[1]),
                "iter_at resolves the requested record",
            );
            c.check(
                next.parse().expect("parse").name().unwrap().as_bytes() == b"Beta",
                "iter_at parses the record after a corrupt index",
            );
        }

        let dbi = pdb.debug_information().expect("debug information");
        let mut modules = dbi.modules().expect("modules");
        let module = modules.next().expect("module").expect("one module");
        let info = pdb
            .module_info(&module)
            .expect("module info")
            .expect("info present");
        let mut local = info.symbols().expect("module symbols");
        let mut local_names = Vec::new();
        while let Some(symbol) = local.next().expect("local iteration") {
            local_names.push(
                symbol
                    .parse()
                    .expect("parse")
                    .name()
                    .unwrap()
                    .as_bytes()
                    .to_vec(),
            );
        }
        c.check(
            local_names == vec![b"LocalOne".to_vec(), b"LocalTwo".to_vec()],
            "module-local symbols are unaffected by the global indexes",
        );

        // 10. Two symbol tables from different PDBs must not share index state.
        let records_a = vec![spec(b"Alpha", false, 1, 0x10)];
        let offsets_a = symbol_stream(&records_a).1;
        let records_b = vec![
            spec(b"Alpha", false, 1, 0x40),
            spec(b"Alpha", false, 1, 0x50),
        ];
        let offsets_b = symbol_stream(&records_b).1;
        let image_a = image_with(
            &records_a,
            Some(name_table(&[(pdb::gsi_bucket(b"Alpha"), offsets_a[0])])),
            None,
            dbi_stream(6, 0xffff, 5),
            &[],
            &[],
            &[],
        );
        let image_b = image_with(
            &records_b,
            Some(name_table(&[
                (pdb::gsi_bucket(b"Alpha"), offsets_b[0]),
                (pdb::gsi_bucket(b"Alpha"), offsets_b[1]),
            ])),
            None,
            dbi_stream(6, 0xffff, 5),
            &[],
            &[],
            &[],
        );
        let mut pdb_a = open(image_a);
        let mut pdb_b = open(image_b);
        let table_a = pdb_a.global_symbols().expect("a");
        let table_b = pdb_b.global_symbols().expect("b");
        let found_a = table_a
            .find_by_name(b"Alpha", SymbolIndexKind::Global)
            .expect("a lookup");
        let found_b = table_b
            .find_by_name(b"Alpha", SymbolIndexKind::Global)
            .expect("b lookup");
        c.check(found_a.len() == 1, "PDB A resolves its single record");
        c.check(found_b.len() == 2, "PDB B resolves both of its records");
        c.check(
            found_a[0].index() == SymbolIndex(offsets_a[0]),
            "PDB A's symbol index is not borrowed from PDB B",
        );
        c.check(
            found_b[0].index() == SymbolIndex(offsets_b[0])
                && found_b[1].index() == SymbolIndex(offsets_b[1]),
            "PDB B's symbol indexes are distinct from PDB A",
        );
        // Interleave the two tables again: offsets must stay tied to their owning stream.
        let found_a2 = table_a
            .find_by_name(b"Alpha", SymbolIndexKind::Global)
            .expect("a lookup again");
        c.check(
            found_a2.len() == 1 && found_a2[0].index() == SymbolIndex(offsets_a[0]),
            "interleaved use keeps PDB A's offsets",
        );
        let found_b2 = table_b
            .find_by_name(b"Alpha", SymbolIndexKind::Global)
            .expect("b lookup again");
        c.check(
            found_b2.len() == 2 && found_b2[1].index() == SymbolIndex(offsets_b[1]),
            "interleaved use keeps PDB B's offsets",
        );
    });
}
