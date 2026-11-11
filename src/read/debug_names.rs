//! Parsing for the DWARF 5 `.debug_names` section.
//!
//! A `.debug_names` section contains one or more name index *contributions*.
//! Each contribution is self-contained: it has its own abbreviation table,
//! bucket/name tables, entry pool and name strings. This module provides
//! read-only access to those contributions, sequential enumeration of the
//! names and entries in a single contribution, and hash plus full-name lookup
//! within one contribution.
//!
//! ## Offset bases
//!
//! The DWARF 5 name index uses several different offset bases, and this module
//! keeps them distinct rather than flattening them into a single section
//! offset:
//!
//! * [`DebugNamesOffset`] is relative to the start of the whole `.debug_names`
//!   section.
//! * [`DebugNamesEntryOffset`] is relative to the start of the entry pool of
//!   one contribution. The entry offsets in the name table and the
//!   `DW_IDX_parent` references both use this base.
//! * [`DebugInfoOffset`] values from the compilation unit list are section
//!   offsets into `.debug_info`.
//! * [`UnitOffset`] values carried by `DW_IDX_die_offset` are relative to the
//!   start of the referenced compilation unit.

#![allow(non_upper_case_globals)]

use alloc::vec::Vec;
use core::convert::{TryFrom, TryInto};

use crate::common::{DebugInfoOffset, DebugNamesEntryOffset, DebugNamesOffset, Format, SectionId};
use crate::constants::{
    DW_FORM_data1, DW_FORM_data2, DW_FORM_data4, DW_FORM_data8, DW_FORM_flag_present, DW_FORM_ref4,
    DW_FORM_ref8, DW_FORM_udata, DW_IDX_compile_unit, DW_IDX_die_offset, DW_IDX_parent,
    DW_IDX_type_unit, DwForm, DwIdx, DwTag,
};
use crate::endianity::Endianity;
use crate::read::{
    EndianSlice, Error, Reader, ReaderOffset, ReaderOffsetId, Result, Section, UnitOffset,
};

/// The `DebugNames` struct represents the DWARF 5 name index information
/// found in the `.debug_names` section.
#[derive(Debug, Clone, Copy)]
pub struct DebugNames<R> {
    section: R,
}

impl<'input, Endian> DebugNames<EndianSlice<'input, Endian>>
where
    Endian: Endianity,
{
    /// Construct a new `DebugNames` instance from the data in the
    /// `.debug_names` section.
    ///
    /// It is the caller's responsibility to read the `.debug_names` section
    /// and present it as a `&[u8]` slice. That means using some ELF loader on
    /// Linux, a Mach-O loader on macOS, etc.
    ///
    /// An empty slice is a valid, empty `.debug_names` section.
    pub fn new(debug_names_section: &'input [u8], endian: Endian) -> Self {
        Self::from(EndianSlice::new(debug_names_section, endian))
    }
}

impl<R: Reader> DebugNames<R> {
    /// Iterate over the name index contributions in this section.
    ///
    /// A section may hold any number of contributions, including zero. If
    /// parsing a contribution fails, the iterator returns that error once and
    /// all further calls return `Ok(None)`; contributions successfully parsed
    /// before the error remain usable.
    pub fn units(&self) -> DebugNamesUnitIter<R> {
        DebugNamesUnitIter {
            base: self.section.clone(),
            input: self.section.clone(),
            done: false,
        }
    }
}

impl<R> Section<R> for DebugNames<R> {
    fn id() -> SectionId {
        SectionId::DebugNames
    }

    fn reader(&self) -> &R {
        &self.section
    }
}

impl<R> From<R> for DebugNames<R> {
    fn from(section: R) -> Self {
        DebugNames { section }
    }
}

/// An iterator over the name index contributions of a `.debug_names` section.
#[derive(Debug, Clone)]
pub struct DebugNamesUnitIter<R: Reader> {
    base: R,
    input: R,
    done: bool,
}

impl<R: Reader> DebugNamesUnitIter<R> {
    /// Advance the iterator and return the next name index contribution.
    ///
    /// Returns `Ok(None)` when all contributions have been parsed. Once an
    /// error is returned, all subsequent calls return `Ok(None)`.
    pub fn next(&mut self) -> Result<Option<NamesUnit<R>>> {
        if self.done {
            return Ok(None);
        }
        if self.input.is_empty() {
            self.done = true;
            return Ok(None);
        }
        match NamesUnit::parse(&self.base, &mut self.input) {
            Ok(unit) => Ok(Some(unit)),
            Err(e) => {
                self.done = true;
                self.input.empty();
                Err(e)
            }
        }
    }
}

#[cfg(feature = "fallible-iterator")]
impl<R: Reader> fallible_iterator::FallibleIterator for DebugNamesUnitIter<R> {
    type Item = NamesUnit<R>;
    type Error = Error;

    fn next(&mut self) -> core::result::Result<Option<Self::Item>, Self::Error> {
        self.next()
    }
}

/// One attribute specification from a name index abbreviation.
#[derive(Debug, Clone, Copy)]
struct AttrSpec {
    index: DwIdx,
    form: DwForm,
}

/// A parsed name index abbreviation.
#[derive(Debug, Clone)]
struct Abbrev {
    code: u64,
    tag: DwTag,
    attrs: Vec<AttrSpec>,
}

/// One record of the name table.
#[derive(Debug, Clone)]
struct NameRecord<R: Reader> {
    /// Position of this record in the name table (zero based).
    index: u32,
    /// The name hash stored alongside the record.
    hash: u32,
    /// The full, null-terminated name as stored in the name string area.
    name: R,
    /// Offset of the entry, relative to the start of the entry pool.
    entry_offset: R::Offset,
}

/// How a `DW_IDX_parent` attribute is encoded in a parsed entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ParentOperand {
    /// The entry has no `DW_IDX_parent` attribute, or it uses
    /// `DW_FORM_flag_present`, meaning the parent is not represented in this
    /// name index. A flag must not be confused with an offset of zero.
    NotInIndex,
    /// An offset relative to the start of the entry pool.
    PoolOffset(u64),
}

/// The values decoded from one entry pool entry.
#[derive(Debug, Clone)]
struct ParsedEntry {
    tag: DwTag,
    compile_unit: u32,
    die_offset: u64,
    parent: ParentOperand,
}

/// A single name index contribution within a `.debug_names` section.
///
/// All readers and offsets stored in this type are bounded by this
/// contribution's bytes; neighboring contributions in the same section are
/// never consulted.
#[derive(Debug, Clone)]
pub struct NamesUnit<R: Reader> {
    section_offset: R::Offset,
    format: Format,
    version: u16,
    bucket_count: u32,
    name_count: u32,
    /// Section offsets into `.debug_info` from the compilation unit list.
    cu_offsets: Vec<DebugInfoOffset<R::Offset>>,
    /// Validated bucket table values (1-based name table indexes).
    buckets: Vec<u32>,
    /// Parsed name table, in its on-disk order.
    names: Vec<NameRecord<R>>,
    /// Parsed abbreviation table.
    abbrevs: Vec<Abbrev>,
    /// Reader over the whole entry pool of this contribution.
    entry_pool: R,
    /// Start offsets of the declared entries, relative to the entry pool.
    entry_starts: Vec<R::Offset>,
}

impl<R: Reader> NamesUnit<R> {
    /// Return the offset of this contribution relative to the start of the
    /// `.debug_names` section.
    pub fn section_offset(&self) -> DebugNamesOffset<R::Offset> {
        DebugNamesOffset(self.section_offset)
    }

    /// Return the DWARF format (32- or 64-bit) of this contribution.
    pub fn format(&self) -> Format {
        self.format
    }

    /// Return the version of this contribution, which is always 5 for data
    /// that parses successfully.
    pub fn version(&self) -> u16 {
        self.version
    }

    /// Return the number of buckets in the bucket table. This may be zero, in
    /// which case the contribution can still be enumerated and looked up by a
    /// linear scan of the name table.
    pub fn bucket_count(&self) -> u32 {
        self.bucket_count
    }

    /// Return the number of names in the name table.
    pub fn name_count(&self) -> u32 {
        self.name_count
    }

    /// Return the compilation unit header offsets from the compilation unit
    /// list, in list order. These are section offsets into `.debug_info`.
    pub fn compilation_unit_offsets(&self) -> &[DebugInfoOffset<R::Offset>] {
        &self.cu_offsets
    }

    /// Check that `count * item_size` bytes fit in the remaining input,
    /// rejecting multiplication overflow without relying on debug panics.
    fn ensure_fits(remaining: R::Offset, count: u32, item_size: u64) -> Result<()> {
        let needed = u64::from(count)
            .checked_mul(item_size)
            .ok_or(Error::BadLength)?;
        if needed > remaining.into_u64() {
            return Err(Error::UnexpectedEof(ReaderOffsetId(needed)));
        }
        Ok(())
    }

    /// Parse one contribution starting at the current position of `input`,
    /// advancing `input` past the contribution on success.
    fn parse(base: &R, input: &mut R) -> Result<Self> {
        let section_offset = input.offset_from(base);
        let (length, format) = input.read_initial_length()?;
        // `rest` is bounded to exactly this contribution, so a truncated table
        // or string can never consume bytes of the next contribution.
        let mut rest = input.split(length)?;

        let version = rest.read_u16()?;
        if version != 5 {
            return Err(Error::UnknownVersion(u64::from(version)));
        }
        let comp_dir_count = rest.read_u32()?;
        let local_tu_count = rest.read_u32()?;
        let foreign_tu_count = rest.read_u32()?;
        // Only indexes pointing at compilation units are supported.
        if local_tu_count != 0 || foreign_tu_count != 0 {
            return Err(Error::UnsupportedNameIndexTypeUnit);
        }
        let bucket_count = rest.read_u32()?;
        let name_count = rest.read_u32()?;
        let abbrev_table_size = rest.read_u32()?;
        let entry_pool_size = rest.read_u32()?;

        // Reject counts whose tables cannot possibly fit before doing any
        // multiplication on their behalf.
        Self::ensure_fits(rest.len(), comp_dir_count, u64::from(format.word_size()))?;
        Self::ensure_fits(rest.len(), bucket_count, 4)?;
        Self::ensure_fits(rest.len(), name_count, 8)?;

        // Compilation unit list: one section offset per CU, sized by format.
        let mut cu_offsets = Vec::new();
        for _ in 0..comp_dir_count {
            cu_offsets.push(DebugInfoOffset(rest.read_offset(format)?));
        }

        // Bucket table: one u32 per bucket. Every nonzero value is a 1-based
        // index into the name table.
        let mut buckets = Vec::new();
        for _ in 0..bucket_count {
            let bucket = rest.read_u32()?;
            if bucket > name_count {
                return Err(Error::InvalidNameIndex);
            }
            buckets.push(bucket);
        }

        // Name table fixed part: name_count records of (u32 hash, u32 entry
        // offset). The entry offsets are always four bytes regardless of the
        // DWARF format.
        let mut name_records = Vec::new();
        for index in 0..name_count {
            let hash = rest.read_u32()?;
            let entry_offset_u32 = rest.read_u32()?;
            if u64::from(entry_offset_u32) > u64::from(entry_pool_size) {
                return Err(Error::InvalidNameIndex);
            }
            name_records.push((index, hash, R::Offset::from_u32(entry_offset_u32)));
        }

        // Abbreviation table.
        let abbrev_size = R::Offset::from_u32(abbrev_table_size);
        let mut abbrev_reader = rest.split(abbrev_size)?;
        let mut abbrevs = Vec::new();
        loop {
            let code = abbrev_reader.read_uleb128()?;
            if code == 0 {
                break;
            }
            if abbrevs.iter().any(|a: &Abbrev| a.code == code) {
                return Err(Error::DuplicateAbbreviationCode);
            }
            let tag = match u16::try_from(abbrev_reader.read_uleb128()?) {
                Ok(tag) => DwTag(tag),
                Err(_) => return Err(Error::InvalidNameIndex),
            };
            let mut attrs = Vec::new();
            loop {
                let index = match u16::try_from(abbrev_reader.read_uleb128()?) {
                    Ok(index) => DwIdx(index),
                    Err(_) => return Err(Error::InvalidNameIndex),
                };
                let form = match u16::try_from(abbrev_reader.read_uleb128()?) {
                    Ok(form) => DwForm(form),
                    Err(_) => return Err(Error::InvalidNameIndex),
                };
                if index.0 == 0 && form.0 == 0 {
                    break;
                }
                attrs.push(AttrSpec { index, form });
            }
            abbrevs.push(Abbrev { code, tag, attrs });
        }
        // The declared abbreviation table size must account for exactly the
        // abbreviations plus the terminator; trailing bytes would desync the
        // entry pool.
        if !abbrev_reader.is_empty() {
            return Err(Error::InvalidNameIndex);
        }

        // Entry pool.
        let entry_pool = rest.split(R::Offset::from_u32(entry_pool_size))?;

        // The remaining bytes are the name string area. An unterminated name
        // fails here rather than walking into the next contribution, because
        // `rest` is bounded to this contribution.
        let mut names = Vec::new();
        for (index, hash, entry_offset) in name_records {
            let name = rest.read_null_terminated_slice()?;
            names.push(NameRecord {
                index,
                hash,
                name,
                entry_offset,
            });
        }
        // Every byte of the contribution belongs to one of the tables;
        // trailing bytes mean the declared sizes were inconsistent.
        if !rest.is_empty() {
            return Err(Error::InvalidNameIndex);
        }

        let unit = NamesUnit {
            section_offset,
            format,
            version,
            bucket_count,
            name_count,
            cu_offsets,
            buckets,
            names,
            abbrevs,
            entry_pool,
            entry_starts: Vec::new(),
        };
        unit.parse_all_entries()
    }

    /// Walk every declared entry once to learn its start offset and validate
    /// the entry pool. Entries referenced by several name table records are
    /// only visited once.
    fn parse_all_entries(mut self) -> Result<Self> {
        let mut starts: Vec<R::Offset> = self
            .names
            .iter()
            .map(|record| record.entry_offset)
            .collect();
        starts.sort_unstable();
        starts.dedup();
        for start in &starts {
            // Decode and validate the whole entry at this offset.
            self.parse_entry_at(*start)?;
        }
        self.entry_starts = starts;
        Ok(self)
    }

    /// Return a reader positioned at the given entry pool offset.
    fn entry_reader(&self, offset: R::Offset) -> Result<R> {
        let mut reader = self.entry_pool.clone();
        reader.skip(offset)?;
        Ok(reader)
    }

    /// Decode the entry at an offset relative to the start of the entry pool.
    fn parse_entry_at(&self, offset: R::Offset) -> Result<ParsedEntry> {
        let mut input = self.entry_reader(offset)?;
        let code = input.read_uleb128()?;
        if code == 0 {
            return Err(Error::InvalidNameIndex);
        }
        let abbrev = match self.abbrevs.iter().find(|a| a.code == code) {
            Some(abbrev) => abbrev,
            None => return Err(Error::UnknownAbbreviation(code)),
        };

        let mut compile_unit = None;
        let mut die_offset = None;
        let mut parent = ParentOperand::NotInIndex;
        for attr in &abbrev.attrs {
            let value = self.read_attr_value(&mut input, attr)?;
            match attr.index {
                DW_IDX_compile_unit => {
                    compile_unit = Some(value_as_u32(value)?);
                }
                DW_IDX_die_offset => match value {
                    AttrValue::Number(offset) => die_offset = Some(offset),
                    AttrValue::Flag => return Err(Error::InvalidNameIndex),
                },
                DW_IDX_parent => match value {
                    AttrValue::Flag => parent = ParentOperand::NotInIndex,
                    AttrValue::Number(pool_offset) => {
                        parent = ParentOperand::PoolOffset(pool_offset)
                    }
                },
                DW_IDX_type_unit => {
                    // Type units are never supported, even if the header
                    // counts are zero.
                    return Err(Error::UnsupportedNameIndexTypeUnit);
                }
                _ => {
                    // Other indexes (e.g. DW_IDX_type_hash) are accepted and
                    // ignored as long as the form itself is supported.
                }
            }
        }

        // An entry without an explicit compile unit index may only exist in
        // a single-CU contribution, in which case index zero is inferred.
        let compile_unit = match compile_unit {
            Some(index) => {
                if u64::from(index) >= self.cu_offsets.len() as u64 {
                    return Err(Error::InvalidNameIndex);
                }
                index
            }
            None => {
                if self.cu_offsets.len() != 1 {
                    return Err(Error::InvalidNameIndex);
                }
                0
            }
        };

        let die_offset = match die_offset {
            Some(offset) => offset,
            None => return Err(Error::InvalidNameIndex),
        };

        Ok(ParsedEntry {
            tag: abbrev.tag,
            compile_unit,
            die_offset,
            parent,
        })
    }

    /// Read one abbreviation operand. Only the fixed-size integer forms,
    /// `udata`, the reference sized forms and `flag_present` are supported;
    /// anything else is reported explicitly.
    fn read_attr_value(&self, input: &mut R, attr: &AttrSpec) -> Result<AttrValue> {
        match attr.form {
            DW_FORM_data1 => input.read_u8().map(|v| AttrValue::Number(u64::from(v))),
            DW_FORM_data2 => input.read_u16().map(|v| AttrValue::Number(u64::from(v))),
            DW_FORM_data4 => input.read_u32().map(|v| AttrValue::Number(u64::from(v))),
            DW_FORM_data8 => input.read_u64().map(AttrValue::Number),
            DW_FORM_udata => input.read_uleb128().map(AttrValue::Number),
            DW_FORM_ref4 => input.read_u32().map(|v| AttrValue::Number(u64::from(v))),
            DW_FORM_ref8 => input.read_u64().map(AttrValue::Number),
            DW_FORM_flag_present => Ok(AttrValue::Flag),
            form => Err(Error::UnknownForm(form)),
        }
    }

    /// Build the public entry view for the given name table record.
    fn entry_for_record(&self, record: &NameRecord<R>) -> Result<NamesEntry<R>> {
        let parsed = self.parse_entry_at(record.entry_offset)?;
        let unit_header_offset = self.cu_offsets[parsed.compile_unit as usize];
        let die_offset = UnitOffset(R::Offset::from_u64(parsed.die_offset)?);
        Ok(NamesEntry {
            name_index: record.index,
            name: record.name.clone(),
            tag: parsed.tag,
            unit_header_offset,
            die_offset,
            entry_offset: DebugNamesEntryOffset(record.entry_offset),
        })
    }

    /// Iterate over every name table record, in name table order. One name may
    /// point at several entries through several records; each record is
    /// yielded separately.
    pub fn names(&self) -> NameEntryIter<'_, R> {
        NameEntryIter {
            unit: self,
            next: 0,
        }
    }

    /// Look up the entries for `name` within this contribution.
    ///
    /// The caller supplies both the name hash and the raw, full name bytes.
    /// The hash selects the bucket and candidate run, and the full name is
    /// compared against the contribution's name strings before any entry is
    /// returned, so two different names that share a hash never get confused.
    /// Hash computation and Unicode case folding are the caller's
    /// responsibility. All matching entries are returned in name table order,
    /// which is the order of the entry list.
    ///
    /// A contribution with no buckets (`bucket_count == 0`) is searched with
    /// a linear scan of the name table.
    pub fn lookup(&self, hash: u32, name: &[u8]) -> Result<NameLookupIter<'_, R>> {
        let mut candidates = Vec::new();
        if self.bucket_count == 0 {
            for record in &self.names {
                if record.hash == hash && self.name_equals(record, name)? {
                    candidates.push(record.index);
                }
            }
        } else {
            let bucket = self.buckets[(hash % self.bucket_count) as usize];
            if bucket != 0 {
                let mut index = bucket - 1;
                while index < self.name_count {
                    let record = &self.names[index as usize];
                    if record.hash != hash {
                        break;
                    }
                    if self.name_equals(record, name)? {
                        candidates.push(record.index);
                    }
                    index += 1;
                }
            }
        }
        Ok(NameLookupIter {
            unit: self,
            candidates,
            next: 0,
        })
    }

    fn name_equals(&self, record: &NameRecord<R>, name: &[u8]) -> Result<bool> {
        let actual = record.name.to_slice()?;
        Ok(actual.as_ref() == name)
    }

    /// Check that an entry pool offset points at the start of a declared
    /// entry. An offset into another entry's operand bytes, or past the pool,
    /// is an error.
    fn resolve_declared_entry(&self, offset: u64) -> Result<R::Offset> {
        let target = R::Offset::from_u64(offset)?;
        if !self.entry_starts.contains(&target) {
            return Err(Error::InvalidNameIndex);
        }
        Ok(target)
    }

    /// Resolve the single-step parent of an entry.
    ///
    /// Returns `Ok(None)` when the entry has no parent recorded in this name
    /// index (no `DW_IDX_parent` attribute, or a `DW_FORM_flag_present`).
    /// Otherwise returns the entry pool offset of the parent, after checking
    /// that it points at the start of a declared entry: an offset into an
    /// entry's operand bytes, or past the pool, is an error.
    pub fn entry_parent(
        &self,
        entry: &NamesEntry<R>,
    ) -> Result<Option<DebugNamesEntryOffset<R::Offset>>> {
        let parsed = self.parse_entry_at(entry.entry_offset.0)?;
        match parsed.parent {
            ParentOperand::NotInIndex => Ok(None),
            ParentOperand::PoolOffset(offset) => Ok(Some(DebugNamesEntryOffset(
                self.resolve_declared_entry(offset)?,
            ))),
        }
    }

    /// Return the parent chain of an entry, from its immediate parent up to
    /// the outermost recorded ancestor.
    ///
    /// Fails with [`Error::InvalidNameIndex`] if a parent target is missing,
    /// lands inside the operand bytes of another entry, or the chain forms a
    /// cycle; in those cases no chain is returned rather than looping forever.
    pub fn parent_chain(
        &self,
        entry: &NamesEntry<R>,
    ) -> Result<Vec<DebugNamesEntryOffset<R::Offset>>> {
        let mut chain = Vec::new();
        let mut current = entry.entry_offset.0;
        loop {
            let parsed = self.parse_entry_at(current)?;
            match parsed.parent {
                ParentOperand::NotInIndex => return Ok(chain),
                ParentOperand::PoolOffset(offset) => {
                    let target = self.resolve_declared_entry(offset)?;
                    let parent = DebugNamesEntryOffset(target);
                    if chain.contains(&parent) {
                        return Err(Error::InvalidNameIndex);
                    }
                    chain.push(parent);
                    current = target;
                }
            }
        }
    }
}

#[derive(Debug, Clone, Copy)]
enum AttrValue {
    Number(u64),
    Flag,
}

fn value_as_u32(value: AttrValue) -> Result<u32> {
    match value {
        AttrValue::Number(v) => v.try_into().map_err(|_| Error::InvalidNameIndex),
        AttrValue::Flag => Err(Error::InvalidNameIndex),
    }
}

/// A successfully parsed entry from a name index contribution.
#[derive(Debug, Clone)]
pub struct NamesEntry<R: Reader> {
    name_index: u32,
    name: R,
    tag: DwTag,
    unit_header_offset: DebugInfoOffset<R::Offset>,
    die_offset: UnitOffset<R::Offset>,
    entry_offset: DebugNamesEntryOffset<R::Offset>,
}

impl<R: Reader> NamesEntry<R> {
    /// The zero-based position of this entry's record in the name table.
    pub fn name_index(&self) -> u32 {
        self.name_index
    }

    /// The full name bytes as stored in the contribution's name string area.
    pub fn name(&self) -> &R {
        &self.name
    }

    /// The entry's tag, taken from its abbreviation.
    pub fn tag(&self) -> DwTag {
        self.tag
    }

    /// The `.debug_info` section offset of the compilation unit header that
    /// owns this entry. This is a section offset, not unit relative.
    pub fn unit_header_offset(&self) -> DebugInfoOffset<R::Offset> {
        self.unit_header_offset
    }

    /// The offset of the DIE relative to the start of its compilation unit.
    pub fn die_offset(&self) -> UnitOffset<R::Offset> {
        self.die_offset
    }

    /// The position of this entry within the contribution's entry pool. The
    /// base is the start of the entry pool, not the start of the section.
    pub fn entry_offset(&self) -> DebugNamesEntryOffset<R::Offset> {
        self.entry_offset
    }
}

/// An iterator over the entries of a [`NamesUnit`], in name table order.
#[derive(Debug, Clone)]
pub struct NameEntryIter<'unit, R: Reader> {
    unit: &'unit NamesUnit<R>,
    next: u32,
}

impl<'unit, R: Reader> NameEntryIter<'unit, R> {
    /// Advance the iterator, returning the next name table entry.
    pub fn next(&mut self) -> Result<Option<NamesEntry<R>>> {
        if self.next >= self.unit.names.len() as u32 {
            return Ok(None);
        }
        let record = &self.unit.names[self.next as usize];
        self.next += 1;
        self.unit.entry_for_record(record).map(Some)
    }
}

#[cfg(feature = "fallible-iterator")]
impl<'unit, R: Reader> fallible_iterator::FallibleIterator for NameEntryIter<'unit, R> {
    type Item = NamesEntry<R>;
    type Error = Error;

    fn next(&mut self) -> core::result::Result<Option<Self::Item>, Self::Error> {
        self.next()
    }
}

/// An iterator over the entries matched by [`NamesUnit::lookup`].
#[derive(Debug, Clone)]
pub struct NameLookupIter<'unit, R: Reader> {
    unit: &'unit NamesUnit<R>,
    candidates: Vec<u32>,
    next: usize,
}

impl<'unit, R: Reader> NameLookupIter<'unit, R> {
    /// Advance the iterator, returning the next matching entry in name table
    /// order.
    pub fn next(&mut self) -> Result<Option<NamesEntry<R>>> {
        if self.next >= self.candidates.len() {
            return Ok(None);
        }
        let name_index = self.candidates[self.next];
        self.next += 1;
        let record = &self.unit.names[name_index as usize];
        self.unit.entry_for_record(record).map(Some)
    }
}

#[cfg(feature = "fallible-iterator")]
impl<'unit, R: Reader> fallible_iterator::FallibleIterator for NameLookupIter<'unit, R> {
    type Item = NamesEntry<R>;
    type Error = Error;

    fn next(&mut self) -> core::result::Result<Option<Self::Item>, Self::Error> {
        self.next()
    }
}
