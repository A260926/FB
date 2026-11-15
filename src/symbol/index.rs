// Copyright 2017 pdb Developers
//
// Licensed under the Apache License, Version 2.0, <LICENSE-APACHE or
// http://apache.org/licenses/LICENSE-2.0> or the MIT license <LICENSE-MIT or
// http://opensource.org/licenses/MIT>, at your option. This file may not be
// copied, modified, or distributed except according to those terms.

//! Name and address indexes for the global symbol table.
//!
//! The debug information stream declares two optional streams next to the symbol record stream:
//! the global symbol index ("GSI") and the public symbol index ("PSI"). Both contain a
//! `HashTable` version 7 structure that maps hashed names to symbol records. The public symbol
//! index additionally embeds a table of public symbol record offsets sorted by address.
//!
//! Layout references:
//!  - <https://github.com/microsoft/pdb-rs/blob/main/docs/pdb/globals.md>
//!  - <https://www.llvm.org/docs/doxygen/GSIStreamBuilder_8cpp_source.html>
//!  - <https://llvm.org/doxygen/PublicsStream_8cpp_source.html>

use crate::common::*;
use crate::msf::Stream;

use super::{Symbol, SymbolData};

/// The symbol index consulted by [`SymbolTable::find_by_name`](crate::SymbolTable::find_by_name).
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum SymbolIndexKind {
    /// The global symbol index declared by the DBI stream.
    Global,
    /// The name table embedded in the public symbol index declared by the DBI stream.
    Public,
}

/// The signature at the start of a serialized `HashTable`.
const GSI_SIGNATURE: u32 = 0xffff_ffff;
/// Version `V70` of the serialized `HashTable`.
const GSI_VERSION: u32 = 0xf12f_091a;
/// Size of the serialized `HashTable` header.
const GSI_HEADER_BYTES: usize = 16;
/// Size of one serialized hash record: a biased symbol offset and a reference count.
const HASH_RECORD_BYTES: usize = 8;
/// Size of the occupancy bitmap preceding the chain starts.
const BUCKET_BITMAP_BYTES: usize = 516;
/// Number of hash buckets of a non-minimal `HashTable`.
const BUCKET_COUNT: usize = 4096;
/// The historical in-memory size of a hash record. Chain starts are stored in this scale, not in
/// the serialized 8-byte stride.
const IN_MEMORY_RECORD_STRIDE: u32 = 12;
/// Size of the public symbol index header.
const PSI_HEADER_BYTES: usize = 28;

/// Returns the bucket of the global symbol index hash table for `name`.
///
/// This is Hash V1 as used by the PDB global symbol index (LLVM's `hashStringV1`): a polynomial
/// over the raw, case-sensitive name bytes, reduced modulo the table's 4096 buckets. Distinct
/// names may collide on one bucket, so an exact byte comparison against the original record name
/// is still performed after a bucket has been selected.
pub fn gsi_bucket(name: &[u8]) -> u32 {
    let mut hash = 0u32;
    for &byte in name {
        hash = hash.wrapping_mul(33).wrapping_add(u32::from(byte));
    }
    hash % BUCKET_COUNT as u32
}

fn read_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

fn invalid(stream: StreamIndex, offset: usize, reason: &'static str) -> Error {
    Error::InvalidSymbolIndex {
        stream: u32::from(stream.0),
        offset: offset as u32,
        reason,
    }
}

/// Returns the data of the symbol record at the given zero-based stream offset.
fn record_data(bytes: &[u8], offset: u32) -> Option<&[u8]> {
    let start = offset as usize;
    if start + 2 > bytes.len() {
        return None;
    }
    let length = usize::from(read_u16(bytes, start));
    if length < 2 || start + 2 + length > bytes.len() {
        return None;
    }
    Some(&bytes[start + 2..start + 2 + length])
}

/// Byte offsets of all symbol record starts in a symbol record stream.
#[derive(Debug)]
struct RecordBounds {
    starts: Vec<u32>,
}

impl RecordBounds {
    fn build(stream: &Stream<'_>) -> Result<Self> {
        let bytes = stream.as_slice();
        let mut starts = Vec::new();
        let mut position = 0usize;
        while position < bytes.len() {
            starts.push(position as u32);
            if bytes.len() - position < 2 {
                return Err(Error::UnexpectedEof);
            }
            let length = usize::from(read_u16(bytes, position));
            if length < 2 {
                return Err(Error::SymbolTooShort);
            }
            position += 2 + length;
            if position > bytes.len() {
                return Err(Error::UnexpectedEof);
            }
        }
        Ok(RecordBounds { starts })
    }

    fn contains(&self, offset: u32) -> bool {
        self.starts.binary_search(&offset).is_ok()
    }
}

/// One hash chain entry: the index of the serialized hash record and its biased symbol offset.
#[derive(Clone, Copy, Debug)]
struct ChainEntry {
    record: u32,
    biased_offset: u32,
}

/// A parsed serialized `HashTable`.
///
/// The table's structure (header, bitmap, chain starts) is validated when it is first parsed. The
/// hash records themselves are kept as raw, biased symbol references; a name query resolves and
/// validates only the chain of the bucket it hits.
#[derive(Debug)]
struct HashTable {
    /// Hash chain entries, grouped by bucket in chain order.
    buckets: Vec<Vec<ChainEntry>>,
    /// Logical offset of the first hash record within the index stream.
    records_start: usize,
}

impl HashTable {
    fn empty() -> Self {
        HashTable {
            buckets: vec![Vec::new(); BUCKET_COUNT],
            records_start: GSI_HEADER_BYTES,
        }
    }

    /// Parses and validates the structure of a serialized `HashTable` starting at `base` within
    /// `index_stream`. Symbol references are not resolved here.
    fn parse(index_stream: StreamIndex, bytes: &[u8], base: usize) -> Result<Self> {
        if bytes.is_empty() {
            return Ok(Self::empty());
        }

        if bytes.len() < GSI_HEADER_BYTES {
            return Err(invalid(
                index_stream,
                base + bytes.len(),
                "truncated global symbol index header",
            ));
        }

        let signature = read_u32(bytes, 0);
        let version = read_u32(bytes, 4);
        let hash_records_bytes = read_u32(bytes, 8) as usize;
        let bucket_region_bytes = read_u32(bytes, 12) as usize;

        if signature != GSI_SIGNATURE {
            return Err(invalid(
                index_stream,
                base,
                "invalid global symbol index signature",
            ));
        }
        if version != GSI_VERSION {
            return Err(invalid(
                index_stream,
                base + 4,
                "unsupported global symbol index version",
            ));
        }
        if hash_records_bytes % HASH_RECORD_BYTES != 0 {
            return Err(invalid(
                index_stream,
                base + 8,
                "hash record byte size is not a multiple of 8",
            ));
        }
        if bucket_region_bytes < BUCKET_BITMAP_BYTES
            || (bucket_region_bytes - BUCKET_BITMAP_BYTES) % 4 != 0
        {
            return Err(invalid(
                index_stream,
                base + 12,
                "invalid global symbol index bucket region",
            ));
        }

        let record_count = hash_records_bytes / HASH_RECORD_BYTES;
        let bucket_start_count = (bucket_region_bytes - BUCKET_BITMAP_BYTES) / 4;
        let records_start = GSI_HEADER_BYTES;
        let bitmap_start = records_start + hash_records_bytes;
        let starts_start = bitmap_start + BUCKET_BITMAP_BYTES;
        let table_end = starts_start + bucket_start_count * 4;

        if table_end > bytes.len() {
            return Err(invalid(
                index_stream,
                base + bytes.len(),
                "truncated global symbol index",
            ));
        }

        let mut occupied_buckets = Vec::new();
        for bucket in 0..BUCKET_COUNT {
            if bytes[bitmap_start + bucket / 8] & (1u8 << (bucket % 8)) != 0 {
                occupied_buckets.push(bucket);
            }
        }
        if occupied_buckets.len() != bucket_start_count {
            return Err(invalid(
                index_stream,
                base + bitmap_start,
                "bucket bitmap does not match the number of chain starts",
            ));
        }

        let mut starts = Vec::with_capacity(bucket_start_count);
        for k in 0..bucket_start_count {
            let position = starts_start + k * 4;
            let raw = read_u32(bytes, position);
            if raw % IN_MEMORY_RECORD_STRIDE != 0 {
                return Err(invalid(
                    index_stream,
                    base + position,
                    "invalid bucket chain start",
                ));
            }
            let index = (raw / IN_MEMORY_RECORD_STRIDE) as usize;
            if index > record_count {
                return Err(invalid(
                    index_stream,
                    base + position,
                    "bucket chain start is out of range",
                ));
            }
            if let Some(&previous) = starts.last() {
                if index <= previous {
                    return Err(invalid(
                        index_stream,
                        base + position,
                        "bucket chain starts are not increasing",
                    ));
                }
            }
            starts.push(index);
        }

        if let Some(&first) = starts.first() {
            if first != 0 {
                return Err(invalid(
                    index_stream,
                    base + starts_start,
                    "first bucket chain start is not zero",
                ));
            }
            if starts[starts.len() - 1] >= record_count {
                let position = starts_start + (starts.len() - 1) * 4;
                return Err(invalid(
                    index_stream,
                    base + position,
                    "bucket chain start is out of range",
                ));
            }
        }

        // Distribute the raw hash records over their buckets. References are resolved lazily.
        let mut buckets = vec![Vec::new(); BUCKET_COUNT];
        for (k, &bucket) in occupied_buckets.iter().enumerate() {
            let begin = starts[k];
            let end = if k + 1 < starts.len() {
                starts[k + 1]
            } else {
                record_count
            };
            let chain = &mut buckets[bucket];
            chain.reserve(end - begin);
            for record in begin..end {
                let position = records_start + record * HASH_RECORD_BYTES;
                chain.push(ChainEntry {
                    record: record as u32,
                    biased_offset: read_u32(bytes, position),
                });
            }
        }

        Ok(HashTable {
            buckets,
            records_start: base + records_start,
        })
    }

    /// Resolves and validates the chain of one bucket.
    ///
    /// A reference is only accepted if it is non-zero, points at the start of a symbol record, and
    /// (when `require_public` is set) that record parses as a public symbol. Failing references are
    /// reported with the logical offset of the hash record inside the index stream.
    fn resolve(
        &self,
        bucket: usize,
        index_stream: StreamIndex,
        symbol_stream: &Stream<'_>,
        bounds: &RecordBounds,
        require_public: bool,
    ) -> Result<Vec<u32>> {
        let mut offsets = Vec::new();
        for entry in &self.buckets[bucket] {
            let position = self.records_start + entry.record as usize * HASH_RECORD_BYTES;
            if entry.biased_offset == 0 {
                return Err(invalid(index_stream, position, "symbol reference is zero"));
            }
            let offset = entry.biased_offset - 1;
            if !bounds.contains(offset) {
                return Err(invalid(
                    index_stream,
                    position,
                    "symbol reference is not a record start",
                ));
            }
            if require_public {
                let data = record_data(symbol_stream.as_slice(), offset).ok_or_else(|| {
                    invalid(
                        index_stream,
                        position,
                        "symbol reference is not a valid symbol record",
                    )
                })?;
                match Symbol::from_parts(SymbolIndex(offset), data).parse() {
                    Ok(SymbolData::Public(_)) => {}
                    _ => {
                        return Err(invalid(
                            index_stream,
                            position,
                            "public symbol index references a non-public symbol",
                        ))
                    }
                }
            }
            offsets.push(offset);
        }
        Ok(offsets)
    }
}

/// A parsed public symbol index: its embedded name table and its address-ordered symbol offsets.
#[derive(Debug)]
struct PublicTable {
    names: HashTable,
    /// Address map entries, in their original order.
    addresses: Vec<u32>,
    /// Logical offset of the address map within the index stream.
    addresses_start: usize,
    /// Whether the address map has been checked against the symbol records.
    addresses_validated: std::cell::Cell<bool>,
}

impl PublicTable {
    fn empty() -> Self {
        PublicTable {
            names: HashTable::empty(),
            addresses: Vec::new(),
            addresses_start: PSI_HEADER_BYTES,
            addresses_validated: std::cell::Cell::new(true),
        }
    }

    fn parse(index: &IndexStream<'_>) -> Result<Self> {
        let bytes = index.stream.as_slice();
        if bytes.is_empty() {
            return Ok(Self::empty());
        }
        if bytes.len() < PSI_HEADER_BYTES {
            return Err(invalid(
                index.index,
                bytes.len(),
                "truncated public symbol index header",
            ));
        }

        let name_table_size = read_u32(bytes, 0) as usize;
        let address_map_size = read_u32(bytes, 4) as usize;
        let num_thunks = read_u32(bytes, 8);
        let size_of_thunk = read_u32(bytes, 12);
        let thunk_table_section = read_u16(bytes, 16);
        let thunk_table_offset = read_u32(bytes, 20);
        let num_sections = read_u32(bytes, 24);

        if num_thunks != 0
            || size_of_thunk != 0
            || thunk_table_section != 0
            || thunk_table_offset != 0
            || num_sections != 0
        {
            return Err(Error::UnimplementedFeature(
                "public symbol thunk or section offset tables",
            ));
        }

        let names_start = PSI_HEADER_BYTES;
        let names_end = names_start + name_table_size;
        let addresses_end = names_end + address_map_size;

        if addresses_end > bytes.len() {
            return Err(invalid(
                index.index,
                bytes.len(),
                "truncated public symbol index",
            ));
        }
        if address_map_size % 4 != 0 {
            return Err(invalid(
                index.index,
                names_end,
                "address map size is not a multiple of 4",
            ));
        }

        let names = HashTable::parse(index.index, &bytes[names_start..names_end], names_start)?;

        // The address map is validated lazily, the first time an address query needs it.
        let entry_count = address_map_size / 4;
        let mut addresses = Vec::with_capacity(entry_count);
        for i in 0..entry_count {
            let position = names_end + i * 4;
            addresses.push(read_u32(bytes, position));
        }

        Ok(PublicTable {
            names,
            addresses,
            addresses_start: names_end,
            addresses_validated: std::cell::Cell::new(false),
        })
    }

    /// Checks the address map against the symbol records, at most once.
    ///
    /// Every entry must reference the start of a public symbol record, and the entries must be
    /// ordered by `(section, offset, name)` as stored in the records themselves. The address
    /// table's order is independent of the name table's bucket order.
    fn validate_addresses(
        &self,
        index_stream: StreamIndex,
        symbol_stream: &Stream<'_>,
        bounds: &RecordBounds,
    ) -> Result<()> {
        if self.addresses_validated.get() {
            return Ok(());
        }

        let symbol_bytes = symbol_stream.as_slice();
        let mut previous: Option<(u16, u32, &[u8])> = None;
        for (i, &offset) in self.addresses.iter().enumerate() {
            let position = self.addresses_start + i * 4;
            if !bounds.contains(offset) {
                return Err(invalid(
                    index_stream,
                    position,
                    "public symbol reference is not a record start",
                ));
            }
            let data = record_data(symbol_bytes, offset).ok_or_else(|| {
                invalid(
                    index_stream,
                    position,
                    "public symbol reference is not a valid symbol record",
                )
            })?;
            let symbol = Symbol::from_parts(SymbolIndex(offset), data);
            let public = match symbol.parse()? {
                SymbolData::Public(public) => public,
                _ => {
                    return Err(invalid(
                        index_stream,
                        position,
                        "address map references a non-public symbol",
                    ))
                }
            };

            let key = (
                public.offset.section,
                public.offset.offset,
                public.name.as_bytes(),
            );
            if let Some(previous) = previous {
                if key < previous {
                    return Err(invalid(
                        index_stream,
                        position,
                        "address map is not ordered by section, offset and name",
                    ));
                }
            }
            previous = Some(key);
        }

        self.addresses_validated.set(true);
        Ok(())
    }
}

/// An optional index stream together with the stream number it was declared as.
#[derive(Debug)]
pub(crate) struct IndexStream<'s> {
    pub(crate) index: StreamIndex,
    pub(crate) stream: Stream<'s>,
}

impl<'s> IndexStream<'s> {
    pub(crate) fn new(index: StreamIndex, stream: Stream<'s>) -> Self {
        IndexStream { index, stream }
    }
}

/// Lazily built index state of a [`SymbolTable`](crate::SymbolTable).
///
/// Record boundaries and the parsed structure of each index are built on first use. Query results
/// are always derived from the original symbol record stream held by the owning symbol table.
#[derive(Debug, Default)]
pub(crate) struct IndexCache {
    record_starts: std::cell::RefCell<Option<RecordBounds>>,
    global: std::cell::RefCell<Option<HashTable>>,
    public: std::cell::RefCell<Option<PublicTable>>,
}

impl IndexCache {
    /// Makes sure the symbol record boundaries have been computed.
    fn ensure_bounds(&self, stream: &Stream<'_>) -> Result<()> {
        if self.record_starts.borrow().is_none() {
            let bounds = RecordBounds::build(stream)?;
            *self.record_starts.borrow_mut() = Some(bounds);
        }
        Ok(())
    }

    fn bounds(&self) -> std::cell::Ref<'_, RecordBounds> {
        std::cell::Ref::map(self.record_starts.borrow(), |bounds| {
            bounds.as_ref().expect("record boundaries were built")
        })
    }

    /// Returns the parsed global index, or an error if it is unavailable.
    fn global(&self, index: &IndexStream<'_>) -> Result<std::cell::Ref<'_, HashTable>> {
        if self.global.borrow().is_none() {
            let table = HashTable::parse(index.index, index.stream.as_slice(), 0)?;
            *self.global.borrow_mut() = Some(table);
        }
        Ok(std::cell::Ref::map(self.global.borrow(), |table| {
            table.as_ref().expect("global index was parsed")
        }))
    }

    /// Returns the parsed public index, or an error if it is unavailable.
    fn public(&self, index: &IndexStream<'_>) -> Result<std::cell::Ref<'_, PublicTable>> {
        if self.public.borrow().is_none() {
            *self.public.borrow_mut() = Some(PublicTable::parse(index)?);
        }
        Ok(std::cell::Ref::map(self.public.borrow(), |table| {
            table.as_ref().expect("public index was parsed")
        }))
    }
}

impl<'s> super::SymbolTable<'s> {
    /// Finds all symbols with the given exact name in the selected index.
    ///
    /// Only the bucket selected by Hash V1 of `name` is consulted. The references in that bucket
    /// are resolved back to the original symbol records; records whose name does not equal `name`
    /// byte-for-byte (hash collisions and case variants) are not returned.
    ///
    /// Multiple distinct records with the same name are all returned, sorted by their
    /// [`SymbolIndex`]. A record referenced more than once is returned once.
    ///
    /// # Errors
    ///
    /// * [`Error::SymbolIndexUnavailable`] if the selected index is not declared by this PDB
    /// * [`Error::InvalidSymbolIndex`] if the selected index is structurally invalid, or if a
    ///   reference in the index is zero, out of range, or not at a record start
    pub fn find_by_name(&self, name: &[u8], kind: SymbolIndexKind) -> Result<Vec<Symbol<'_>>> {
        let bucket = gsi_bucket(name) as usize;
        self.cache.ensure_bounds(&self.stream)?;
        let bounds = self.cache.bounds();

        let (index_stream, offsets) = match kind {
            SymbolIndexKind::Global => {
                let index = self
                    .global_index
                    .as_ref()
                    .ok_or(Error::SymbolIndexUnavailable("global"))?;
                let table = self.cache.global(index)?;
                let offsets = table.resolve(bucket, index.index, &self.stream, &bounds, false)?;
                (index.index, offsets)
            }
            SymbolIndexKind::Public => {
                let index = self
                    .public_index
                    .as_ref()
                    .ok_or(Error::SymbolIndexUnavailable("public"))?;
                let table = self.cache.public(index)?;
                let offsets =
                    table
                        .names
                        .resolve(bucket, index.index, &self.stream, &bounds, true)?;
                (index.index, offsets)
            }
        };

        let bytes = self.stream.as_slice();
        let mut symbols = Vec::with_capacity(offsets.len());
        for offset in offsets {
            let data = record_data(bytes, offset).ok_or(Error::InvalidSymbolIndex {
                stream: u32::from(index_stream.0),
                offset,
                reason: "symbol index reference is not a valid symbol record",
            })?;
            let symbol = Symbol::from_parts(SymbolIndex(offset), data);
            if let Some(symbol_name) = symbol.parse()?.name() {
                if symbol_name.as_bytes() == name {
                    symbols.push(symbol);
                }
            }
        }

        symbols.sort_by_key(|symbol| symbol.index());
        symbols.dedup_by_key(|symbol| symbol.index());
        Ok(symbols)
    }

    /// Finds all public symbols located at exactly the given section and offset.
    ///
    /// The address map of the public symbol index is scanned in its original order, so aliases
    /// sharing the same address are returned in address-table order. Only exact matches are
    /// returned: a symbol at a preceding address is not a match, and offsets in different
    /// sections are not merged.
    ///
    /// # Errors
    ///
    /// * [`Error::SymbolIndexUnavailable`] if this PDB has no public symbol index
    /// * [`Error::InvalidSymbolIndex`] if the index is structurally invalid, if an address map
    ///   entry does not reference a public symbol record, or if the address map is not ordered by
    ///   section, offset and name
    pub fn publics_at(&self, address: PdbInternalSectionOffset) -> Result<Vec<Symbol<'_>>> {
        self.cache.ensure_bounds(&self.stream)?;
        let bounds = self.cache.bounds();
        let index = self
            .public_index
            .as_ref()
            .ok_or(Error::SymbolIndexUnavailable("public"))?;
        let table = self.cache.public(index)?;
        table.validate_addresses(index.index, &self.stream, &bounds)?;

        let bytes = self.stream.as_slice();
        let mut symbols = Vec::new();
        for &offset in &table.addresses {
            let data = record_data(bytes, offset).ok_or_else(|| Error::InvalidSymbolIndex {
                stream: u32::from(index.index.0),
                offset,
                reason: "address map reference is not a valid symbol record",
            })?;
            let symbol = Symbol::from_parts(SymbolIndex(offset), data);
            if let SymbolData::Public(data) = symbol.parse()? {
                if data.offset == address {
                    symbols.push(symbol);
                }
            }
        }
        Ok(symbols)
    }
}
