use core::{fmt, slice};

use crate::endian::Endianness;
use crate::macho;
use crate::read::{
    ReadRef, Relocation, RelocationEncoding, RelocationFlags, RelocationKind, RelocationTarget,
    SectionIndex, SymbolIndex,
};

use super::{MachHeader, MachOFile};

/// An iterator for the relocations in a [`MachOSection32`](super::MachOSection32).
pub type MachORelocationIterator32<'data, 'file, Endian = Endianness, R = &'data [u8]> =
    MachORelocationIterator<'data, 'file, macho::MachHeader32<Endian>, R>;
/// An iterator for the relocations in a [`MachOSection64`](super::MachOSection64).
pub type MachORelocationIterator64<'data, 'file, Endian = Endianness, R = &'data [u8]> =
    MachORelocationIterator<'data, 'file, macho::MachHeader64<Endian>, R>;

/// An iterator for the relocations in a [`MachOSection`](super::MachOSection).
pub struct MachORelocationIterator<'data, 'file, Mach, R = &'data [u8]>
where
    Mach: MachHeader,
    R: ReadRef<'data>,
{
    pub(super) file: &'file MachOFile<'data, Mach, R>,
    pub(super) relocations: slice::Iter<'data, macho::Relocation<Mach::Endian>>,
}

impl<'data, 'file, Mach, R> Iterator for MachORelocationIterator<'data, 'file, Mach, R>
where
    Mach: MachHeader,
    R: ReadRef<'data>,
{
    type Item = (u64, Relocation);

    fn next(&mut self) -> Option<Self::Item> {
        use RelocationEncoding as E;
        use RelocationKind as K;

        let mut paired_addend = 0;
        let mut subtractor = None;
        loop {
            let reloc = self.relocations.next()?;
            let endian = self.file.endian;
            let cputype = self.file.header.cputype(endian);
            if reloc.r_scattered(endian, cputype) {
                // Scattered support is limited to little-endian 32-bit objects.
                // Other cases keep the previous behaviour of skipping the record.
                use crate::endian::Endian;
                if endian.is_little_endian() && !self.file.header.is_type_64() {
                    if let Some(item) = self.next_scattered(reloc) {
                        return Some(item);
                    }
                }
                continue;
            }
            let reloc = reloc.info(self.file.endian);
            let flags = RelocationFlags::MachO {
                r_type: reloc.r_type,
                r_pcrel: reloc.r_pcrel,
                r_length: reloc.r_length,
            };
            let mut size = 8 << reloc.r_length;
            let g = E::Generic;
            let unknown = (K::Unknown, E::Unknown);
            let (kind, encoding) = match cputype {
                macho::CPU_TYPE_ARM => match (reloc.r_type, reloc.r_pcrel) {
                    (macho::ARM_RELOC_VANILLA, false) => (K::Absolute, g),
                    _ => unknown,
                },
                macho::CPU_TYPE_ARM64 | macho::CPU_TYPE_ARM64_32 => {
                    match (reloc.r_type, reloc.r_pcrel) {
                        (macho::ARM64_RELOC_UNSIGNED, false) => (K::Absolute, g),
                        (macho::ARM64_RELOC_BRANCH26, true) => {
                            size = 26;
                            (K::PltRelative, E::AArch64Call)
                        }
                        (macho::ARM64_RELOC_POINTER_TO_GOT, true) => (K::GotRelative, g),
                        (macho::ARM64_RELOC_ADDEND, _) => {
                            paired_addend = i64::from(reloc.r_symbolnum)
                                .wrapping_shl(64 - 24)
                                .wrapping_shr(64 - 24);
                            continue;
                        }
                        (macho::ARM64_RELOC_SUBTRACTOR, _) => {
                            subtractor = Some(SymbolIndex(reloc.r_symbolnum as usize));
                            continue;
                        }
                        _ => unknown,
                    }
                }
                macho::CPU_TYPE_X86 => match (reloc.r_type, reloc.r_pcrel) {
                    (macho::GENERIC_RELOC_VANILLA, false) => (K::Absolute, g),
                    _ => unknown,
                },
                macho::CPU_TYPE_X86_64 => match (reloc.r_type, reloc.r_pcrel) {
                    (macho::X86_64_RELOC_UNSIGNED, false) => (K::Absolute, g),
                    (macho::X86_64_RELOC_SIGNED, true) => (K::Relative, E::X86RipRelative),
                    (macho::X86_64_RELOC_BRANCH, true) => (K::PltRelative, E::X86Branch),
                    (macho::X86_64_RELOC_GOT, true) => (K::GotRelative, g),
                    (macho::X86_64_RELOC_GOT_LOAD, true) => (K::GotRelative, E::X86RipRelativeMovq),
                    (macho::X86_64_RELOC_SUBTRACTOR, _) => {
                        subtractor = Some(SymbolIndex(reloc.r_symbolnum as usize));
                        continue;
                    }
                    _ => unknown,
                },
                macho::CPU_TYPE_POWERPC | macho::CPU_TYPE_POWERPC64 => {
                    match (reloc.r_type, reloc.r_pcrel) {
                        (macho::PPC_RELOC_VANILLA, false) => (K::Absolute, g),
                        _ => unknown,
                    }
                }
                _ => unknown,
            };
            let target = if reloc.r_extern {
                RelocationTarget::Symbol(SymbolIndex(reloc.r_symbolnum as usize))
            } else if reloc.r_symbolnum == macho::R_ABS {
                RelocationTarget::Absolute
            } else {
                RelocationTarget::Section(SectionIndex(reloc.r_symbolnum as usize))
            };
            let implicit_addend = paired_addend == 0;
            let mut addend = paired_addend;
            if reloc.r_pcrel {
                // For PC relative relocations on some architectures, the
                // addend does not include the offset required due to the
                // PC being different from the place of the relocation.
                // This differs from other file formats, so adjust the
                // addend here to account for this.
                match cputype {
                    macho::CPU_TYPE_X86 => {
                        addend -= 1 << reloc.r_length;
                    }
                    macho::CPU_TYPE_X86_64 => {
                        addend -= 1 << reloc.r_length;
                        match reloc.r_type {
                            macho::X86_64_RELOC_SIGNED_1 => addend -= 1,
                            macho::X86_64_RELOC_SIGNED_2 => addend -= 2,
                            macho::X86_64_RELOC_SIGNED_4 => addend -= 4,
                            _ => {}
                        }
                    }
                    // TODO: maybe missing support for some architectures and relocations
                    _ => {}
                }
            }
            return Some((
                reloc.r_address as u64,
                Relocation {
                    kind,
                    encoding,
                    size,
                    target,
                    subtractor,
                    subtractor_target: None,
                    addend,
                    implicit_addend,
                    flags,
                },
            ));
        }
    }
}

impl<'data, 'file, Mach, R> MachORelocationIterator<'data, 'file, Mach, R>
where
    Mach: MachHeader,
    R: ReadRef<'data>,
{
    /// Parse a scattered relocation record.
    ///
    /// Returns `None` for records that are not supported, so that the caller
    /// continues with the following record. A `GENERIC_RELOC_SECTDIFF` or
    /// `GENERIC_RELOC_LOCAL_SECTDIFF` record and its immediately following
    /// `GENERIC_RELOC_PAIR` are combined into a single relocation.
    fn next_scattered(
        &mut self,
        reloc: &'data macho::Relocation<Mach::Endian>,
    ) -> Option<(u64, Relocation)> {
        let endian = self.file.endian;
        let info = reloc.scattered_info(endian);
        let flags = RelocationFlags::MachO {
            r_type: info.r_type,
            r_pcrel: info.r_pcrel,
            r_length: info.r_length,
        };
        let size = match info.r_length {
            0 => 8,
            1 => 16,
            2 => 32,
            _ => return None,
        };
        let kind = if info.r_pcrel {
            RelocationKind::Relative
        } else {
            RelocationKind::Absolute
        };
        match info.r_type {
            macho::GENERIC_RELOC_VANILLA => Some(self.scattered_relocation(
                info.r_address,
                size,
                kind,
                flags,
                RelocationTarget::Address(u64::from(info.r_value)),
                None,
            )),
            macho::GENERIC_RELOC_SECTDIFF | macho::GENERIC_RELOC_LOCAL_SECTDIFF => {
                // Difference relocations are only supported without PC-relativity.
                if info.r_pcrel {
                    return None;
                }
                // Peek at the following record, which must be the PAIR. It is
                // only consumed once confirmed, so a malformed file still
                // surfaces the records that come after it.
                let pair = *self.relocations.as_slice().first()?;
                if !pair.r_scattered(endian, self.file.header.cputype(endian)) {
                    return None;
                }
                let pair_info = pair.scattered_info(endian);
                if pair_info.r_type != macho::GENERIC_RELOC_PAIR {
                    return None;
                }
                self.relocations.next();
                // The PAIR's r_address is not another field to relocate; only
                // its r_value identifies the subtracted old address.
                Some(self.scattered_relocation(
                    info.r_address,
                    size,
                    RelocationKind::Absolute,
                    flags,
                    RelocationTarget::Address(u64::from(info.r_value)),
                    Some(RelocationTarget::Address(u64::from(pair_info.r_value))),
                ))
            }
            _ => None,
        }
    }

    /// Construct a relocation for a scattered record.
    ///
    /// The field already contains the old address, or the difference of the two
    /// old addresses, so the addend is implicit and zero. The `r_value` is kept
    /// as the identity of the [`RelocationTarget::Address`] target.
    fn scattered_relocation(
        &self,
        r_address: u32,
        size: u8,
        kind: RelocationKind,
        flags: RelocationFlags,
        target: RelocationTarget,
        subtractor_target: Option<RelocationTarget>,
    ) -> (u64, Relocation) {
        (
            u64::from(r_address),
            Relocation {
                kind,
                encoding: RelocationEncoding::Generic,
                size,
                target,
                subtractor: None,
                subtractor_target,
                addend: 0,
                implicit_addend: true,
                flags,
            },
        )
    }
}

impl<'data, 'file, Mach, R> fmt::Debug for MachORelocationIterator<'data, 'file, Mach, R>
where
    Mach: MachHeader,
    R: ReadRef<'data>,
{
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("MachORelocationIterator").finish()
    }
}
