/// === Sym bindings ===
/// Local symbol.
pub const STB_LOCAL: u8 = 0;
/// Global symbol.
pub const STB_GLOBAL: u8 = 1;
/// Weak symbol.
pub const STB_WEAK: u8 = 2;
/// Number of defined types..
pub const STB_NUM: u8 = 3;
/// Start of OS-specific.
pub const STB_LOOS: u8 = 10;
/// Unique symbol..
pub const STB_GNU_UNIQUE: u8 = 10;
/// End of OS-specific.
pub const STB_HIOS: u8 = 12;
/// Start of processor-specific.
pub const STB_LOPROC: u8 = 13;
/// End of processor-specific.
pub const STB_HIPROC: u8 = 15;

/// === Sym types ===
/// Symbol type is unspecified.
pub const STT_NOTYPE: u8 = 0;
/// Symbol is a data object.
pub const STT_OBJECT: u8 = 1;
/// Symbol is a code object.
pub const STT_FUNC: u8 = 2;
/// Symbol associated with a section.
pub const STT_SECTION: u8 = 3;
/// Symbol's name is file name.
pub const STT_FILE: u8 = 4;
/// Symbol is a common data object.
pub const STT_COMMON: u8 = 5;
/// Symbol is thread-local data object.
pub const STT_TLS: u8 = 6;
/// Number of defined types.
pub const STT_NUM: u8 = 7;
/// Start of OS-specific.
pub const STT_LOOS: u8 = 10;
/// Symbol is indirect code object.
pub const STT_GNU_IFUNC: u8 = 10;
/// End of OS-specific.
pub const STT_HIOS: u8 = 12;
/// Start of processor-specific.
pub const STT_LOPROC: u8 = 13;
/// End of processor-specific.
pub const STT_HIPROC: u8 = 15;

/// === Sym visibility ===
/// Default: Visibility is specified by the symbol's binding type
pub const STV_DEFAULT: u8 = 0;
/// Internal: use of this attribute is currently reserved.
pub const STV_INTERNAL: u8 = 1;
/// Hidden: Not visible to other components, necessarily protected. Binding scope becomes local
/// when the object is included in an executable or shared object.
pub const STV_HIDDEN: u8 = 2;
/// Protected: Symbol defined in current component is visible in other components, but cannot be preempted.
/// Any reference from within the defining component must be resolved to the definition in that
/// component.
pub const STV_PROTECTED: u8 = 3;
/// Exported: ensures a symbol remains global, cannot be demoted or eliminated by any other symbol
/// visibility technique.
pub const STV_EXPORTED: u8 = 4;
/// Singleton: ensures a symbol remains global, and that a single instance of the definition is
/// bound to by all references within a process. Cannot be demoted or eliminated.
pub const STV_SINGLETON: u8 = 5;
/// Eliminate: extends the hidden attribute. Not written in any symbol table of a dynamic
/// executable or shared object.
pub const STV_ELIMINATE: u8 = 6;

/// Get the ST bind.
///
/// This is the first four bits of the "info" byte.
#[inline]
pub fn st_bind(info: u8) -> u8 {
    info >> 4
}

/// Get the ST type.
///
/// This is the last four bits of the "info" byte.
#[inline]
pub fn st_type(info: u8) -> u8 {
    info & 0xf
}

/// Get the ST visibility.
///
/// This is the last three bits of the "other" byte.
#[inline]
pub fn st_visibility(other: u8) -> u8 {
    other & 0x7
}

/// Is this information defining an import?
#[inline]
pub fn is_import(info: u8, value: u64) -> bool {
    let bind = st_bind(info);
    bind == STB_GLOBAL && value == 0
}

/// Convenience function to get the &'static str type from the symbols `st_info`.
#[inline]
pub fn get_type(info: u8) -> &'static str {
    type_to_str(st_type(info))
}

/// Get the string for some bind.
#[inline]
pub fn bind_to_str(typ: u8) -> &'static str {
    match typ {
        STB_LOCAL => "LOCAL",
        STB_GLOBAL => "GLOBAL",
        STB_WEAK => "WEAK",
        STB_NUM => "NUM",
        STB_GNU_UNIQUE => "GNU_UNIQUE",
        _ => "UNKNOWN_STB",
    }
}

/// Get the string for some type.
#[inline]
pub fn type_to_str(typ: u8) -> &'static str {
    match typ {
        STT_NOTYPE => "NOTYPE",
        STT_OBJECT => "OBJECT",
        STT_FUNC => "FUNC",
        STT_SECTION => "SECTION",
        STT_FILE => "FILE",
        STT_COMMON => "COMMON",
        STT_TLS => "TLS",
        STT_NUM => "NUM",
        STT_GNU_IFUNC => "GNU_IFUNC",
        _ => "UNKNOWN_STT",
    }
}

/// Get the string for some visibility
#[inline]
pub fn visibility_to_str(typ: u8) -> &'static str {
    match typ {
        STV_DEFAULT => "DEFAULT",
        STV_INTERNAL => "INTERNAL",
        STV_HIDDEN => "HIDDEN",
        STV_PROTECTED => "PROTECTED",
        STV_EXPORTED => "EXPORTED",
        STV_SINGLETON => "SINGLETON",
        STV_ELIMINATE => "ELIMINATE",
        _ => "UNKNOWN_STV",
    }
}

/// Where a symbol is defined, according to its `st_shndx` value together with
/// any attached `SHT_SYMTAB_SHNDX` extension table.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum SymbolSection {
    /// `SHN_UNDEF`: the symbol is undefined and must be resolved elsewhere.
    Undefined,
    /// `SHN_ABS`: the symbol is an absolute value.
    Absolute,
    /// `SHN_COMMON`: the symbol is a common block symbol.
    Common,
    /// One of the reserved section indices (`SHN_LORESERVE`..`SHN_HIRESERVE`)
    /// other than `SHN_ABS`, `SHN_COMMON` or `SHN_XINDEX`; the raw 16-bit
    /// encoding is preserved.
    Reserved(u16),
    /// An ordinary section header index. Large indices that were encoded as
    /// `SHN_XINDEX` and resolved through an extension table are reported here
    /// as well, even when their numeric value falls inside the reserved range.
    Section(usize),
}

/// Classify a symbol's defining section from its raw 16-bit encoding and the
/// (possibly extension-resolved) section index.
///
/// A raw `SHN_XINDEX` value is reported as [`SymbolSection::Reserved`] here;
/// [`Symtab::section_kind`] upgrades it to [`SymbolSection::Section`] once an
/// attached extension table resolves it.
pub(crate) fn classify_symbol_section(raw: u16, resolved: usize) -> SymbolSection {
    use crate::elf::section_header as sh;
    match u32::from(raw) {
        sh::SHN_UNDEF => SymbolSection::Undefined,
        sh::SHN_ABS => SymbolSection::Absolute,
        sh::SHN_COMMON => SymbolSection::Common,
        sh::SHN_LORESERVE..=sh::SHN_HIRESERVE => SymbolSection::Reserved(raw),
        _ => SymbolSection::Section(resolved),
    }
}

macro_rules! elf_sym_std_impl {
    ($size:ty) => {
        #[cfg(test)]
        mod tests {
            use super::*;
            #[test]
            fn size_of() {
                assert_eq!(::std::mem::size_of::<Sym>(), SIZEOF_SYM);
            }
        }

        use crate::elf::sym::Sym as ElfSym;

        use core::fmt;
        use core::slice;

        impl Sym {
            /// Checks whether this `Sym` has `STB_GLOBAL`/`STB_WEAK` bind and a `st_value` of 0
            #[inline]
            pub fn is_import(&self) -> bool {
                let bind = self.st_info >> 4;
                (bind == STB_GLOBAL || bind == STB_WEAK) && self.st_value == 0
            }
            /// Checks whether this `Sym` has type `STT_FUNC`
            #[inline]
            pub fn is_function(&self) -> bool {
                st_type(self.st_info) == STT_FUNC
            }
        }

        impl From<Sym> for ElfSym {
            #[inline]
            fn from(sym: Sym) -> Self {
                ElfSym {
                    st_name: sym.st_name as usize,
                    st_info: sym.st_info,
                    st_other: sym.st_other,
                    st_shndx: sym.st_shndx as usize,
                    st_value: u64::from(sym.st_value),
                    st_size: u64::from(sym.st_size),
                }
            }
        }

        impl From<ElfSym> for Sym {
            #[inline]
            fn from(sym: ElfSym) -> Self {
                Sym {
                    st_name: sym.st_name as u32,
                    st_info: sym.st_info,
                    st_other: sym.st_other,
                    st_shndx: sym.st_shndx as u16,
                    st_value: sym.st_value as $size,
                    st_size: sym.st_size as $size,
                }
            }
        }

        impl fmt::Debug for Sym {
            fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
                let bind = st_bind(self.st_info);
                let typ = st_type(self.st_info);
                let vis = st_visibility(self.st_other);
                f.debug_struct("Sym")
                    .field("st_name", &self.st_name)
                    .field("st_value", &format_args!("{:x}", self.st_value))
                    .field("st_size", &self.st_size)
                    .field(
                        "st_info",
                        &format_args!(
                            "{:x} {} {}",
                            self.st_info,
                            bind_to_str(bind),
                            type_to_str(typ)
                        ),
                    )
                    .field(
                        "st_other",
                        &format_args!("{} {}", self.st_other, visibility_to_str(vis)),
                    )
                    .field("st_shndx", &self.st_shndx)
                    .finish()
            }
        }

        /// # Safety
        ///
        /// This function creates a `Sym` slice directly from a raw pointer
        #[inline]
        pub unsafe fn from_raw<'a>(symp: *const Sym, count: usize) -> &'a [Sym] {
            unsafe { slice::from_raw_parts(symp, count) }
        }

        if_std! {
            use crate::error::Result;

            use std::fs::File;
            use std::io::{Read, Seek};
            use std::io::SeekFrom::Start;

            pub fn from_fd(fd: &mut File, offset: usize, count: usize) -> Result<Vec<Sym>> {
                // TODO: AFAIK this shouldn't work, since i pass in a byte size...
                let mut syms = vec![Sym::default(); count];
                fd.seek(Start(offset as u64))?;
                unsafe {
                    fd.read_exact(plain::as_mut_bytes(&mut *syms))?;
                }
                syms.dedup();
                Ok(syms)
            }
        }
    };
}

#[cfg(feature = "alloc")]
use scroll::{Pread, Pwrite, SizeWith};

pub mod sym32 {
    pub use crate::elf::sym::*;

    #[repr(C)]
    #[derive(Clone, Copy, PartialEq, Default)]
    #[cfg_attr(feature = "alloc", derive(Pread, Pwrite, SizeWith))]
    /// 32-bit Sym - used for both static and dynamic symbol information in a binary
    pub struct Sym {
        /// Symbol name (string tbl index)
        pub st_name: u32,
        /// Symbol value
        pub st_value: u32,
        /// Symbol size
        pub st_size: u32,
        /// Symbol type and binding
        pub st_info: u8,
        /// Symbol visibility
        pub st_other: u8,
        /// Section index
        pub st_shndx: u16,
    }

    // Declare that the type is plain.
    unsafe impl plain::Plain for Sym {}

    pub const SIZEOF_SYM: usize = 4 + 1 + 1 + 2 + 4 + 4;

    elf_sym_std_impl!(u32);
}

pub mod sym64 {
    pub use crate::elf::sym::*;

    #[repr(C)]
    #[derive(Clone, Copy, PartialEq, Default)]
    #[cfg_attr(feature = "alloc", derive(Pread, Pwrite, SizeWith))]
    /// 64-bit Sym - used for both static and dynamic symbol information in a binary
    pub struct Sym {
        /// Symbol name (string tbl index)
        pub st_name: u32,
        /// Symbol type and binding
        pub st_info: u8,
        /// Symbol visibility
        pub st_other: u8,
        /// Section index
        pub st_shndx: u16,
        /// Symbol value
        pub st_value: u64,
        /// Symbol size
        pub st_size: u64,
    }

    // Declare that the type is plain.
    unsafe impl plain::Plain for Sym {}

    pub const SIZEOF_SYM: usize = 4 + 1 + 1 + 2 + 8 + 8;

    elf_sym_std_impl!(u64);
}

use crate::container::{Container, Ctx};
#[cfg(feature = "alloc")]
use crate::error::Result;
use crate::options::Permissive;
#[cfg(feature = "alloc")]
use alloc::vec::Vec;
use core::fmt;
use scroll::ctx;
use scroll::ctx::SizeWith;

#[derive(Clone, Copy, PartialEq, Default)]
/// A unified Sym definition - convertible to and from 32-bit and 64-bit variants
pub struct Sym {
    pub st_name: usize,
    pub st_info: u8,
    pub st_other: u8,
    pub st_shndx: usize,
    pub st_value: u64,
    pub st_size: u64,
}

impl Sym {
    #[inline]
    pub fn size(container: Container) -> usize {
        Self::size_with(&Ctx::from(container))
    }
    /// Checks whether this `Sym` has `STB_GLOBAL`/`STB_WEAK` bind and a `st_value` of 0
    #[inline]
    pub fn is_import(&self) -> bool {
        let bind = self.st_bind();
        (bind == STB_GLOBAL || bind == STB_WEAK) && self.st_value == 0
    }
    /// Checks whether this `Sym` has type `STT_FUNC`
    #[inline]
    pub fn is_function(&self) -> bool {
        st_type(self.st_info) == STT_FUNC
    }
    /// Get the ST bind.
    ///
    /// This is the first four bits of the "info" byte.
    #[inline]
    pub fn st_bind(&self) -> u8 {
        self.st_info >> 4
    }
    /// Get the ST type.
    ///
    /// This is the last four bits of the "info" byte.
    #[inline]
    pub fn st_type(&self) -> u8 {
        st_type(self.st_info)
    }
    /// Get the ST visibility.
    ///
    /// This is the last three bits of the "other" byte.
    #[inline]
    pub fn st_visibility(&self) -> u8 {
        st_visibility(self.st_other)
    }
    /// The raw 16-bit `st_shndx` encoding as stored in the symbol entry.
    ///
    /// For symbols read from a [`Symtab`] without an attached
    /// `SHT_SYMTAB_SHNDX` extension table this is identical to `st_shndx`.
    /// Symbols whose table has an attachment should be inspected through
    /// [`Symtab::raw_shndx`] to obtain the raw encoding.
    #[inline]
    pub fn raw_st_shndx(&self) -> u16 {
        self.st_shndx as u16
    }
    /// Classify the section that defines this symbol.
    ///
    /// This classification is based on the value as carried by this `Sym`
    /// value. For symbols resolved through an attached extension table, use
    /// [`Symtab::section_kind`], which additionally remembers the original
    /// 16-bit encoding.
    #[inline]
    pub fn symbol_section(&self) -> SymbolSection {
        classify_symbol_section(self.raw_st_shndx(), self.st_shndx)
    }
    #[cfg(feature = "endian_fd")]
    /// Parse `count` vector of ELF symbols from `offset`
    pub fn parse(bytes: &[u8], mut offset: usize, count: usize, ctx: Ctx) -> Result<Vec<Sym>> {
        if count > bytes.len() / Sym::size_with(&ctx) {
            return Err(crate::error::Error::BufferTooShort(count, "symbols"));
        }
        let mut syms = Vec::with_capacity(count);
        for _ in 0..count {
            let sym = bytes.gread_with(&mut offset, ctx)?;
            syms.push(sym);
        }
        Ok(syms)
    }
}

impl fmt::Debug for Sym {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let bind = self.st_bind();
        let typ = self.st_type();
        let vis = self.st_visibility();
        f.debug_struct("Sym")
            .field("st_name", &self.st_name)
            .field(
                "st_info",
                &format_args!(
                    "0x{:x} {} {}",
                    self.st_info,
                    bind_to_str(bind),
                    type_to_str(typ)
                ),
            )
            .field(
                "st_other",
                &format_args!("{} {}", self.st_other, visibility_to_str(vis)),
            )
            .field("st_shndx", &self.st_shndx)
            .field("st_value", &format_args!("0x{:x}", self.st_value))
            .field("st_size", &self.st_size)
            .finish()
    }
}

impl ctx::SizeWith<Ctx> for Sym {
    #[inline]
    fn size_with(&Ctx { container, .. }: &Ctx) -> usize {
        match container {
            Container::Little => sym32::SIZEOF_SYM,
            Container::Big => sym64::SIZEOF_SYM,
        }
    }
}

if_alloc! {
    use core::result;

    impl<'a> ctx::TryFromCtx<'a, Ctx> for Sym {
        type Error = crate::error::Error;
        #[inline]
        fn try_from_ctx(bytes: &'a [u8], Ctx { container, le}: Ctx) -> result::Result<(Self, usize), Self::Error> {
            let sym = match container {
                Container::Little => {
                    (bytes.pread_with::<sym32::Sym>(0, le)?.into(), sym32::SIZEOF_SYM)
                },
                Container::Big => {
                    (bytes.pread_with::<sym64::Sym>(0, le)?.into(), sym64::SIZEOF_SYM)
                }
            };
            Ok(sym)
        }
    }

    impl ctx::TryIntoCtx<Ctx> for Sym {
        type Error = crate::error::Error;
        #[inline]
        fn try_into_ctx(self, bytes: &mut [u8], Ctx {container, le}: Ctx) -> result::Result<usize, Self::Error> {
            match container {
                Container::Little => {
                    let sym: sym32::Sym = self.into();
                    Ok(bytes.pwrite_with(sym, 0, le)?)
                },
                Container::Big => {
                    let sym: sym64::Sym = self.into();
                    Ok(bytes.pwrite_with(sym, 0, le)?)
                }
            }
        }
    }

    impl ctx::IntoCtx<Ctx> for Sym {
        #[inline]
        fn into_ctx(self, bytes: &mut [u8], Ctx {container, le}: Ctx) {
            match container {
                Container::Little => {
                    let sym: sym32::Sym = self.into();
                    bytes.pwrite_with(sym, 0, le).unwrap();
                },
                Container::Big => {
                    let sym: sym64::Sym = self.into();
                    bytes.pwrite_with(sym, 0, le).unwrap();
                }
            }
        }
    }
}

if_alloc! {
    #[derive(Default, Clone)]
    /// An ELF symbol table, allowing lazy iteration over symbols
    pub struct Symtab<'a> {
        bytes: &'a [u8],
        count: usize,
        ctx: Ctx,
        start: usize,
        end: usize,
        /// Optional `SHT_SYMTAB_SHNDX` extension table, one 32-bit entry per
        /// symbol. When present, symbols whose raw `st_shndx` is
        /// `SHN_XINDEX` take their section index from this slice.
        extended: Option<&'a [u8]>,
    }

    impl<'a> fmt::Debug for Symtab<'a> {
        fn fmt(&self, fmt: &mut fmt::Formatter) -> fmt::Result {
            let len = self.bytes.len();
            fmt.debug_struct("Symtab")
                .field("bytes", &len)
                .field("range", &format_args!("{:#x}..{:#x}", self.start, self.end))
                .field("count", &self.count)
                .field("extended_section_indices", &self.extended.is_some())
                .field("Symbols", &self.to_vec())
                .finish()
        }
    }

    impl<'a> Symtab<'a> {
        /// Parse a table of `count` ELF symbols from `offset`.
        ///
        /// The returned table has no attached `SHT_SYMTAB_SHNDX` extension
        /// table, so `SHN_XINDEX` values are left untouched in `st_shndx`.
        /// Extension tables are attached internally when parsing through
        /// [`crate::elf::Elf`].
        pub fn parse(bytes: &'a [u8], offset: usize, count: usize, ctx: Ctx) -> Result<Symtab<'a>> {
            Self::parse_with_opts(bytes, offset, count, ctx, &crate::options::ParseOptions::default())
        }

        /// Parse a table of `count` ELF symbols from `offset` with options.
        pub(crate) fn parse_with_opts(bytes: &'a [u8], offset: usize, count: usize, ctx: Ctx, opts: &crate::options::ParseOptions) -> Result<Symtab<'a>> {

            // Validate offset is within bounds
            if offset >= bytes.len() {
                return Err(crate::error::Error::Malformed(
                    format!("Symbol table offset ({}) is beyond file boundary ({})", offset, bytes.len())
                )).or_permissive_and_value(
                    opts.parse_mode.is_permissive(),
                    "Symbol table offset is beyond file boundary, returning empty symbol table",
                    Symtab { bytes: &[], count: 0, ctx, start: offset, end: offset, extended: None }
                );
            }

            // Check for extremely large counts
            let mut actual_count = if count > usize::MAX {
                Err(crate::error::Error::Malformed(
                    format!("Symbol count ({}) exceeds maximum possible value", count)
                ))
                .or_permissive_and_then(
                    opts.parse_mode.is_permissive(),
                    "Symbol count exceeds maximum; truncating",
                    || usize::MAX,
                )?
            } else {
                count
            };

            let mut requested_size = actual_count
                .checked_mul(Sym::size_with(&ctx))
                .ok_or_else(|| crate::error::Error::Malformed(
                    format!("Too many ELF symbols (offset {:#x}, count {})", offset, actual_count)
                ))?;

            // Check if the requested size extends beyond the file
            let available_bytes = bytes.len().saturating_sub(offset);
            if requested_size > available_bytes {
                let sym_size = Sym::size_with(&ctx);
                let adjusted_count = if sym_size > 0 { available_bytes / sym_size } else { 0 };

            let (new_size, new_count) = Err(crate::error::Error::Malformed(
                format!("Symbol table extends beyond file boundary (requested: {}, available: {})",
                        requested_size, available_bytes)
            ))
                .or_permissive_and_then(
                    opts.parse_mode.is_permissive(),
                    "Symbol table extends beyond file; truncating",
                    || (available_bytes, adjusted_count),
                )?;

                requested_size = new_size;
                actual_count = new_count;
            }

            bytes.pread_with(offset, requested_size)
                .map_err(Into::into)
                .map(|symbol_bytes| Symtab {
                    bytes: symbol_bytes,
                    count: actual_count,
                    ctx,
                    start: offset,
                    end: offset + requested_size,
                    extended: None,
                })
                .or_permissive_and_value(
                    opts.parse_mode.is_permissive(),
                    "Failed to read symbol table data",
                    Symtab { bytes: &[], count: 0, ctx, start: offset, end: offset, extended: None },
                )
        }

        /// Attach the bytes of an `SHT_SYMTAB_SHNDX` section to this symbol table.
        ///
        /// `extended` must hold exactly one 32-bit entry per symbol; the
        /// caller ([`Elf`](crate::elf::Elf)) is responsible for validating the
        /// section's size, uniqueness and `sh_link` before attaching. After
        /// attaching, [`Symtab::get`], [`Symtab::iter`] and
        /// [`Symtab::to_vec`] resolve `SHN_XINDEX` symbols through this table.
        pub(crate) fn with_extended_sections(&mut self, extended: &'a [u8]) {
            self.extended = Some(extended);
        }

        /// Whether an `SHT_SYMTAB_SHNDX` extension table is attached.
        #[inline]
        pub fn has_extended_sections(&self) -> bool {
            self.extended.is_some()
        }

        /// The raw bytes of the attached `SHT_SYMTAB_SHNDX` section, if any.
        #[inline]
        pub(crate) fn extended_sections_bytes(&self) -> Option<&'a [u8]> {
            self.extended
        }

        /// Read the raw 16-bit `st_shndx` encoding of the symbol at `index`.
        ///
        /// Returns `None` if the symbol table has no symbol at `index`.
        #[inline]
        pub fn raw_shndx(&self, index: usize) -> Option<u16> {
            if index >= self.count {
                return None;
            }
            let sym_size = Sym::size_with(&self.ctx);
            // st_shndx is the last field of the 32-bit layout (offset 14);
            // in the 64-bit layout it follows st_info/st_other (offset 6).
            let field_offset = match self.ctx.container {
                Container::Little => sym32::SIZEOF_SYM - 2,
                Container::Big => 6,
            };
            self.bytes
                .pread_with::<u16>(index * sym_size + field_offset, self.ctx.le)
                .ok()
        }

        /// Resolve the defining section index of the symbol at `index`.
        ///
        /// A raw `SHN_XINDEX` value is replaced by the corresponding entry in
        /// the attached extension table. Without an attachment, or when the
        /// raw value is not `SHN_XINDEX`, the symbol's own `st_shndx` is
        /// returned unchanged.
        #[inline]
        pub fn resolved_shndx(&self, index: usize) -> Option<usize> {
            let sym = self.get(index)?;
            let raw = self.raw_shndx(index)?;
            if u32::from(raw) == crate::elf::section_header::SHN_XINDEX {
                if let Some(extended) = self.extended {
                    return extended
                        .pread_with::<u32>(index * 4, self.ctx.le)
                        .ok()
                        .map(|value| value as usize);
                }
            }
            Some(sym.st_shndx)
        }

        /// Classify the defining section of the symbol at `index`.
        ///
        /// Unlike [`Sym::symbol_section`], this keeps the original 16-bit
        /// encoding around when an extension table resolves an `SHN_XINDEX`
        /// entry, so a large index is reported as `Section` even when its
        /// numeric value lands inside the reserved range. Without an
        /// attachment, an unresolved `SHN_XINDEX` stays `Reserved`.
        #[inline]
        pub fn section_kind(&self, index: usize) -> Option<SymbolSection> {
            let raw = self.raw_shndx(index)?;
            if u32::from(raw) == crate::elf::section_header::SHN_XINDEX {
                return match self.resolved_shndx(index)? {
                    // resolved_shndx only diverges from the raw value when an
                    // extension table is attached; distinguish the two cases.
                    resolved if self.extended.is_some() => Some(SymbolSection::Section(resolved)),
                    _ => Some(SymbolSection::Reserved(raw)),
                };
            }
            let resolved = self.resolved_shndx(index)?;
            Some(classify_symbol_section(raw, resolved))
        }

        /// Try to parse a single symbol from the binary, at `index`.
        ///
        /// If an `SHT_SYMTAB_SHNDX` extension table is attached and the
        /// symbol's raw `st_shndx` is `SHN_XINDEX`, the returned symbol's
        /// `st_shndx` carries the extended section index.
        #[inline]
        pub fn get(&self, index: usize) -> Option<Sym> {
            if index >= self.count {
                None
            } else {
                let mut sym: Sym = self.bytes.pread_with(index * Sym::size_with(&self.ctx), self.ctx).unwrap();
                if let Some(resolved) = self.extension_for(index, sym.st_shndx) {
                    sym.st_shndx = resolved;
                }
                Some(sym)
            }
        }

        /// Look up the extension-table value for `index` when the symbol's raw
        /// section index is `SHN_XINDEX`; `st_shndx` here still holds the raw
        /// 16-bit value because the extension has not been applied yet.
        #[inline]
        fn extension_for(&self, index: usize, raw_shndx: usize) -> Option<usize> {
            if raw_shndx as u32 != crate::elf::section_header::SHN_XINDEX {
                return None;
            }
            let extended = self.extended?;
            extended
                .pread_with::<u32>(index * 4, self.ctx.le)
                .ok()
                .map(|value| value as usize)
        }

        /// The number of symbols in the table.
        #[inline]
        pub fn len(&self) -> usize {
            self.count
        }

        /// The offset of symbol table in elf
        #[inline]
        pub fn offset(&self) -> usize {
            self.start
        }

        /// The ctx of symbol table
        #[inline]
        pub fn ctx(&self) -> &Ctx {
            &self.ctx
        }

        /// Returns true if table has no symbols.
        #[inline]
        pub fn is_empty(&self) -> bool {
            self.count == 0
        }

        /// Iterate over all symbols.
        #[inline]
        pub fn iter(&self) -> SymIterator<'a> {
            self.into_iter()
        }

        /// Parse all symbols into a vector.
        pub fn to_vec(&self) -> Vec<Sym> {
            self.iter().collect()
        }
    }

    impl<'a, 'b> IntoIterator for &'b Symtab<'a> {
        type Item = <SymIterator<'a> as Iterator>::Item;
        type IntoIter = SymIterator<'a>;

        #[inline]
        fn into_iter(self) -> Self::IntoIter {
            SymIterator {
                bytes: self.bytes,
                offset: 0,
                index: 0,
                count: self.count,
                ctx: self.ctx,
                extended: self.extended,
            }
        }
    }

    /// An iterator over symbols in an ELF symbol table
    pub struct SymIterator<'a> {
        bytes: &'a [u8],
        offset: usize,
        index: usize,
        count: usize,
        ctx: Ctx,
        extended: Option<&'a [u8]>,
    }

    impl<'a> Iterator for SymIterator<'a> {
        type Item = Sym;

        #[inline]
        fn next(&mut self) -> Option<Self::Item> {
            if self.index >= self.count {
                None
            } else {
                let index = self.index;
                self.index += 1;
                let mut sym: Sym = self.bytes.gread_with(&mut self.offset, self.ctx).unwrap();
                if sym.st_shndx as u32 == crate::elf::section_header::SHN_XINDEX {
                    if let Some(extended) = self.extended {
                        if let Ok(value) = extended.pread_with::<u32>(index * 4, self.ctx.le) {
                            sym.st_shndx = value as usize;
                        }
                    }
                }
                Some(sym)
            }
        }
    }

    impl<'a> ExactSizeIterator for SymIterator<'a> {
        #[inline]
        fn len(&self) -> usize {
            self.count - self.index
        }
    }
} // end if_alloc
