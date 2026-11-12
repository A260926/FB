/*
Copyright (C) 2001-present by Serge Lamikhov-Center

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef ELFIO_GNU_HASH_REBUILD_HPP
#define ELFIO_GNU_HASH_REBUILD_HPP

#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace ELFIO {

//------------------------------------------------------------------------------
//! \struct gnu_hash_rebuild_params
//! \brief Layout parameters for a rebuilt SHT_GNU_HASH section.
//------------------------------------------------------------------------------
struct gnu_hash_rebuild_params
{
    Elf_Word symoffset   = 0; //!< First symbol table index covered by the hash
    Elf_Word nbuckets    = 0; //!< Number of hash buckets
    Elf_Word bloom_size  = 0; //!< Number of words in the Bloom filter
    Elf_Word bloom_shift = 0; //!< Bloom filter shift count
};

//------------------------------------------------------------------------------
//! \struct gnu_hash_rebuild_permutation
//! \brief Symbol index permutation applied by a successful rebuild.
//------------------------------------------------------------------------------
struct gnu_hash_rebuild_permutation
{
    std::vector<Elf_Word>
        old_to_new; //!< old_to_new[i]: new index of the symbol formerly at i
    std::vector<Elf_Word>
        new_to_old; //!< new_to_old[i]: former index of the symbol now at i
};

//------------------------------------------------------------------------------
//! \class gnu_hash_rebuilder
//! \brief Rebuilds the content of a SHT_GNU_HASH section from the SHT_DYNSYM
//! section it is linked to.
//!
//! The rebuild reorders the hashed part of the dynamic symbol table into the
//! bucket order mandated by the GNU hash format and rewrites every piece of
//! section metadata that refers to symbol indices: REL/RELA r_info fields,
//! SHT_GNU_versym halfwords, SHT_SYMTAB_SHNDX words and SHT_GROUP signature
//! indices. Only section metadata is rewritten; program segments are left
//! alone and no attempt is made to turn an arbitrary shared library into a
//! loadable image. The previous content of the hash section is never
//! consulted - it may be corrupt - everything is recomputed from the symbol
//! names. All checks run before any modification: when an error is reported
//! the elfio object is left untouched.
//------------------------------------------------------------------------------
class gnu_hash_rebuilder
{
  public:
    //------------------------------------------------------------------------------
    //! \brief Constructor
    //! \param elf_file The ELF file the sections belong to
    //------------------------------------------------------------------------------
    explicit gnu_hash_rebuilder( elfio& elf_file ) : elf_file( elf_file ) {}

    //------------------------------------------------------------------------------
    //! \brief Rebuild the GNU hash section linked to a dynamic symbol table
    //! \param dynsym_section The target SHT_DYNSYM section
    //! \param gnu_hash_section The SHT_GNU_HASH section linked to it
    //! \param params Layout parameters for the new hash content
    //! \param permutation Receives the applied old-to-new / new-to-old mapping
    //! \param error Receives a diagnostic when the rebuild is refused
    //! \return True on success; on failure nothing has been modified
    //------------------------------------------------------------------------------
    bool rebuild( section*                        dynsym_section,
                  section*                        gnu_hash_section,
                  const gnu_hash_rebuild_params&  params,
                  gnu_hash_rebuild_permutation&   permutation,
                  std::string&                    error )
    {
        error.clear();
        permutation.old_to_new.clear();
        permutation.new_to_old.clear();

        const endianness_convertor& conv = *elf_file.get_convertor();
        const bool                  is64 = elf_file.get_class() == ELFCLASS64;

        // ------------------------------------------------------------------
        // Target sections
        // ------------------------------------------------------------------
        if ( dynsym_section == nullptr || gnu_hash_section == nullptr ) {
            return fail( error, "null section pointer" );
        }
        if ( dynsym_section->get_type() != SHT_DYNSYM ) {
            return fail( error, "target section is not SHT_DYNSYM" );
        }
        if ( gnu_hash_section->get_type() != SHT_GNU_HASH ) {
            return fail( error, "hash section is not SHT_GNU_HASH" );
        }
        const unsigned sec_count = elf_file.sections.size();
        if ( dynsym_section->get_index() >= sec_count ||
             elf_file.sections[dynsym_section->get_index()] !=
                 dynsym_section ) {
            return fail( error,
                         "dynsym section does not belong to this file" );
        }
        if ( gnu_hash_section->get_index() >= sec_count ||
             elf_file.sections[gnu_hash_section->get_index()] !=
                 gnu_hash_section ) {
            return fail( error,
                         "gnu hash section does not belong to this file" );
        }
        if ( gnu_hash_section->get_link() != dynsym_section->get_index() ) {
            return fail( error,
                         "gnu hash section is not linked to the dynsym" );
        }

        // ------------------------------------------------------------------
        // Layout parameters
        // ------------------------------------------------------------------
        if ( params.nbuckets == 0 ) {
            return fail( error, "nbuckets must be non-zero" );
        }
        if ( params.bloom_size == 0 ) {
            return fail( error, "bloom word count must be non-zero" );
        }
        if ( params.bloom_shift >= 32 ) {
            return fail( error, "bloom shift is out of range" );
        }

        // ------------------------------------------------------------------
        // Symbol table shape
        // ------------------------------------------------------------------
        const Elf_Xword sym_entsize = dynsym_section->get_entry_size();
        const Elf_Xword sym_minsize =
            is64 ? sizeof( Elf64_Sym ) : sizeof( Elf32_Sym );
        if ( sym_entsize < sym_minsize ) {
            return fail( error, "dynsym entry size is too small" );
        }
        if ( dynsym_section->get_size() % sym_entsize != 0 ) {
            return fail( error,
                         "dynsym size is not a multiple of its entry size" );
        }
        const Elf_Xword nsyms64 = dynsym_section->get_size() / sym_entsize;
        if ( nsyms64 > std::numeric_limits<Elf_Word>::max() ) {
            return fail( error, "symbol count is not representable" );
        }
        const Elf_Word nsyms     = (Elf_Word)nsyms64;
        const char*    sym_data  = dynsym_section->get_data();
        if ( nsyms != 0 && sym_data == nullptr ) {
            return fail( error, "dynsym data is not available" );
        }

        // ------------------------------------------------------------------
        // sh_info, local prefix and symoffset
        // ------------------------------------------------------------------
        const Elf_Word sh_info = dynsym_section->get_info();
        if ( sh_info > nsyms ) {
            return fail( error, "dynsym sh_info exceeds the symbol count" );
        }
        if ( nsyms != 0 && sh_info == 0 ) {
            return fail( error,
                         "dynsym sh_info does not cover the null symbol" );
        }
        if ( params.symoffset > nsyms ) {
            return fail( error, "symoffset exceeds the symbol count" );
        }
        if ( params.symoffset < sh_info ) {
            return fail( error,
                         "symoffset cuts into the local symbol prefix" );
        }

        // ------------------------------------------------------------------
        // String table and symbol names
        // ------------------------------------------------------------------
        if ( dynsym_section->get_link() >= sec_count ) {
            return fail( error, "dynsym string table link is out of range" );
        }
        const section* strtab = elf_file.sections[dynsym_section->get_link()];
        if ( strtab->get_type() != SHT_STRTAB ) {
            return fail( error, "dynsym link does not denote a string table" );
        }
        const char*    str_data = strtab->get_data();
        const Elf_Xword str_size = strtab->get_size();
        if ( str_size != 0 && str_data == nullptr ) {
            return fail( error, "string table data is not available" );
        }

        const size_t st_name_off = 0;             // same in Elf32_Sym/Elf64_Sym
        const size_t st_info_off = is64 ? 4 : 12; // st_info offset

        std::vector<std::uint32_t> hashes; // one per symbol >= symoffset
        hashes.reserve( nsyms - params.symoffset );

        for ( Elf_Word i = 0; i < nsyms; ++i ) {
            const char* entry = sym_data + (Elf_Xword)i * sym_entsize;
            Elf_Word    st_name;
            std::memcpy( &st_name, entry + st_name_off, sizeof( st_name ) );
            st_name = conv( st_name );
            unsigned char st_info;
            std::memcpy( &st_info, entry + st_info_off, sizeof( st_info ) );

            if ( i < sh_info && ELF_ST_BIND( st_info ) != STB_LOCAL ) {
                return fail( error,
                             "non-local symbol inside the local prefix" );
            }
            if ( i >= sh_info && ELF_ST_BIND( st_info ) == STB_LOCAL ) {
                return fail( error,
                             "local symbol outside the local prefix" );
            }
            if ( st_name >= str_size ) {
                return fail( error,
                             "symbol name offset is outside the string table" );
            }
            if ( std::memchr( str_data + st_name, '\0',
                              str_size - st_name ) == nullptr ) {
                return fail( error, "symbol name is not null-terminated" );
            }
            if ( i >= params.symoffset ) {
                hashes.push_back( elf_gnu_hash(
                    (const unsigned char*)( str_data + st_name ) ) );
            }
        }

        // ------------------------------------------------------------------
        // Capacity of the existing hash section (checked arithmetic)
        // ------------------------------------------------------------------
        const Elf_Xword nchains    = nsyms - params.symoffset;
        const Elf_Xword word_bytes = is64 ? 8 : 4;
        Elf_Xword       need       = 16;
        Elf_Xword       term       = 0;
        if ( !checked_mul( params.bloom_size, word_bytes, term ) ||
             !checked_add( need, term, need ) ||
             !checked_mul( params.nbuckets, (Elf_Xword)4, term ) ||
             !checked_add( need, term, need ) ||
             !checked_mul( nchains, (Elf_Xword)4, term ) ||
             !checked_add( need, term, need ) ) {
            return fail( error, "gnu hash layout overflows" );
        }
        if ( need > gnu_hash_section->get_size() ) {
            return fail( error,
                         "gnu hash section capacity is too small" );
        }

        // ------------------------------------------------------------------
        // Attachments: collect and validate in a single scan
        // ------------------------------------------------------------------
        struct reloc_ref
        {
            section* sec;
            bool     is_rela;
        };
        std::vector<reloc_ref> relocs;
        std::vector<section*>  versyms;
        std::vector<section*>  shndxs;
        std::vector<section*>  groups;

        for ( unsigned i = 0; i < sec_count; ++i ) {
            section* sec = elf_file.sections[i];
            if ( sec->get_link() != dynsym_section->get_index() ) {
                continue;
            }
            switch ( sec->get_type() ) {
            case SHT_REL:
                relocs.push_back( { sec, false } );
                break;
            case SHT_RELA:
                relocs.push_back( { sec, true } );
                break;
            case SHT_GNU_versym:
                versyms.push_back( sec );
                break;
            case SHT_SYMTAB_SHNDX:
                shndxs.push_back( sec );
                break;
            case SHT_GROUP:
                groups.push_back( sec );
                break;
            case SHT_GNU_HASH:
                if ( sec != gnu_hash_section ) {
                    return fail( error,
                                 "another SHT_GNU_HASH section is linked to "
                                 "the target dynsym" );
                }
                break;
            case SHT_HASH:
                return fail( error,
                             "a SHT_HASH section is linked to the target "
                             "dynsym; refusing to leave a stale index" );
            default:
                return fail( error,
                             "unsupported section type is linked to the "
                             "target dynsym" );
            }
        }

        // Relocation sections: entry shape and referenced symbol indices
        const Elf_Xword rel_min  = is64 ? sizeof( Elf64_Rel )
                                        : sizeof( Elf32_Rel );
        const Elf_Xword rela_min = is64 ? sizeof( Elf64_Rela )
                                        : sizeof( Elf32_Rela );
        if ( !is64 && !relocs.empty() && nsyms > 0x1000000 ) {
            return fail( error,
                         "symbol index does not fit into ELF32 r_info" );
        }
        for ( const reloc_ref& r : relocs ) {
            const Elf_Xword minent = r.is_rela ? rela_min : rel_min;
            const Elf_Xword entsz  = r.sec->get_entry_size();
            const Elf_Xword sz     = r.sec->get_size();
            if ( sz == 0 ) {
                continue;
            }
            if ( entsz < minent || sz % entsz != 0 ) {
                return fail( error,
                             "relocation section has incomplete entries" );
            }
            const char* data = r.sec->get_data();
            if ( data == nullptr ) {
                return fail( error,
                             "relocation section data is not available" );
            }
            const Elf_Xword n = sz / entsz;
            for ( Elf_Xword e = 0; e < n; ++e ) {
                const char* entry = data + e * entsz;
                Elf_Xword   sym;
                if ( is64 ) {
                    std::uint64_t info;
                    std::memcpy( &info, entry + 8, sizeof( info ) );
                    info = conv( info );
                    sym  = info >> 32;
                }
                else {
                    std::uint32_t info;
                    std::memcpy( &info, entry + 4, sizeof( info ) );
                    info = conv( info );
                    sym  = info >> 8;
                }
                if ( sym >= nsyms ) {
                    return fail(
                        error,
                        "relocation references a symbol index out of range" );
                }
            }
        }

        // Versym: one halfword per symbol
        for ( const section* sec : versyms ) {
            const Elf_Xword sz = sec->get_size();
            if ( sz % 2 != 0 || sz / 2 != nsyms ) {
                return fail( error,
                             "versym section length does not match the "
                             "symbol count" );
            }
            if ( sz != 0 && sec->get_data() == nullptr ) {
                return fail( error, "versym section data is not available" );
            }
        }

        // Symtab shndx: one word per symbol
        for ( const section* sec : shndxs ) {
            const Elf_Xword sz = sec->get_size();
            if ( sz % 4 != 0 || sz / 4 != nsyms ) {
                return fail( error,
                             "symtab shndx section length does not match "
                             "the symbol count" );
            }
            if ( sz != 0 && sec->get_data() == nullptr ) {
                return fail( error, "shndx section data is not available" );
            }
        }

        // Groups: signature symbol index and member section indices
        for ( const section* sec : groups ) {
            const Elf_Xword sz = sec->get_size();
            if ( sz < 4 || sz % 4 != 0 ) {
                return fail( error, "group section has incomplete entries" );
            }
            const char* data = sec->get_data();
            if ( data == nullptr ) {
                return fail( error, "group section data is not available" );
            }
            if ( sec->get_info() >= nsyms ) {
                return fail( error,
                             "group signature symbol index is out of range" );
            }
            for ( Elf_Xword k = 1; k < sz / 4; ++k ) {
                Elf_Word member;
                std::memcpy( &member, data + k * 4, sizeof( member ) );
                member = conv( member );
                if ( member >= sec_count ) {
                    return fail(
                        error,
                        "group member section index is out of range" );
                }
            }
        }

        // ==================================================================
        // All checks passed; from here on nothing can fail the contract.
        // ==================================================================

        // ------------------------------------------------------------------
        // Permutation: stable grouping of hashed symbols by bucket number
        // ------------------------------------------------------------------
        const Elf_Word symoffset = params.symoffset;
        permutation.new_to_old.resize( nsyms );
        permutation.old_to_new.resize( nsyms );
        for ( Elf_Word i = 0; i < symoffset; ++i ) {
            permutation.new_to_old[i] = i;
        }

        std::vector<Elf_Word> bucket_count( params.nbuckets, 0 );
        for ( Elf_Word j = 0; j < nchains; ++j ) {
            ++bucket_count[hashes[j] % params.nbuckets];
        }
        std::vector<Elf_Word> bucket_start( params.nbuckets, 0 );
        {
            Elf_Word pos = symoffset;
            for ( Elf_Word b = 0; b < params.nbuckets; ++b ) {
                bucket_start[b] = pos;
                pos += bucket_count[b];
            }
        }
        {
            std::vector<Elf_Word> cursor = bucket_start;
            for ( Elf_Word j = 0; j < nchains; ++j ) {
                const Elf_Word b = hashes[j] % params.nbuckets;
                permutation.new_to_old[cursor[b]++] = symoffset + j;
            }
        }
        for ( Elf_Word p = 0; p < nsyms; ++p ) {
            permutation.old_to_new[permutation.new_to_old[p]] = p;
        }

        // ------------------------------------------------------------------
        // Dynamic symbol table: move whole entries to their new positions
        // ------------------------------------------------------------------
        {
            std::vector<char> payload( (size_t)dynsym_section->get_size() );
            for ( Elf_Word p = 0; p < nsyms; ++p ) {
                std::memcpy(
                    payload.data() + (Elf_Xword)p * sym_entsize,
                    sym_data +
                        (Elf_Xword)permutation.new_to_old[p] * sym_entsize,
                    sym_entsize );
            }
            dynsym_section->set_data( payload.data(), payload.size() );
        }

        // ------------------------------------------------------------------
        // Relocations: rewrite the symbol part of every r_info in one pass;
        // offsets, types and addends stay untouched
        // ------------------------------------------------------------------
        for ( const reloc_ref& r : relocs ) {
            const Elf_Xword sz = r.sec->get_size();
            if ( sz == 0 ) {
                continue;
            }
            const char*         old_data = r.sec->get_data();
            std::vector<char>   payload( old_data, old_data + sz );
            const Elf_Xword     entsz = r.sec->get_entry_size();
            const Elf_Xword     n     = sz / entsz;
            for ( Elf_Xword e = 0; e < n; ++e ) {
                char* entry = payload.data() + e * entsz;
                if ( is64 ) {
                    std::uint64_t info;
                    std::memcpy( &info, entry + 8, sizeof( info ) );
                    info = conv( info );
                    const Elf_Word sym_new =
                        permutation.old_to_new[(Elf_Word)( info >> 32 )];
                    info = ( (std::uint64_t)sym_new << 32 ) |
                           ( info & 0xFFFFFFFFu );
                    info = conv( info );
                    std::memcpy( entry + 8, &info, sizeof( info ) );
                }
                else {
                    std::uint32_t info;
                    std::memcpy( &info, entry + 4, sizeof( info ) );
                    info = conv( info );
                    const Elf_Word sym_new =
                        permutation.old_to_new[info >> 8];
                    info = ( sym_new << 8 ) | ( info & 0xFFu );
                    info = conv( info );
                    std::memcpy( entry + 4, &info, sizeof( info ) );
                }
            }
            r.sec->set_data( payload.data(), payload.size() );
        }

        // ------------------------------------------------------------------
        // Versym halfwords and shndx words: moved raw with the permutation,
        // values (including hidden bits) are not interpreted
        // ------------------------------------------------------------------
        for ( section* sec : versyms ) {
            const Elf_Xword sz = sec->get_size();
            if ( sz == 0 ) {
                continue;
            }
            const char*       old_data = sec->get_data();
            std::vector<char> payload( (size_t)sz );
            for ( Elf_Word p = 0; p < nsyms; ++p ) {
                std::memcpy( payload.data() + (Elf_Xword)p * 2,
                             old_data +
                                 (Elf_Xword)permutation.new_to_old[p] * 2,
                             2 );
            }
            sec->set_data( payload.data(), payload.size() );
        }
        for ( section* sec : shndxs ) {
            const Elf_Xword sz = sec->get_size();
            if ( sz == 0 ) {
                continue;
            }
            const char*       old_data = sec->get_data();
            std::vector<char> payload( (size_t)sz );
            for ( Elf_Word p = 0; p < nsyms; ++p ) {
                std::memcpy( payload.data() + (Elf_Xword)p * 4,
                             old_data +
                                 (Elf_Xword)permutation.new_to_old[p] * 4,
                             4 );
            }
            sec->set_data( payload.data(), payload.size() );
        }

        // ------------------------------------------------------------------
        // Groups: only the signature symbol index follows the permutation;
        // member section indices are not touched
        // ------------------------------------------------------------------
        for ( section* sec : groups ) {
            sec->set_info( permutation.old_to_new[sec->get_info()] );
        }

        // ------------------------------------------------------------------
        // The hash section itself: header, bloom filter, buckets and chains
        // generated from the symbol names; any spare capacity is zeroed and
        // the section keeps its size and address
        // ------------------------------------------------------------------
        {
            const Elf_Xword   capacity = gnu_hash_section->get_size();
            std::vector<char> payload( (size_t)capacity, 0 );
            char*             out = payload.data();

            auto put32 = [&]( std::uint32_t v, char* at ) {
                v = conv( v );
                std::memcpy( at, &v, sizeof( v ) );
            };

            put32( params.nbuckets, out );
            put32( symoffset, out + 4 );
            put32( params.bloom_size, out + 8 );
            put32( params.bloom_shift, out + 12 );

            char* bloom_at   = out + 16;
            char* buckets_at = bloom_at + (Elf_Xword)params.bloom_size * word_bytes;
            char* chains_at  = buckets_at + (Elf_Xword)params.nbuckets * 4;

            for ( Elf_Word j = 0; j < nchains; ++j ) {
                const std::uint32_t h = hashes[j];
                if ( is64 ) {
                    const Elf_Xword idx = ( h / 64 ) % params.bloom_size;
                    std::uint64_t   w;
                    std::memcpy( &w, bloom_at + idx * 8, sizeof( w ) );
                    w = conv( w );
                    w |= ( (std::uint64_t)1 << ( h % 64 ) ) |
                         ( (std::uint64_t)1
                           << ( ( h >> params.bloom_shift ) % 64 ) );
                    w = conv( w );
                    std::memcpy( bloom_at + idx * 8, &w, sizeof( w ) );
                }
                else {
                    const Elf_Xword idx = ( h / 32 ) % params.bloom_size;
                    std::uint32_t   w;
                    std::memcpy( &w, bloom_at + idx * 4, sizeof( w ) );
                    w = conv( w );
                    w |= ( (std::uint32_t)1 << ( h % 32 ) ) |
                         ( (std::uint32_t)1
                           << ( ( h >> params.bloom_shift ) % 32 ) );
                    w = conv( w );
                    std::memcpy( bloom_at + idx * 4, &w, sizeof( w ) );
                }
            }

            for ( Elf_Word b = 0; b < params.nbuckets; ++b ) {
                if ( bucket_count[b] != 0 ) {
                    put32( bucket_start[b], buckets_at + (Elf_Xword)b * 4 );
                }
            }

            for ( Elf_Word p = symoffset; p < nsyms; ++p ) {
                const std::uint32_t h =
                    hashes[permutation.new_to_old[p] - symoffset];
                bool last = p + 1 == nsyms;
                if ( !last ) {
                    const Elf_Word next_bucket =
                        hashes[permutation.new_to_old[p + 1] - symoffset] %
                        params.nbuckets;
                    last = next_bucket != h % params.nbuckets;
                }
                put32( ( h & ~1u ) | ( last ? 1u : 0u ),
                       chains_at + (Elf_Xword)( p - symoffset ) * 4 );
            }

            gnu_hash_section->set_data( payload.data(), payload.size() );
        }

        return true;
    }

  private:
    //------------------------------------------------------------------------------
    //! \brief Record an error message and report failure
    //! \param error Receives the message
    //! \param message The diagnostic text
    //! \return Always false
    //------------------------------------------------------------------------------
    static bool fail( std::string& error, const std::string& message )
    {
        error = message;
        return false;
    }

    //------------------------------------------------------------------------------
    //! \brief Overflow-checked addition
    //------------------------------------------------------------------------------
    static bool checked_add( Elf_Xword a, Elf_Xword b, Elf_Xword& out )
    {
        if ( b > std::numeric_limits<Elf_Xword>::max() - a ) {
            return false;
        }
        out = a + b;
        return true;
    }

    //------------------------------------------------------------------------------
    //! \brief Overflow-checked multiplication
    //------------------------------------------------------------------------------
    static bool checked_mul( Elf_Xword a, Elf_Xword b, Elf_Xword& out )
    {
        if ( a != 0 && b > std::numeric_limits<Elf_Xword>::max() / a ) {
            return false;
        }
        out = a * b;
        return true;
    }

  private:
    elfio& elf_file; //!< The ELF file being worked on
};

} // namespace ELFIO

#endif // ELFIO_GNU_HASH_REBUILD_HPP
