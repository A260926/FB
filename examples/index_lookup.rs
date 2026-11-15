//! Demonstrates looking up symbols through the PDB's precomputed symbol indexes.
//!
//! The example builds a small in-memory PDB whose global symbol index and public symbol index are
//! populated from known symbol records, then queries both kinds of index on the same symbol table.

#[path = "../task_support/fixture.rs"]
mod fixture;

use pdb::{FallibleIterator, PdbInternalSectionOffset, SymbolIndexKind, PDB};

const S_GDATA32: u16 = 0x110d;
const S_PUB32: u16 = 0x110e;

struct Record {
    name: &'static [u8],
    public: bool,
    section: u16,
    offset: u32,
}

const RECORDS: &[Record] = &[
    Record {
        name: b"Alpha",
        public: false,
        section: 1,
        offset: 0x10,
    },
    Record {
        name: b"Gamma",
        public: true,
        section: 1,
        offset: 0x20,
    },
    Record {
        name: b"Delta",
        public: true,
        section: 1,
        offset: 0x20,
    },
    Record {
        name: b"Beta",
        public: true,
        section: 2,
        offset: 0x20,
    },
];

/// Serializes the records and returns the stream and each record's offset.
fn symbol_stream() -> (Vec<u8>, Vec<u32>) {
    let mut out = Vec::new();
    let mut offsets = Vec::new();
    for record in RECORDS {
        offsets.push(out.len() as u32);
        let mut rec = Vec::new();
        rec.extend_from_slice(&(if record.public { S_PUB32 } else { S_GDATA32 }).to_le_bytes());
        rec.extend_from_slice(&(if record.public { 3u32 } else { 0x74u32 }).to_le_bytes());
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

fn psi_stream(names: Vec<u8>, addresses: &[u32]) -> Vec<u8> {
    let mut psi = vec![0u8; 28];
    psi[0..4].copy_from_slice(&(names.len() as u32).to_le_bytes());
    psi[4..8].copy_from_slice(&((addresses.len() * 4) as u32).to_le_bytes());
    psi.extend(names);
    for address in addresses {
        psi.extend_from_slice(&address.to_le_bytes());
    }
    psi
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let (symbols_stream, offsets) = symbol_stream();

    let mut global_entries = Vec::new();
    let mut public_entries = Vec::new();
    for (record, &offset) in RECORDS.iter().zip(&offsets) {
        if record.public {
            public_entries.push((pdb::gsi_bucket(record.name), offset));
        } else {
            global_entries.push((pdb::gsi_bucket(record.name), offset));
        }
    }

    let mut dbi = vec![0u8; 64];
    dbi[0..4].copy_from_slice(&0xffff_ffffu32.to_le_bytes());
    dbi[4..8].copy_from_slice(&19990903u32.to_le_bytes());
    dbi[8..12].copy_from_slice(&1u32.to_le_bytes());
    dbi[12..14].copy_from_slice(&6u16.to_le_bytes());
    dbi[16..18].copy_from_slice(&7u16.to_le_bytes());
    dbi[20..22].copy_from_slice(&5u16.to_le_bytes());
    dbi[58..60].copy_from_slice(&0x8664u16.to_le_bytes());

    let mut info = vec![0u8; 48];
    info[0..4].copy_from_slice(&20000404u32.to_le_bytes());
    info[8..12].copy_from_slice(&1u32.to_le_bytes());
    for (i, byte) in info[12..28].iter_mut().enumerate() {
        *byte = (i + 1) as u8;
    }
    info[36..40].copy_from_slice(&1u32.to_le_bytes());

    let mut tpi = vec![0u8; 56];
    tpi[0..4].copy_from_slice(&20040203u32.to_le_bytes());
    tpi[4..8].copy_from_slice(&56u32.to_le_bytes());
    tpi[8..12].copy_from_slice(&0x1000u32.to_le_bytes());
    tpi[12..16].copy_from_slice(&0x1000u32.to_le_bytes());
    tpi[20..22].copy_from_slice(&0xffffu16.to_le_bytes());
    tpi[22..24].copy_from_slice(&0xffffu16.to_le_bytes());
    tpi[24..28].copy_from_slice(&4u32.to_le_bytes());
    tpi[28..32].copy_from_slice(&4096u32.to_le_bytes());

    let streams = vec![
        None,
        Some(info),
        Some(tpi),
        Some(dbi),
        None,
        Some(symbols_stream),
        Some(fixture::gsi(global_entries)),
        // Address entries are ordered by (section, offset, name): Delta before Gamma, then Beta.
        Some(psi_stream(
            fixture::gsi(public_entries),
            &[offsets[2], offsets[1], offsets[3]],
        )),
    ];
    let image = fixture::msf(&streams);

    let mut pdb = PDB::open(std::io::Cursor::new(image))?;
    let symbols = pdb.global_symbols()?;

    println!("--- name lookups ---");
    for (name, kind) in &[
        (&b"Alpha"[..], SymbolIndexKind::Global),
        (&b"Gamma"[..], SymbolIndexKind::Public),
        (&b"Delta"[..], SymbolIndexKind::Public),
    ] {
        let found = symbols.find_by_name(name, *kind)?;
        println!(
            "{:?} in {:?}: {} result(s)",
            String::from_utf8_lossy(name),
            *kind,
            found.len()
        );
        for symbol in found {
            let data = symbol.parse()?;
            match data {
                pdb::SymbolData::Public(public) => println!(
                    "  offset={:#x} public {:?} at section {} offset {:#x}",
                    symbol.index().0,
                    public.name.to_string(),
                    public.offset.section,
                    public.offset.offset
                ),
                other => println!(
                    "  offset={:#x} {:?}",
                    symbol.index().0,
                    other.name().map(|name| name.to_string())
                ),
            }
        }
    }

    println!("--- address lookups ---");
    for address in &[
        PdbInternalSectionOffset {
            section: 1,
            offset: 0x20,
        },
        PdbInternalSectionOffset {
            section: 2,
            offset: 0x20,
        },
    ] {
        let found = symbols.publics_at(*address)?;
        println!(
            "section {} offset {:#x}: {} result(s)",
            address.section,
            address.offset,
            found.len()
        );
        for symbol in found {
            let data = symbol.parse()?;
            if let pdb::SymbolData::Public(public) = data {
                println!("  {:?}", public.name.to_string());
            }
        }
    }

    let mut count = 0;
    let mut iter = symbols.iter();
    while let Some(symbol) = iter.next()? {
        println!(
            "iterated offset={:#x} name={:?}",
            symbol.index().0,
            symbol.parse()?.name().map(|n| n.to_string())
        );
        count += 1;
    }
    println!("iterated {} records total", count);
    Ok(())
}
