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

#ifndef ELFIO_BB_MAP_HPP
#define ELFIO_BB_MAP_HPP

#include <elfio/elf_types.hpp>

// This header may be included on its own (it is force-included by the task
// build), so pull in the full elfio definitions. The include guard makes the
// reverse inclusion from elfio.hpp a no-op.
#include <elfio/elfio.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ELFIO {

// SHT_LLVM_BB_ADDR_MAP (LLVM extension, see
// https://llvm.org/docs/Extensions.html#sht-llvm-bb-addr-map-section-
// basic-block-address-map)
constexpr Elf_Word SHT_LLVM_BB_ADDR_MAP = 0x6fff4c0a;

//------------------------------------------------------------------------------
//! \struct bb_map_error
//! \brief Describes a rejected map together with a physical location.
//!        The index is published only when no error occurred.
struct bb_map_error
{
    std::string message;            //!< Human readable failure reason
    Elf_Half    section_index = 0;  //!< Offending section index
    Elf_Xword   section_offset = 0; //!< Byte offset inside that section
    bool        has_section = false;//!< section_index is valid
    bool        has_offset = false; //!< section_offset is valid
};

//------------------------------------------------------------------------------
//! \struct bb_map_callsite
//! \brief One callsite position inside a block. Offsets are section-relative;
//!        map_offset locates the delta ULEB in the map section.
struct bb_map_callsite
{
    Elf_Xword offset = 0;     //!< Section-relative callsite position
    Elf_Xword map_offset = 0; //!< Delta ULEB offset inside the map section
};

//------------------------------------------------------------------------------
//! \struct bb_map_relocation_info
//! \brief Identity of the R_X86_64_64 RELA entry that resolves a base field.
//!        Present only for ET_REL input.
struct bb_map_relocation_info
{
    bool       present = false;   //!< The range was resolved through a RELA
    Elf_Half   rela_section = 0;  //!< RELA section index
    Elf_Xword  rela_index = 0;    //!< Entry index within the RELA section
    Elf_Half   symbol_table = 0;  //!< Symbol table section index (sh_link)
    Elf_Word   symbol_index = 0;  //!< Symbol index within that table
    Elf_Sxword addend = 0;        //!< Signed RELA addend
};

//------------------------------------------------------------------------------
//! \struct bb_map_range
//! \brief One address range of a function. A function split over hot and cold
//!        ranges keeps one entry record and several range records.
struct bb_map_range
{
    unsigned              ordinal = 0;         //!< Range ordinal in function
    Elf_Half              code_section = 0;    //!< Resolved code section index
    Elf64_Addr            raw_base = 0;        //!< Raw 8-byte base field
    Elf_Xword             base_offset = 0;     //!< Section-relative range base
    Elf64_Off             base_file_offset = 0;/*!< Physical file offset of
                                                    the base field */
    Elf_Xword             map_offset = 0;      //!< Base field offset in map
    bb_map_relocation_info relocation;         //!< ET_REL relocation identity
};

//------------------------------------------------------------------------------
//! \struct bb_map_block
//! \brief One basic block record. Blocks keep physical map order; zero-size
//!        blocks are retained but never cover an address.
struct bb_map_block
{
    Elf_Half                     map_section = 0;     //!< Map section index
    Elf_Xword                    function_offset = 0; /*!< Entry byte offset
                                                            in the map */
    unsigned                     range_ordinal = 0;   //!< Owning range ordinal
    Elf_Xword                    block_ordinal = 0;   //!< Ordinal in function
    Elf_Xword                    id = 0;              //!< Raw block ID
    Elf_Half                     code_section = 0;    //!< Code section index
    Elf_Xword                    begin = 0;           //!< Half-open begin
    Elf_Xword                    end = 0;             //!< Half-open end
    Elf_Xword                    metadata = 0;        //!< Raw metadata ULEB
    std::vector<bb_map_callsite> callsites;           //!< Ordered callsites
    Elf_Xword                    map_begin = 0;       //!< Record span begin
    Elf_Xword                    map_end = 0;         //!< Record span end
};

//------------------------------------------------------------------------------
//! \struct bb_map_function
//! \brief One entry record. Function identity is (map section, entry offset).
struct bb_map_function
{
    Elf_Half                  map_section = 0;  //!< Map section index
    Elf_Xword                 entry_offset = 0; //!< Version byte offset
    unsigned char             version = 0;      //!< Record version (2 or 3)
    unsigned char             feature = 0;      //!< Raw feature byte
    std::vector<bb_map_range> ranges;           //!< Ranges, ordinal order
    std::vector<bb_map_block> blocks;           //!< Blocks, physical order
};

//------------------------------------------------------------------------------
//! \class bb_map_index
//! \brief Read-only, self-contained index of every LLVM basic block address
//!        map in an ELF file. The index copies all data it needs, so the
//!        source elfio object may be destroyed once load() returns.
class bb_map_index
{
  public:
    bb_map_index() = default;
    ~bb_map_index() = default;
    bb_map_index( bb_map_index&& ) = default;
    bb_map_index& operator=( bb_map_index&& ) = default;

    //------------------------------------------------------------------------------
    //! \brief Build an index. On failure returns nullptr and fills \p error
    //!        with the offending section and byte offset. No partial index is
    //!        published.
    //! \param elf_file Loaded ELF64 LE EM_X86_64 file
    //! \param error Receives the first failure location
    //! \return Owning index, or nullptr on error
    static std::unique_ptr<bb_map_index>
    load( const elfio& elf_file, bb_map_error& error );

    //------------------------------------------------------------------------------
    //! \brief Number of parsed function entry records
    Elf_Xword get_functions_num() const
    {
        return static_cast<Elf_Xword>( functions.size() );
    }

    //------------------------------------------------------------------------------
    //! \brief Access a function record by index
    const bb_map_function& get_function( Elf_Xword index ) const
    {
        return functions[static_cast<size_t>( index )];
    }

    //------------------------------------------------------------------------------
    //! \brief Number of parsed block records, including zero-size blocks
    Elf_Xword get_blocks_num() const
    {
        return static_cast<Elf_Xword>( block_refs.size() );
    }

    //------------------------------------------------------------------------------
    //! \brief Access a block in physical (map/function/range/block) order
    const bb_map_block& get_block( Elf_Xword index ) const
    {
        const block_ref& ref = block_refs[static_cast<size_t>( index )];
        return functions[ref.function].blocks[ref.block];
    }

    //------------------------------------------------------------------------------
    //! \brief Locate the entry record (map section, entry byte offset)
    const bb_map_function*
    find_function( Elf_Half map_section, Elf_Xword entry_offset ) const
    {
        for ( const auto& function : functions ) {
            if ( function.map_section == map_section &&
                 function.entry_offset == entry_offset ) {
                return &function;
            }
        }
        return nullptr;
    }

    //------------------------------------------------------------------------------
    //! \brief Find every nonempty block whose half-open range contains
    //!        \p offset in code section \p code_section. Gaps and zero-size
    //!        blocks never match; the same numeric offset in another code
    //!        section is a distinct address. Results stay in physical
    //!        map/function/range/block order.
    //! \return Pointers valid for the lifetime of the index
    std::vector<const bb_map_block*>
    find( Elf_Half code_section, Elf_Xword offset ) const
    {
        std::vector<const bb_map_block*> hits;
        for ( const auto& ref : block_refs ) {
            const bb_map_block& block =
                functions[ref.function].blocks[ref.block];
            if ( block.code_section == code_section &&
                 block.begin <= offset && offset < block.end ) {
                hits.push_back( &block );
            }
        }
        return hits;
    }

  private:
    struct block_ref
    {
        size_t function = 0;
        size_t block = 0;
    };

    std::vector<bb_map_function> functions;
    std::vector<block_ref>       block_refs;

    friend struct bb_map_builder;
};

//------------------------------------------------------------------------------
namespace bb_map_detail {

//! Bounded ULEB128 reader over one map section.
class uleb_reader
{
  public:
    uleb_reader( const unsigned char* section_data, Elf_Xword section_size )
        : data( section_data ), size( section_size )
    {
    }

    bool       at_end() const { return pos == size; }
    Elf_Xword  tell() const { return pos; }

    //! Skip \p count raw (non-ULEB) bytes.
    bool skip( Elf_Xword count )
    {
        if ( count > size - pos ) {
            return false;
        }
        pos += count;
        return true;
    }

    //! Read one ULEB128; \p start_offset receives the first byte position.
    bool read( Elf_Xword& value, Elf_Xword& start_offset )
    {
        start_offset = pos;
        Elf_Xword result = 0;
        unsigned  shift = 0;
        while ( true ) {
            if ( pos >= size || shift >= 64 ) {
                return false;
            }
            unsigned char byte = data[pos++];
            unsigned char piece = byte & 0x7F;
            if ( shift >= 57 &&
                 piece > ( Elf_Xword( 1 ) << ( 64 - shift ) ) - 1 ) {
                return false; // Value does not fit in 64 bits
            }
            result |= static_cast<Elf_Xword>( piece ) << shift;
            if ( ( byte & 0x80 ) == 0 ) {
                value = result;
                return true;
            }
            shift += 7;
        }
    }

  private:
    const unsigned char* data;
    Elf_Xword            size;
    Elf_Xword            pos = 0;
};

inline bool add_checked( Elf_Xword a, Elf_Xword b, Elf_Xword& result )
{
    if ( b > std::numeric_limits<Elf_Xword>::max() - a ) {
        return false;
    }
    result = a + b;
    return true;
}

inline bool signed_add_checked( Elf64_Addr value, Elf_Sxword addend,
                                Elf64_Addr& result )
{
    if ( addend >= 0 ) {
        return add_checked( value, static_cast<Elf_Xword>( addend ), result );
    }
    Elf_Xword magnitude = static_cast<Elf_Xword>( -addend );
    if ( magnitude > value ) {
        return false;
    }
    result = value - magnitude;
    return true;
}

inline Elf64_Addr read_le64( const unsigned char* p )
{
    Elf64_Addr value = 0;
    for ( int i = 0; i < 8; ++i ) {
        value |= static_cast<Elf64_Addr>( p[i] ) << ( 8 * i );
    }
    return value;
}

} // namespace bb_map_detail

//------------------------------------------------------------------------------
struct bb_map_builder
{
    const elfio&  elf;
    bb_map_error& error;
    bb_map_index  draft;

    bb_map_builder( const elfio& elf_file, bb_map_error& err )
        : elf( elf_file ), error( err )
    {
    }

    void fail( const std::string& message, Elf_Half section,
               bool has_section, Elf_Xword offset, bool has_offset )
    {
        error.message = message;
        error.section_index = section;
        error.has_section = has_section;
        error.section_offset = offset;
        error.has_offset = has_offset;
    }

    void fail_at( const std::string& message, Elf_Half section,
                  Elf_Xword offset )
    {
        fail( message, section, true, offset, true );
    }

    //! One base field and the function/range record that owns it.
    struct base_ref
    {
        size_t   function = 0;
        size_t   range = 0;
        Elf_Xword map_offset = 0;
    };

    //! One RELA entry resolving a base field (ET_REL only).
    struct base_reloc
    {
        Elf_Half   rela_section = 0;
        Elf_Xword  rela_index = 0;
        Elf_Half   symbol_table = 0;
        Elf_Word   symbol_index = 0;
        Elf_Sxword addend = 0;
    };

    bool run()
    {
        if ( elf.get_class() != ELFCLASS64 ||
             elf.get_encoding() != ELFDATA2LSB ||
             elf.get_machine() != EM_X86_64 ) {
            fail( "only ELF64 little-endian EM_X86_64 is supported", 0, false,
                 0, false );
            return false;
        }
        if ( elf.get_type() != ET_REL && elf.get_type() != ET_EXEC &&
             elf.get_type() != ET_DYN ) {
            fail( "only ET_REL, ET_EXEC and ET_DYN are supported", 0, false, 0,
                 false );
            return false;
        }

        for ( Elf_Half i = 0; i < elf.sections.size(); ++i ) {
            const section* map = elf.sections[i];
            if ( map->get_type() != SHT_LLVM_BB_ADDR_MAP ) {
                continue;
            }
            if ( ( map->get_flags() &
                   ( SHF_COMPRESSED | SHF_RPX_DEFLATE ) ) != 0 ) {
                fail_at( "compressed bb-addr-map sections are unsupported", i,
                         0 );
                return false;
            }

            std::vector<base_ref> refs;
            if ( !parse_section( i, map, refs ) ) {
                return false;
            }

            std::vector<base_reloc> relocs;
            if ( elf.get_type() == ET_REL ) {
                if ( !collect_relocations( i, refs, relocs ) ) {
                    return false;
                }
            }
            else if ( !has_no_map_relocations( i ) ) {
                return false;
            }

            if ( !resolve_section( i, refs, relocs ) ) {
                return false;
            }
        }

        publish();
        return true;
    }

  private:
    //! Parse every entry of one map section. Base fields are recorded in \p
    //! refs in physical order; block coordinates stay range-relative here.
    bool parse_section( Elf_Half map_index, const section* map,
                        std::vector<base_ref>& refs )
    {
        Elf_Xword map_size = map->get_size();
        const unsigned char* data =
            map_size == 0 ? nullptr
                          : reinterpret_cast<const unsigned char*>(
                                map->get_data() );
        if ( map_size > 0 && data == nullptr ) {
            fail_at( "map section data is not available", map_index, 0 );
            return false;
        }
        bb_map_detail::uleb_reader reader( data, map_size );

        while ( !reader.at_end() ) {
            Elf_Xword entry_off = reader.tell();
            if ( !reader.skip( 2 ) ) {
                fail_at( "truncated map entry header", map_index, entry_off );
                return false;
            }
            unsigned char version = data[entry_off];
            unsigned char feature = data[entry_off + 1];
            if ( !is_supported_encoding( version, feature ) ) {
                fail_at( "unsupported bb-addr-map version or feature",
                         map_index, entry_off );
                return false;
            }

            bb_map_function function;
            function.map_section = map_index;
            function.entry_offset = entry_off;
            function.version = version;
            function.feature = feature;

            Elf_Xword range_count = 1;
            if ( ( feature & 0x8 ) != 0 ) {
                Elf_Xword count_off = 0;
                if ( !reader.read( range_count, count_off ) ) {
                    fail_at( "truncated range count", map_index, count_off );
                    return false;
                }
            }
            bool callsite_encoding = ( version == 3 && ( feature & 0x20 ) );
            Elf_Xword block_ordinal = 0;

            for ( Elf_Xword r = 0; r < range_count; ++r ) {
                Elf_Xword base_off = reader.tell();
                if ( !reader.skip( 8 ) ) {
                    fail_at( "truncated range base field", map_index,
                             base_off );
                    return false;
                }
                Elf64_Addr raw_base =
                    bb_map_detail::read_le64( data + base_off );

                Elf_Xword block_count = 0;
                Elf_Xword count_off = 0;
                if ( !reader.read( block_count, count_off ) ) {
                    fail_at( "truncated block count", map_index, count_off );
                    return false;
                }

                bb_map_range range;
                range.ordinal = static_cast<unsigned>( r );
                range.raw_base = raw_base;
                range.map_offset = base_off;
                range.base_file_offset = map->get_offset() + base_off;
                function.ranges.push_back( range );
                refs.push_back( base_ref{ draft.functions.size(),
                                          function.ranges.size() - 1,
                                          base_off } );

                Elf_Xword cursor = 0; // Cumulative offset from the range base
                for ( Elf_Xword b = 0; b < block_count; ++b ) {
                    Elf_Xword record_begin = reader.tell();
                    Elf_Xword id = 0;
                    Elf_Xword gap = 0;
                    Elf_Xword id_off = 0;
                    Elf_Xword gap_off = 0;
                    if ( !reader.read( id, id_off ) ||
                         !reader.read( gap, gap_off ) ) {
                        fail_at( "truncated block header", map_index,
                                 reader.tell() );
                        return false;
                    }
                    Elf_Xword begin = 0;
                    if ( !bb_map_detail::add_checked( cursor, gap, begin ) ) {
                        fail_at( "block offset overflow", map_index,
                                 gap_off );
                        return false;
                    }

                    bb_map_block block;
                    block.map_section = map_index;
                    block.function_offset = entry_off;
                    block.range_ordinal = range.ordinal;
                    block.block_ordinal = block_ordinal++;
                    block.id = id;
                    block.begin = begin;
                    block.map_begin = record_begin;

                    Elf_Xword end = 0;
                    if ( callsite_encoding ) {
                        Elf_Xword site_count = 0;
                        Elf_Xword site_count_off = 0;
                        if ( !reader.read( site_count, site_count_off ) ) {
                            fail_at( "truncated callsite count", map_index,
                                     site_count_off );
                            return false;
                        }
                        Elf_Xword site_cursor = begin;
                        for ( Elf_Xword c = 0; c < site_count; ++c ) {
                            Elf_Xword delta = 0;
                            Elf_Xword delta_off = 0;
                            if ( !reader.read( delta, delta_off ) ) {
                                fail_at( "truncated callsite delta", map_index,
                                         delta_off );
                                return false;
                            }
                            Elf_Xword site = 0;
                            if ( !bb_map_detail::add_checked( site_cursor,
                                                              delta, site ) ) {
                                fail_at( "callsite offset overflow", map_index,
                                         delta_off );
                                return false;
                            }
                            block.callsites.push_back(
                                bb_map_callsite{ site, delta_off } );
                            site_cursor = site;
                        }
                        Elf_Xword tail = 0;
                        Elf_Xword tail_off = 0;
                        if ( !reader.read( tail, tail_off ) ) {
                            fail_at( "truncated block tail", map_index,
                                     tail_off );
                            return false;
                        }
                        Elf_Xword tail_origin =
                            block.callsites.empty()
                                ? begin
                                : block.callsites.back().offset;
                        if ( !bb_map_detail::add_checked( tail_origin, tail,
                                                          end ) ) {
                            fail_at( "block end overflow", map_index,
                                     tail_off );
                            return false;
                        }
                    }
                    else {
                        Elf_Xword size = 0;
                        Elf_Xword size_off = 0;
                        if ( !reader.read( size, size_off ) ) {
                            fail_at( "truncated block size", map_index,
                                     size_off );
                            return false;
                        }
                        if ( !bb_map_detail::add_checked( begin, size, end ) ) {
                            fail_at( "block end overflow", map_index,
                                     size_off );
                            return false;
                        }
                    }

                    Elf_Xword metadata = 0;
                    Elf_Xword metadata_off = 0;
                    if ( !reader.read( metadata, metadata_off ) ) {
                        fail_at( "truncated block metadata", map_index,
                                 metadata_off );
                        return false;
                    }
                    block.end = end;
                    block.metadata = metadata;
                    block.map_end = reader.tell();

                    for ( const auto& earlier : function.blocks ) {
                        if ( earlier.id == id ) {
                            fail_at( "duplicate block id within function",
                                     map_index, record_begin );
                            return false;
                        }
                    }
                    for ( const auto& callsite : block.callsites ) {
                        if ( callsite.offset > end ) {
                            fail_at( "callsite outside its block", map_index,
                                     callsite.map_offset );
                            return false;
                        }
                    }
                    cursor = end;
                    function.blocks.push_back( std::move( block ) );
                }
            }
            draft.functions.push_back( std::move( function ) );
        }
        return true;
    }

    static bool is_supported_encoding( unsigned char version,
                                       unsigned char feature )
    {
        if ( version == 2 ) {
            return feature == 0 || feature == 8;
        }
        if ( version == 3 ) {
            return feature == 0 || feature == 8 || feature == 32 ||
                   feature == 40;
        }
        return false;
    }

    //! ET_REL: every base field must get exactly one R_X86_64_64 entry from a
    //! RELA whose sh_info names this map section. REL tables, other relocation
    //! types, duplicates, missing entries and stray sites are all errors.
    bool collect_relocations( Elf_Half map_index,
                              const std::vector<base_ref>& refs,
                              std::vector<base_reloc>&     relocs )
    {
        relocs.assign( refs.size(), base_reloc{} );
        std::vector<unsigned> found( refs.size(), 0 );

        for ( Elf_Half i = 0; i < elf.sections.size(); ++i ) {
            const section* rela = elf.sections[i];
            if ( rela->get_info() != map_index ) {
                continue;
            }
            if ( rela->get_type() == SHT_REL ) {
                fail_at( "SHT_REL against a bb-addr-map is unsupported; "
                         "RELA is required",
                         i, 0 );
                return false;
            }
            if ( rela->get_type() != SHT_RELA ) {
                continue;
            }
            const_relocation_section_accessor accessor( elf, rela );
            Elf_Half symtab_index =
                static_cast<Elf_Half>( rela->get_link() );
            Elf_Xword entry_size = rela->get_entry_size();
            for ( Elf_Xword r = 0; r < accessor.get_entries_num(); ++r ) {
                Elf64_Addr  r_offset = 0;
                Elf_Word    symbol = 0;
                unsigned    type = 0;
                Elf_Sxword  addend = 0;
                if ( !accessor.get_entry( r, r_offset, symbol, type,
                                          addend ) ) {
                    fail_at( "cannot parse RELA entry", i, r * entry_size );
                    return false;
                }
                if ( type != R_X86_64_64 ) {
                    fail_at( "unsupported relocation type on map base field",
                             i, r * entry_size );
                    return false;
                }
                size_t slot = refs.size();
                for ( size_t k = 0; k < refs.size(); ++k ) {
                    if ( refs[k].map_offset == r_offset ) {
                        slot = k;
                        break;
                    }
                }
                if ( slot == refs.size() ) {
                    fail_at( "relocation site outside map base fields",
                             map_index, static_cast<Elf_Xword>( r_offset ) );
                    return false;
                }
                if ( found[slot] != 0 ) {
                    fail_at( "duplicate relocation for map base field",
                             map_index, refs[slot].map_offset );
                    return false;
                }
                found[slot] = 1;
                relocs[slot] = base_reloc{ i, r, symtab_index, symbol,
                                           addend };
            }
        }

        for ( size_t k = 0; k < refs.size(); ++k ) {
            if ( found[k] == 0 ) {
                fail_at( "missing relocation for map base field", map_index,
                         refs[k].map_offset );
                return false;
            }
        }
        return true;
    }

    //! Linked objects must carry no relocations against map sections.
    bool has_no_map_relocations( Elf_Half map_index )
    {
        for ( Elf_Half i = 0; i < elf.sections.size(); ++i ) {
            const section* rela = elf.sections[i];
            if ( ( rela->get_type() == SHT_RELA ||
                   rela->get_type() == SHT_REL ) &&
                 rela->get_info() == map_index ) {
                fail_at( "linked objects must not relocate map base fields",
                         i, 0 );
                return false;
            }
        }
        return true;
    }

    //! ET_REL: resolve one base field through its defined STT_SECTION or
    //! STT_FUNC symbol plus the signed RELA addend.
    bool resolve_reloc_base( const base_reloc& reloc,
                             Elf_Half&         code_section,
                             Elf_Xword&        base_offset )
    {
        const section* symtab = elf.sections[reloc.symbol_table];
        Elf_Xword rela_byte =
            reloc.rela_index *
            elf.sections[reloc.rela_section]->get_entry_size();
        if ( symtab == nullptr ||
             ( symtab->get_type() != SHT_SYMTAB &&
               symtab->get_type() != SHT_DYNSYM ) ) {
            fail_at( "relocation symbol table is missing",
                     reloc.rela_section, rela_byte );
            return false;
        }
        const_symbol_section_accessor symbols( elf, symtab );
        std::string   name;
        Elf64_Addr    value = 0;
        Elf_Xword     size = 0;
        unsigned char bind = 0;
        unsigned char type = 0;
        Elf_Half      shndx = 0;
        unsigned char other = 0;
        if ( !symbols.get_symbol( reloc.symbol_index, name, value, size, bind,
                                  type, shndx, other ) ) {
            fail_at( "relocation symbol not found", reloc.rela_section,
                     rela_byte );
            return false;
        }
        if ( shndx == SHN_UNDEF || shndx >= SHN_LORESERVE ) {
            fail_at( "map base relocation uses an unsupported symbol section",
                     reloc.rela_section, rela_byte );
            return false;
        }
        if ( type != STT_SECTION && type != STT_FUNC ) {
            fail_at( "map base relocation symbol must be STT_SECTION or "
                     "STT_FUNC",
                     reloc.rela_section, rela_byte );
            return false;
        }
        if ( shndx >= elf.sections.size() ) {
            fail_at( "map base relocation symbol section index out of range",
                     reloc.rela_section, rela_byte );
            return false;
        }
        Elf64_Addr resolved = 0;
        if ( !bb_map_detail::signed_add_checked( value, reloc.addend,
                                                 resolved ) ) {
            fail_at( "map base relocation addend overflows",
                     reloc.rela_section, rela_byte );
            return false;
        }
        code_section = shndx;
        base_offset = static_cast<Elf_Xword>( resolved );
        return true;
    }

    //! Linked object: map a base VA to the unique executable PROGBITS section
    //! holding the complete [base, base + extent) range.
    bool map_va_to_section( Elf64_Addr base, Elf_Xword extent,
                            Elf_Half map_index, Elf_Xword map_off,
                            Elf_Half& code_section, Elf_Xword& base_offset )
    {
        Elf_Half candidate = 0;
        unsigned matches = 0;
        for ( Elf_Half i = 0; i < elf.sections.size(); ++i ) {
            const section* sec = elf.sections[i];
            if ( sec->get_type() != SHT_PROGBITS ||
                 ( sec->get_flags() & SHF_EXECINSTR ) == 0 ) {
                continue;
            }
            Elf64_Addr addr = sec->get_address();
            Elf_Xword  size = sec->get_size();
            if ( addr > base ) {
                continue;
            }
            Elf_Xword relative = static_cast<Elf_Xword>( base - addr );
            Elf_Xword range_end = 0;
            if ( !bb_map_detail::add_checked( relative, extent, range_end ) ||
                 range_end > size ) {
                continue;
            }
            ++matches;
            candidate = i;
        }
        if ( matches == 0 ) {
            fail_at( "map base address is not inside an executable section",
                     map_index, map_off );
            return false;
        }
        if ( matches > 1 ) {
            fail_at( "map base address matches several executable sections",
                     map_index, map_off );
            return false;
        }
        code_section = candidate;
        base_offset = static_cast<Elf_Xword>(
            base - elf.sections[candidate]->get_address() );
        return true;
    }

    //! Attach section identity and absolute section-relative coordinates to
    //! every range and block of one parsed map section.
    bool resolve_section( Elf_Half map_index,
                          const std::vector<base_ref>&  refs,
                          const std::vector<base_reloc>& relocs )
    {
        for ( size_t k = 0; k < refs.size(); ++k ) {
            bb_map_function& function = draft.functions[refs[k].function];
            bb_map_range&    range = function.ranges[refs[k].range];

            Elf_Half  code_section = 0;
            Elf_Xword base_offset = 0;
            if ( elf.get_type() == ET_REL ) {
                const base_reloc& reloc = relocs[k];
                if ( !resolve_reloc_base( reloc, code_section,
                                          base_offset ) ) {
                    return false;
                }
                range.relocation.present = true;
                range.relocation.rela_section = reloc.rela_section;
                range.relocation.rela_index = reloc.rela_index;
                range.relocation.symbol_table = reloc.symbol_table;
                range.relocation.symbol_index = reloc.symbol_index;
                range.relocation.addend = reloc.addend;
            }
            else {
                Elf_Xword extent = 0;
                for ( const auto& block : function.blocks ) {
                    if ( block.range_ordinal == range.ordinal &&
                         block.end > extent ) {
                        extent = block.end;
                    }
                }
                if ( !map_va_to_section( range.raw_base, extent, map_index,
                                         range.map_offset, code_section,
                                         base_offset ) ) {
                    return false;
                }
            }

            const section* target = elf.sections[code_section];
            if ( target == nullptr || base_offset > target->get_size() ) {
                fail_at( "range base reaches outside its code section",
                         map_index, range.map_offset );
                return false;
            }
            Elf_Xword target_size = target->get_size();
            range.code_section = code_section;
            range.base_offset = base_offset;

            for ( auto& block : function.blocks ) {
                if ( block.range_ordinal != range.ordinal ) {
                    continue;
                }
                Elf_Xword block_begin = 0;
                Elf_Xword block_end = 0;
                if ( !bb_map_detail::add_checked( base_offset, block.begin,
                                                  block_begin ) ||
                     !bb_map_detail::add_checked( base_offset, block.end,
                                                  block_end ) ||
                     block_end > target_size ) {
                    fail_at( "block reaches outside its code section",
                             map_index, block.map_begin );
                    return false;
                }
                block.code_section = code_section;
                block.begin = block_begin;
                block.end = block_end;
                for ( auto& callsite : block.callsites ) {
                    Elf_Xword site = 0;
                    if ( !bb_map_detail::add_checked( base_offset,
                                                      callsite.offset, site ) ||
                         site > target_size ) {
                        fail_at( "callsite reaches outside its code section",
                                 map_index, callsite.map_offset );
                        return false;
                    }
                    callsite.offset = site;
                }
            }
        }
        return true;
    }

    void publish()
    {
        for ( size_t f = 0; f < draft.functions.size(); ++f ) {
            for ( size_t b = 0; b < draft.functions[f].blocks.size(); ++b ) {
                draft.block_refs.push_back(
                    bb_map_index::block_ref{ f, b } );
            }
        }
    }
};

//------------------------------------------------------------------------------
inline std::unique_ptr<bb_map_index>
bb_map_index::load( const elfio& elf_file, bb_map_error& error )
{
    auto index = std::unique_ptr<bb_map_index>( new bb_map_index() );
    bb_map_builder builder( elf_file, error );
    if ( !builder.run() ) {
        return nullptr;
    }
    *index = std::move( builder.draft );
    return index;
}

} // namespace ELFIO

#endif // ELFIO_BB_MAP_HPP
