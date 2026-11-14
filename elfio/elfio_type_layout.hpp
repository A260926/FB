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

#ifndef ELFIO_TYPE_LAYOUT_HPP
#define ELFIO_TYPE_LAYOUT_HPP

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace ELFIO {

// DWARF v4 tags handled by this interface.
constexpr unsigned DW_TAG_array_type       = 0x01;
constexpr unsigned DW_TAG_member           = 0x0d;
constexpr unsigned DW_TAG_pointer_type     = 0x0f;
constexpr unsigned DW_TAG_compile_unit     = 0x11;
constexpr unsigned DW_TAG_structure_type   = 0x13;
constexpr unsigned DW_TAG_union_type       = 0x17;
constexpr unsigned DW_TAG_typedef          = 0x16;
constexpr unsigned DW_TAG_base_type        = 0x24;
constexpr unsigned DW_TAG_const_type       = 0x26;
constexpr unsigned DW_TAG_subrange_type    = 0x21;
constexpr unsigned DW_TAG_volatile_type    = 0x35;

// DWARF v4 attributes used by this interface.
constexpr unsigned DW_AT_name                  = 0x03;
constexpr unsigned DW_AT_byte_size             = 0x0b;
constexpr unsigned DW_AT_bit_offset            = 0x0c;
constexpr unsigned DW_AT_bit_size              = 0x0d;
constexpr unsigned DW_AT_lower_bound           = 0x22;
constexpr unsigned DW_AT_upper_bound           = 0x2f;
constexpr unsigned DW_AT_count                 = 0x37;
constexpr unsigned DW_AT_data_member_location  = 0x38;
constexpr unsigned DW_AT_data_bit_offset       = 0x4b;
constexpr unsigned DW_AT_type                  = 0x49;

// DWARF v4 forms understood by this interface.
constexpr unsigned DW_FORM_data1       = 0x0b;
constexpr unsigned DW_FORM_data2       = 0x05;
constexpr unsigned DW_FORM_data4       = 0x06;
constexpr unsigned DW_FORM_data8       = 0x07;
constexpr unsigned DW_FORM_string      = 0x08;
constexpr unsigned DW_FORM_ref4        = 0x13;
constexpr unsigned DW_FORM_flag_present = 0x19;
constexpr unsigned DW_FORM_strp        = 0x0e;
constexpr unsigned DW_FORM_udata       = 0x0f;
constexpr unsigned DW_FORM_sdata       = 0x10;

//------------------------------------------------------------------------------
//! Kind of a node in the recovered type graph.
enum class type_kind
{
    compile_unit,
    base_type,
    structure_type,
    union_type,
    member,
    pointer_type,
    typedef_type,
    const_type,
    volatile_type,
    array_type,
    subrange_type
};

//------------------------------------------------------------------------------
//! Placement details of a DW_TAG_member node. All offsets are kept both in
//! their original DWARF representation and as unified bit offsets from the
//! beginning of the containing object.
struct member_layout_info
{
    // DW_AT_data_member_location, when present (always non-negative).
    bool has_byte_offset = false;
    std::uint64_t byte_offset = 0;

    bool is_bitfield = false;

    // Modern DWARF: DW_AT_data_bit_offset (+ DW_AT_bit_size).
    bool has_data_bit_offset = false;
    std::uint64_t data_bit_offset = 0;

    // Legacy DWARF: DW_AT_bit_offset (+ DW_AT_bit_size).
    bool has_legacy_bit_offset = false;
    std::uint64_t legacy_bit_offset = 0;

    // DW_AT_bit_size, present for every bitfield.
    std::uint64_t bit_size = 0;

    // Storage unit size: member's own DW_AT_byte_size if present, otherwise
    // the size of the member's type.
    std::uint64_t storage_byte_size = 0;

    // Unified view: bit offset and width measured from the object start.
    std::uint64_t bit_offset = 0;
    std::uint64_t bit_width  = 0;
};

//------------------------------------------------------------------------------
//! Bounds of a DW_TAG_subrange_type node.
struct subrange_info
{
    bool has_count = false;
    std::int64_t count = 0;

    bool has_lower_bound = false;
    std::int64_t lower_bound = 0;

    bool has_upper_bound = false;
    std::int64_t upper_bound = 0;

    // Number of elements, resolved either from count or from the
    // lower/upper bound pair.
    bool has_element_count = false;
    std::int64_t element_count = 0;
};

//------------------------------------------------------------------------------
//! A node of the recovered type graph. Nodes are identified by their DIE
//! offset within .debug_info; references always point at the same node.
struct type_node
{
    type_kind kind = type_kind::base_type;
    std::uint64_t die_offset = 0; //!< Section offset of the DIE
    std::string name;             //!< Empty for anonymous DIEs

    bool has_byte_size = false;
    std::uint64_t byte_size = 0;

    //! DW_AT_type target; null for void pointers and DIEs without a type.
    const type_node* type = nullptr;

    //! Members of structure/union types, in declaration order. Duplicate and
    //! anonymous members are preserved.
    std::vector<const type_node*> members;

    //! Subranges of an array type, in declaration order.
    std::vector<const type_node*> subranges;

    member_layout_info member_info;
    subrange_info      range;
};

//------------------------------------------------------------------------------
//! Owning type graph returned on success.
struct type_graph
{
    std::vector<std::unique_ptr<type_node>> nodes; //!< Owners of the nodes
    const type_node* root = nullptr;               //!< Requested root DIE
};

//------------------------------------------------------------------------------
//! Error description. The offset is relative to the named section, so the
//! offending byte can be located without scanning the whole image.
struct type_layout_error
{
    std::string section;
    std::uint64_t offset = 0;
    std::string message;
    bool unsupported = false;
};

//------------------------------------------------------------------------------
//! Result of a layout request: either an owning graph or an error carrying
//! a section-relative position.
using type_layout_result =
    std::variant<type_graph, type_layout_error>;

namespace tlayout_detail {

//------------------------------------------------------------------------------
struct section_bytes
{
    const unsigned char* data = nullptr;
    std::size_t          size = 0;
    std::string          name;
};

//------------------------------------------------------------------------------
class cursor
{
  public:
    cursor( section_bytes section, bool big_endian )
        : sb( std::move( section ) ), be( big_endian )
    {
    }

    std::uint64_t position() const { return pos; }
    bool at_end() const { return pos == sb.size; }
    bool bytes_left( std::size_t n ) const { return n <= sb.size - pos; }

    void seek( std::uint64_t target )
    {
        if ( target > sb.size ) {
            fail_at( target, "offset is out of section bounds" );
        }
        pos = static_cast<std::size_t>( target );
    }

    std::uint8_t u8()
    {
        require( 1 );
        return sb.data[pos++];
    }

    std::uint16_t u16()
    {
        require( 2 );
        std::uint16_t v;
        if ( be ) {
            v = static_cast<std::uint16_t>( sb.data[pos] ) << 8 |
                sb.data[pos + 1];
        }
        else {
            v = static_cast<std::uint16_t>( sb.data[pos] ) |
                static_cast<std::uint16_t>( sb.data[pos + 1] ) << 8;
        }
        pos += 2;
        return v;
    }

    std::uint32_t u32()
    {
        require( 4 );
        std::uint32_t v = 0;
        for ( int i = 0; i < 4; ++i ) {
            std::uint8_t b = sb.data[pos + ( be ? 3 - i : i )];
            v |= static_cast<std::uint32_t>( b ) << ( 8 * i );
        }
        pos += 4;
        return v;
    }

    std::uint64_t u64()
    {
        require( 8 );
        std::uint64_t v = 0;
        for ( int i = 0; i < 8; ++i ) {
            std::uint64_t b = sb.data[pos + ( be ? 7 - i : i )];
            v |= b << ( 8 * i );
        }
        pos += 8;
        return v;
    }

    std::uint64_t uleb128()
    {
        std::uint64_t result = 0;
        int shift = 0;
        while ( true ) {
            std::uint8_t b = u8();
            if ( shift >= 64 ) {
                fail( "ULEB128 overflow" );
            }
            result |= static_cast<std::uint64_t>( b & 0x7f ) << shift;
            if ( ( b & 0x80 ) == 0 ) {
                break;
            }
            shift += 7;
        }
        return result;
    }

    std::int64_t sleb128()
    {
        std::int64_t result = 0;
        int shift = 0;
        std::uint8_t b = 0;
        while ( true ) {
            b = u8();
            if ( shift >= 64 ) {
                fail( "SLEB128 overflow" );
            }
            result |= static_cast<std::int64_t>( b & 0x7f ) << shift;
            shift += 7;
            if ( ( b & 0x80 ) == 0 ) {
                break;
            }
        }
        if ( shift < 64 && ( b & 0x40 ) != 0 ) {
            result |= -( static_cast<std::int64_t>( 1 ) << shift );
        }
        return result;
    }

    //! Reads a NUL-terminated string starting at the cursor.
    std::string cstring()
    {
        std::size_t start = pos;
        while ( pos < sb.size && sb.data[pos] != 0 ) {
            ++pos;
        }
        if ( pos == sb.size ) {
            fail( "unterminated string" );
        }
        std::string s( reinterpret_cast<const char*>( sb.data + start ),
                       pos - start );
        ++pos; // consume NUL
        return s;
    }

    //! Reads a NUL-terminated string at an arbitrary section offset.
    std::string cstring_at( std::uint64_t offset )
    {
        if ( offset >= sb.size ) {
            fail_at( offset, "string offset out of section bounds" );
        }
        std::size_t start = static_cast<std::size_t>( offset );
        std::size_t p     = start;
        while ( p < sb.size && sb.data[p] != 0 ) {
            ++p;
        }
        if ( p == sb.size ) {
            fail_at( offset, "unterminated string" );
        }
        return std::string( reinterpret_cast<const char*>( sb.data + start ),
                            p - start );
    }

    [[noreturn]] void fail( const std::string& message ) const
    {
        fail_at( pos, message );
    }

    [[noreturn]] void fail_at( std::uint64_t offset,
                               const std::string& message ) const
    {
        throw type_layout_error{ sb.name, offset, message, false };
    }

  private:
    void require( std::size_t n )
    {
        if ( !bytes_left( n ) ) {
            fail( "unexpected end of section" );
        }
    }

    section_bytes sb;
    std::size_t   pos = 0;
    bool          be;
};

//------------------------------------------------------------------------------
struct attr_spec
{
    unsigned attr = 0;
    unsigned form = 0;
};

struct abbrev_decl
{
    unsigned tag = 0;
    bool has_children = false;
    std::vector<attr_spec> attrs;
};

struct raw_attr
{
    unsigned attr = 0;
    unsigned form = 0;
    bool is_signed = false;
    bool is_string = false;
    bool is_ref    = false;
    std::uint64_t u = 0;
    std::int64_t  s = 0;
    std::string  str;
};

struct raw_die
{
    std::uint64_t offset = 0;
    unsigned tag = 0;
    int depth = 0;
    std::uint64_t parent = 0; //!< Offset of the enclosing DIE; 0 for the root
    std::vector<raw_attr> attrs;
};

//------------------------------------------------------------------------------
//! Exception used internally to unwind parse failures; converted into a
//! type_layout_result at the public boundary.
struct layout_failure
{
    type_layout_error error;
};

//------------------------------------------------------------------------------
static bool checked_add( std::uint64_t a, std::uint64_t b,
                         std::uint64_t& out )
{
    if ( b > std::numeric_limits<std::uint64_t>::max() - a ) {
        return false;
    }
    out = a + b;
    return true;
}

//------------------------------------------------------------------------------
static bool checked_mul( std::uint64_t a, std::uint64_t b,
                         std::uint64_t& out )
{
    if ( a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a ) {
        return false;
    }
    out = a * b;
    return true;
}

//------------------------------------------------------------------------------
class layout_builder
{
  public:
    layout_builder( const elfio& reader, std::uint64_t unit_offset,
                    std::uint64_t root_offset )
        : reader( reader ), unit_offset( unit_offset ),
          root_offset( root_offset )
    {
    }

    type_graph run()
    {
        load_sections();
        parse_unit_header();
        parse_abbrev_table();
        scan_dies();
        validate_references();
        build_graph();
        return std::move( graph );
    }

  private:
    //--------------------------------------------------------------------------
    void load_sections()
    {
        info_sec = get_section( ".debug_info" );
        abbrev_sec = get_section( ".debug_abbrev" );
        str_sec = get_section( ".debug_str", /*optional=*/true );
        big_endian = reader.get_encoding() == ELFDATA2MSB;
    }

    section_bytes get_section( const std::string& name,
                               bool optional = false )
    {
        section* sec = reader.sections[ name ];
        if ( sec == nullptr ) {
            if ( optional ) {
                return { nullptr, 0, name };
            }
            throw layout_failure{
                { name, 0, "required section is missing", false } };
        }
        return { reinterpret_cast<const unsigned char*>( sec->get_data() ),
                 static_cast<std::size_t>( sec->get_size() ), name };
    }

    //--------------------------------------------------------------------------
    void parse_unit_header()
    {
        cursor c( info_sec, big_endian );
        if ( unit_offset >= info_sec.size ||
             info_sec.size - unit_offset < 11 ) {
            throw layout_failure{
                { info_sec.name, unit_offset,
                  "compilation unit header is out of section bounds",
                  false } };
        }
        // DWARF32 unit header: unit_length(4) version(2)
        // debug_abbrev_offset(4) address_size(1)
        std::uint64_t hdr = unit_offset;
        c.seek( hdr );
        std::uint32_t unit_length = c.u32();
        std::uint16_t version     = c.u16();
        abbrev_offset             = c.u32();
        address_size              = c.u8();

        if ( version != 4 ) {
            throw layout_failure{
                { info_sec.name, hdr + 4,
                  "only DWARF version 4 is supported", true } };
        }
        // 0xfffffff0..0xffffffff are reserved; 0xffffffff introduces the
        // 64-bit DWARF format, which this interface does not handle.
        if ( unit_length >= 0xfffffff0 ) {
            throw layout_failure{
                { info_sec.name, hdr,
                  "DWARF64 and reserved unit lengths are unsupported",
                  true } };
        }
        if ( address_size != 1 && address_size != 2 &&
             address_size != 4 && address_size != 8 ) {
            throw layout_failure{
                { info_sec.name, hdr + 10,
                  "invalid unit address size", false } };
        }
        // unit_length covers everything after the length field itself.
        std::uint64_t length_end;
        if ( !checked_add( hdr + 4, unit_length, length_end ) ||
             length_end > info_sec.size ) {
            throw layout_failure{
                { info_sec.name, hdr,
                  "unit length reaches past .debug_info", false } };
        }
        unit_end    = length_end;
        dies_start  = hdr + 11;
        if ( dies_start > unit_end ) {
            throw layout_failure{
                { info_sec.name, hdr, "truncated unit header", false } };
        }
    }

    //--------------------------------------------------------------------------
    void parse_abbrev_table()
    {
        if ( abbrev_offset >= abbrev_sec.size ) {
            throw layout_failure{
                { abbrev_sec.name, abbrev_offset,
                  "abbrev offset out of section bounds", false } };
        }
        cursor c( abbrev_sec, big_endian );
        c.seek( abbrev_offset );

        while ( true ) {
            std::uint64_t decl_pos = c.position();
            std::uint64_t code     = c.uleb128();
            if ( code == 0 ) {
                break;
            }
            if ( code > std::numeric_limits<unsigned>::max() ||
                 abbrevs.count( static_cast<unsigned>( code ) ) != 0 ) {
                throw layout_failure{
                    { abbrev_sec.name, decl_pos,
                      "invalid or duplicate abbreviation code", false } };
            }
            abbrev_decl decl;
            decl.tag = static_cast<unsigned>( c.uleb128() );
            std::uint8_t children = c.u8();
            decl.has_children = children != 0;
            while ( true ) {
                unsigned attr = static_cast<unsigned>( c.uleb128() );
                unsigned form = static_cast<unsigned>( c.uleb128() );
                if ( attr == 0 && form == 0 ) {
                    break;
                }
                decl.attrs.push_back( { attr, form } );
            }
            abbrevs.emplace( static_cast<unsigned>( code ),
                             std::move( decl ) );
        }
    }

    //--------------------------------------------------------------------------
    void scan_dies()
    {
        cursor c( info_sec, big_endian );
        c.seek( dies_start );

        int depth = 0;
        // Offset of the most recent DIE at each depth still open.
        std::vector<std::uint64_t> parent_stack;
        while ( c.position() < unit_end ) {
            std::uint64_t die_pos = c.position();
            std::uint64_t code    = c.uleb128();
            if ( code == 0 ) {
                if ( depth == 0 ) {
                    if ( c.position() != unit_end ) {
                        c.fail_at( die_pos,
                                   "trailing bytes after root DIE chain" );
                    }
                    break;
                }
                --depth;
                parent_stack.resize( depth );
                continue;
            }
            auto it = abbrevs.find( static_cast<unsigned>( code ) );
            if ( it == abbrevs.end() ) {
                c.fail_at( die_pos, "unknown abbreviation code" );
            }
            const abbrev_decl& decl = it->second;

            raw_die die;
            die.offset = die_pos;
            die.tag    = decl.tag;
            die.depth  = depth;
            die.parent = depth > 0 ? parent_stack[depth - 1] : 0;
            die_boundaries.insert( die_pos );

            for ( const attr_spec& spec : decl.attrs ) {
                raw_attr ra;
                ra.attr = spec.attr;
                ra.form = spec.form;
                decode_attr( c, ra );
                die.attrs.push_back( std::move( ra ) );
            }
            raw_dies.push_back( std::move( die ) );

            if ( decl.has_children ) {
                if ( parent_stack.size() <=
                     static_cast<std::size_t>( depth ) ) {
                    parent_stack.resize( depth + 1 );
                }
                parent_stack[depth] = die_pos;
                ++depth;
            }
        }
        if ( c.position() != unit_end ) {
            c.fail_at( c.position(), "abrupt end of DIE chain" );
        }
    }

    void decode_attr( cursor& c, raw_attr& ra )
    {
        switch ( ra.form ) {
        case DW_FORM_data1:
            ra.u = c.u8();
            break;
        case DW_FORM_data2:
            ra.u = c.u16();
            break;
        case DW_FORM_data4:
            ra.u = c.u32();
            break;
        case DW_FORM_data8:
            ra.u = c.u64();
            break;
        case DW_FORM_udata:
            ra.u = c.uleb128();
            break;
        case DW_FORM_sdata:
            ra.is_signed = true;
            ra.s         = c.sleb128();
            break;
        case DW_FORM_flag_present:
            ra.u = 1;
            break;
        case DW_FORM_string:
            ra.is_string = true;
            ra.str       = c.cstring();
            break;
        case DW_FORM_strp: {
            std::uint32_t str_off = c.u32();
            if ( str_sec.data == nullptr ) {
                c.fail( "DW_FORM_strp without .debug_str section" );
            }
            cursor sc( str_sec, big_endian );
            ra.is_string = true;
            ra.str       = sc.cstring_at( str_off );
            break;
        }
        case DW_FORM_ref4: {
            std::uint32_t rel = c.u32();
            ra.is_ref         = true;
            std::uint64_t target;
            if ( !checked_add( unit_offset, rel, target ) ) {
                c.fail( "DW_FORM_ref4 offset overflow" );
            }
            ra.u = target;
            break;
        }
        default:
            throw type_layout_error{
                info_sec.name, c.position(),
                "unsupported attribute form 0x" + to_hex( ra.form ), true };
        }
    }

    static std::string to_hex( unsigned v )
    {
        static const char* digits = "0123456789abcdef";
        std::string s;
        do {
            s.push_back( digits[v & 0xf] );
            v >>= 4;
        } while ( v != 0 );
        s.push_back( 'x' );
        s.push_back( '0' );
        std::reverse( s.begin(), s.end() );
        return s;
    }

    //--------------------------------------------------------------------------
    void validate_references()
    {
        for ( const raw_die& die : raw_dies ) {
            for ( const raw_attr& ra : die.attrs ) {
                if ( !ra.is_ref ) {
                    continue;
                }
                if ( ra.u < dies_start || ra.u >= unit_end ) {
                    throw layout_failure{
                        { info_sec.name, die.offset,
                          "DW_FORM_ref4 points outside its compilation unit",
                          false } };
                }
                if ( die_boundaries.count( ra.u ) == 0 ) {
                    throw layout_failure{
                        { info_sec.name, die.offset,
                          "DW_FORM_ref4 lands inside attribute data",
                          false } };
                }
            }
        }
        if ( die_boundaries.count( root_offset ) == 0 ) {
            throw layout_failure{
                { info_sec.name, root_offset,
                  "root offset is not the start of a DIE", false } };
        }
    }

    //--------------------------------------------------------------------------
    const raw_die* find_raw( std::uint64_t offset ) const
    {
        for ( const raw_die& die : raw_dies ) {
            if ( die.offset == offset ) {
                return &die;
            }
        }
        return nullptr;
    }

    static const raw_attr* find_attr( const raw_die& die, unsigned attr )
    {
        for ( const raw_attr& ra : die.attrs ) {
            if ( ra.attr == attr ) {
                return &ra;
            }
        }
        return nullptr;
    }

    //--------------------------------------------------------------------------
    void build_graph()
    {
        graph.root = get_node( root_offset );
        compute_value_reachability();
        check_value_cycles();
        validate_containers();

        // Every value-carrying node reachable without crossing a pointer
        // must own a resolvable size. The resolved size is written back so
        // every node reports a byte size; pointer-only pointees are excluded,
        // as their incompleteness never affects a by-value layout.
        for ( const auto& entry : node_by_offset ) {
            type_node* node = entry.second;
            if ( value_reachable.count( node->die_offset ) == 0 ) {
                continue;
            }
            if ( node->kind != type_kind::subrange_type &&
                 node->kind != type_kind::compile_unit ) {
                std::uint64_t size = resolve_size( node );
                node->has_byte_size = true;
                node->byte_size     = size;
            }
        }
    }

    type_node* get_node( std::uint64_t offset )
    {
        auto existing = node_by_offset.find( offset );
        if ( existing != node_by_offset.end() ) {
            return existing->second;
        }
        const raw_die* die = find_raw( offset );
        if ( die == nullptr ) {
            throw layout_failure{
                { info_sec.name, offset, "reference target DIE not found",
                  false } };
        }

        auto owned       = std::make_unique<type_node>();
        type_node* node  = owned.get();
        node->die_offset = offset;
        node->kind       = map_tag( die->tag, offset );
        node_by_offset.emplace( offset, node );
        graph.nodes.push_back( std::move( owned ) );

        populate( node, die );
        return node;
    }

    static type_kind map_tag( unsigned tag, std::uint64_t offset )
    {
        switch ( tag ) {
        case DW_TAG_compile_unit:
            return type_kind::compile_unit;
        case DW_TAG_base_type:
            return type_kind::base_type;
        case DW_TAG_structure_type:
            return type_kind::structure_type;
        case DW_TAG_union_type:
            return type_kind::union_type;
        case DW_TAG_member:
            return type_kind::member;
        case DW_TAG_pointer_type:
            return type_kind::pointer_type;
        case DW_TAG_typedef:
            return type_kind::typedef_type;
        case DW_TAG_const_type:
            return type_kind::const_type;
        case DW_TAG_volatile_type:
            return type_kind::volatile_type;
        case DW_TAG_array_type:
            return type_kind::array_type;
        case DW_TAG_subrange_type:
            return type_kind::subrange_type;
        default:
            throw layout_failure{
                { ".debug_info", offset,
                  "unsupported DIE tag 0x" + to_hex( tag ), true } };
        }
    }

    void populate( type_node* node, const raw_die* die )
    {
        if ( const raw_attr* name = find_attr( *die, DW_AT_name ) ) {
            node->name = string_value( *name, node->die_offset );
        }
        if ( const raw_attr* size = find_attr( *die, DW_AT_byte_size ) ) {
            node->has_byte_size = true;
            node->byte_size     = unsigned_value( *size, node->die_offset );
        }
        if ( const raw_attr* type = find_attr( *die, DW_AT_type ) ) {
            // The pointee is always materialized so references keep a single
            // shared identity; whether it counts as by-value reachable is
            // decided later by traversal that never crosses pointer edges.
            node->type = get_node( ref_value( *type, node->die_offset ) );
        }

        switch ( node->kind ) {
        case type_kind::structure_type:
        case type_kind::union_type:
            collect_children( die, DW_TAG_member, node->members );
            break;
        case type_kind::member:
            populate_member( node, die );
            break;
        case type_kind::array_type:
            if ( node->type == nullptr ) {
                fail_at( node->die_offset,
                         "array type lacks an element type reference" );
            }
            collect_children( die, DW_TAG_subrange_type,
                              node->subranges );
            for ( const type_node* sub : node->subranges ) {
                populate_subrange( get_node( sub->die_offset ) );
            }
            break;
        case type_kind::subrange_type:
            populate_subrange( node );
            break;
        case type_kind::typedef_type:
        case type_kind::const_type:
        case type_kind::volatile_type:
            if ( node->type == nullptr ) {
                fail_at( node->die_offset,
                         "qualified or typedef DIE lacks a type reference" );
            }
            break;
        case type_kind::pointer_type:
            // A pointer without a type is a void pointer. Its size comes from
            // the unit's address width, and traversal never follows the
            // reference.
            break;
        default:
            break;
        }
    }

    void collect_children( const raw_die* parent_die,
                           unsigned child_tag,
                           std::vector<const type_node*>& out )
    {
        for ( const raw_die& child : raw_dies ) {
            if ( child.parent != parent_die->offset ) {
                continue;
            }
            // Every direct child participates in the type's layout; an
            // unrecognized child is an unsupported construct rather than
            // something that may be silently dropped.
            if ( child.tag != child_tag ) {
                throw layout_failure{
                    { info_sec.name, child.offset,
                      "unsupported child DIE tag 0x" +
                          to_hex( child.tag ),
                      true } };
            }
            out.push_back( get_node( child.offset ) );
        }
    }

    void populate_member( type_node* node, const raw_die* die )
    {
        member_layout_info& mi = node->member_info;

        if ( const raw_attr* loc =
                 find_attr( *die, DW_AT_data_member_location ) ) {
            std::int64_t v = signed_value( *loc, node->die_offset );
            if ( v < 0 ) {
                fail_at( node->die_offset,
                         "negative data_member_location" );
            }
            mi.has_byte_offset = true;
            mi.byte_offset     = static_cast<std::uint64_t>( v );
        }
        if ( const raw_attr* dbo =
                 find_attr( *die, DW_AT_data_bit_offset ) ) {
            mi.has_data_bit_offset = true;
            mi.data_bit_offset =
                unsigned_value( *dbo, node->die_offset );
        }
        if ( const raw_attr* lbo =
                 find_attr( *die, DW_AT_bit_offset ) ) {
            mi.has_legacy_bit_offset = true;
            std::int64_t v = signed_value( *lbo, node->die_offset );
            if ( v < 0 ) {
                fail_at( node->die_offset, "negative bit_offset" );
            }
            mi.legacy_bit_offset = static_cast<std::uint64_t>( v );
        }
        if ( const raw_attr* bs = find_attr( *die, DW_AT_bit_size ) ) {
            mi.is_bitfield = true;
            mi.bit_size    = unsigned_value( *bs, node->die_offset );
            if ( mi.bit_size == 0 ) {
                fail_at( node->die_offset, "bitfield with zero bit size" );
            }
        }
        if ( ( mi.has_data_bit_offset || mi.has_legacy_bit_offset ) &&
             !mi.is_bitfield ) {
            fail_at( node->die_offset,
                     "bit offset attribute without DW_AT_bit_size" );
        }
        if ( node->type == nullptr ) {
            fail_at( node->die_offset, "member lacks a type reference" );
        }
    }

    void populate_subrange( type_node* node )
    {
        if ( node->range.has_element_count ) {
            return;
        }
        const raw_die* die = find_raw( node->die_offset );
        subrange_info& r   = node->range;

        if ( const raw_attr* count = find_attr( *die, DW_AT_count ) ) {
            std::int64_t v = signed_value( *count, node->die_offset );
            if ( v < 0 ) {
                fail_at( node->die_offset, "negative subrange count" );
            }
            r.has_count = true;
            r.count     = v;
        }
        if ( const raw_attr* lb =
                 find_attr( *die, DW_AT_lower_bound ) ) {
            r.has_lower_bound = true;
            r.lower_bound     = signed_value( *lb, node->die_offset );
        }
        if ( const raw_attr* ub =
                 find_attr( *die, DW_AT_upper_bound ) ) {
            r.has_upper_bound = true;
            r.upper_bound     = signed_value( *ub, node->die_offset );
        }

        if ( r.has_count ) {
            r.has_element_count = true;
            r.element_count     = r.count;
        }
        if ( r.has_lower_bound && r.has_upper_bound ) {
            if ( r.upper_bound < r.lower_bound ) {
                fail_at( node->die_offset,
                         "subrange upper bound is below lower bound" );
            }
            std::uint64_t span;
            if ( !checked_add(
                     static_cast<std::uint64_t>( r.upper_bound -
                                                 r.lower_bound ),
                     1, span ) ) {
                fail_at( node->die_offset, "subrange size overflow" );
            }
            if ( r.has_count &&
                 r.count != static_cast<std::int64_t>( span ) ) {
                fail_at( node->die_offset,
                         "subrange count does not match its bounds" );
            }
            r.has_element_count = true;
            r.element_count     = static_cast<std::int64_t>( span );
        }
        if ( !r.has_element_count ) {
            fail_at( node->die_offset,
                     "subrange has neither count nor lower/upper bounds" );
        }
    }

    //--------------------------------------------------------------------------
    std::uint64_t resolve_size( type_node* node )
    {
        auto cached = size_cache.find( node->die_offset );
        if ( cached != size_cache.end() ) {
            return cached->second;
        }
        if ( in_progress.count( node->die_offset ) != 0 ) {
            throw layout_failure{
                { info_sec.name, node->die_offset,
                  "value-sized type forms a reference cycle", false } };
        }
        in_progress.insert( node->die_offset );
        std::uint64_t size = compute_size( node );
        in_progress.erase( node->die_offset );
        size_cache.emplace( node->die_offset, size );
        return size;
    }

    std::uint64_t compute_size( type_node* node )
    {
        if ( node->has_byte_size ) {
            return node->byte_size;
        }
        switch ( node->kind ) {
        case type_kind::pointer_type:
            // Pointers stop by-value expansion and default to the unit's
            // address width.
            return address_size;
        case type_kind::base_type:
        case type_kind::structure_type:
        case type_kind::union_type:
            fail_at( node->die_offset, "type is missing a byte size" );
        case type_kind::member:
            if ( node->type == nullptr ) {
                fail_at( node->die_offset,
                         "member size is unknown and it has no type" );
            }
            return resolve_size( const_cast<type_node*>( node->type ) );
        case type_kind::typedef_type:
        case type_kind::const_type:
        case type_kind::volatile_type:
            return resolve_size( const_cast<type_node*>( node->type ) );
        case type_kind::array_type: {
            std::uint64_t element =
                resolve_size( const_cast<type_node*>( node->type ) );
            std::uint64_t total = element;
            for ( const type_node* sub_node : node->subranges ) {
                const subrange_info& r = sub_node->range;
                std::uint64_t count =
                    static_cast<std::uint64_t>( r.element_count );
                if ( !checked_mul( total, count, total ) ) {
                    fail_at( sub_node->die_offset,
                             "array size overflow" );
                }
            }
            return total;
        }
        default:
            fail_at( node->die_offset,
                     "DIE of this kind has no byte size" );
        }
    }

    //--------------------------------------------------------------------------
    void compute_value_reachability()
    {
        // BFS from the root following every edge except pointer->pointee.
        // A node reached this way must have a complete by-value layout;
        // pointer-only pointees are not required to be sized.
        std::vector<const type_node*> queue;
        queue.push_back( graph.root );
        value_reachable.insert( graph.root->die_offset );
        for ( std::size_t head = 0; head < queue.size(); ++head ) {
            const type_node* node = queue[head];
            for ( const type_node* next : value_targets( node ) ) {
                if ( value_reachable.insert( next->die_offset ).second ) {
                    queue.push_back( next );
                }
            }
        }
    }

    //--------------------------------------------------------------------------
    void check_value_cycles()
    {
        // Explicit DFS coloring catches cycles even when every DIE carries an
        // explicit byte_size, in which case size resolution never recurses.
        enum color : unsigned char
        {
            white = 0,
            gray  = 1,
            black = 2
        };
        std::unordered_map<std::uint64_t, unsigned char> colors;

        std::vector<const type_node*> stack;
        stack.push_back( graph.root );
        while ( !stack.empty() ) {
            const type_node* node = stack.back();
            auto it = colors.find( node->die_offset );
            if ( it == colors.end() ) {
                colors.emplace( node->die_offset, gray );
                for ( const type_node* next : value_targets( node ) ) {
                    auto cit = colors.find( next->die_offset );
                    if ( cit != colors.end() && cit->second == gray ) {
                        throw layout_failure{
                            { info_sec.name, next->die_offset,
                              "value-sized type forms a reference cycle",
                              false } };
                    }
                    if ( cit == colors.end() ) {
                        stack.push_back( next );
                    }
                }
            }
            else {
                if ( it->second == gray ) {
                    it->second = black;
                }
                stack.pop_back();
            }
        }
    }

    std::vector<const type_node*>
    value_targets( const type_node* node ) const
    {
        std::vector<const type_node*> targets;
        auto add = [&]( const type_node* t ) {
            if ( t != nullptr ) {
                targets.push_back( t );
            }
        };

        if ( node->kind == type_kind::structure_type ||
             node->kind == type_kind::union_type ) {
            for ( const type_node* member : node->members ) {
                add( member );
            }
        }
        if ( node->kind != type_kind::pointer_type ) {
            add( node->type );
        }
        if ( node->kind == type_kind::array_type ) {
            for ( const type_node* sub : node->subranges ) {
                add( sub );
            }
        }
        return targets;
    }

    //--------------------------------------------------------------------------
    void validate_containers()
    {
        for ( const auto& owned : graph.nodes ) {
            type_node* container = owned.get();
            if ( container->kind != type_kind::structure_type &&
                 container->kind != type_kind::union_type ) {
                continue;
            }
            // Pointer-only pointees are never laid out by value.
            if ( value_reachable.count( container->die_offset ) == 0 ) {
                continue;
            }
            bool is_struct =
                container->kind == type_kind::structure_type;
            std::uint64_t container_size = resolve_size( container );
            std::uint64_t container_bits;
            if ( !checked_mul( container_size, 8, container_bits ) ) {
                fail_at( container->die_offset, "container size overflow" );
            }

            for ( const type_node* mp : container->members ) {
                type_node* member = const_cast<type_node*>( mp );
                member_layout_info& mi = member->member_info;
                mi.storage_byte_size   = resolve_size( member );

                if ( mi.is_bitfield ) {
                    resolve_bitfield( member, is_struct,
                                      container_bits );
                    continue;
                }

                if ( !mi.has_byte_offset ) {
                    if ( is_struct ) {
                        fail_at( member->die_offset,
                                 "structure member lacks "
                                 "data_member_location" );
                    }
                    // Union members without an explicit location start at 0.
                    mi.has_byte_offset = true;
                    mi.byte_offset     = 0;
                }

                // Union members may overlap each other, but no member may
                // reach past the container's own storage.
                std::uint64_t end;
                if ( !checked_add( mi.byte_offset, mi.storage_byte_size,
                                   end ) ||
                     end > container_size ) {
                    fail_at( member->die_offset,
                             is_struct
                                 ? "member extends past structure size"
                                 : "member extends past union size" );
                }
                mi.bit_offset = mi.byte_offset * 8;
                mi.bit_width  = mi.storage_byte_size * 8;
            }
        }
    }

    void resolve_bitfield( type_node* member,
                           bool is_struct, std::uint64_t container_bits )
    {
        member_layout_info& mi = member->member_info;

        if ( !mi.has_data_bit_offset && !mi.has_legacy_bit_offset ) {
            fail_at( member->die_offset,
                     "bitfield lacks both data_bit_offset and bit_offset" );
        }

        std::uint64_t unified = 0;
        if ( mi.has_data_bit_offset ) {
            unified = mi.data_bit_offset;
        }

        if ( mi.has_legacy_bit_offset ) {
            // Legacy bitfields need the storage unit's byte start. Union
            // members default that start to zero.
            std::uint64_t byte_start;
            if ( mi.has_byte_offset ) {
                byte_start = mi.byte_offset;
            }
            else if ( !is_struct ) {
                mi.has_byte_offset = true;
                mi.byte_offset     = 0;
                byte_start         = 0;
            }
            else {
                fail_at( member->die_offset,
                         "legacy bitfield in a structure lacks "
                         "data_member_location" );
            }

            std::uint64_t storage_bits;
            if ( !checked_mul( mi.storage_byte_size, 8, storage_bits ) ) {
                fail_at( member->die_offset, "storage size overflow" );
            }
            std::uint64_t field_end;
            if ( !checked_add( mi.legacy_bit_offset, mi.bit_size,
                               field_end ) ||
                 field_end > storage_bits ) {
                fail_at( member->die_offset,
                         "bitfield exceeds its storage unit" );
            }
            std::uint64_t base_bits;
            if ( !checked_mul( byte_start, 8, base_bits ) ) {
                fail_at( member->die_offset, "byte offset overflow" );
            }
            std::uint64_t legacy_unified;
            if ( big_endian ) {
                if ( !checked_add( base_bits, mi.legacy_bit_offset,
                                   legacy_unified ) ) {
                    fail_at( member->die_offset, "bit offset overflow" );
                }
            }
            else {
                std::uint64_t tail =
                    storage_bits - mi.legacy_bit_offset - mi.bit_size;
                if ( !checked_add( base_bits, tail, legacy_unified ) ) {
                    fail_at( member->die_offset, "bit offset overflow" );
                }
            }
            if ( mi.has_data_bit_offset &&
                 legacy_unified != mi.data_bit_offset ) {
                fail_at( member->die_offset,
                         "data_bit_offset disagrees with bit_offset" );
            }
            unified = legacy_unified;
        }

        mi.bit_offset = unified;
        mi.bit_width  = mi.bit_size;

        // Members of a union may overlap each other, but no member may reach
        // past the union's own storage.
        std::uint64_t end;
        if ( !checked_add( mi.bit_offset, mi.bit_width, end ) ||
             end > container_bits ) {
            fail_at( member->die_offset,
                     "bitfield extends past container size" );
        }
    }

    //--------------------------------------------------------------------------
    std::uint64_t unsigned_value( const raw_attr& ra,
                                  std::uint64_t die_pos ) const
    {
        if ( ra.is_signed ) {
            if ( ra.s < 0 ) {
                throw layout_failure{
                    { info_sec.name, die_pos,
                      "expected a non-negative integer attribute", false } };
            }
            return static_cast<std::uint64_t>( ra.s );
        }
        return ra.u;
    }

    std::int64_t signed_value( const raw_attr& ra,
                               std::uint64_t /*die_pos*/ ) const
    {
        return ra.is_signed ? ra.s : static_cast<std::int64_t>( ra.u );
    }

    std::string string_value( const raw_attr& ra,
                              std::uint64_t die_pos ) const
    {
        if ( !ra.is_string ) {
            throw layout_failure{
                { info_sec.name, die_pos,
                  "name attribute is not a string form", false } };
        }
        return ra.str;
    }

    std::uint64_t ref_value( const raw_attr& ra,
                             std::uint64_t die_pos ) const
    {
        if ( !ra.is_ref ) {
            throw layout_failure{
                { info_sec.name, die_pos,
                  "type attribute is not a reference form", false } };
        }
        return ra.u;
    }

    [[noreturn]] void fail_at( std::uint64_t offset,
                               const std::string& message ) const
    {
        throw layout_failure{
            { info_sec.name, offset, message, false } };
    }

    //--------------------------------------------------------------------------
    const elfio& reader;
    std::uint64_t unit_offset;
    std::uint64_t root_offset;

    section_bytes info_sec;
    section_bytes abbrev_sec;
    section_bytes str_sec;
    bool big_endian = false;

    std::uint32_t abbrev_offset = 0;
    std::uint8_t  address_size  = 0;
    std::uint64_t unit_end      = 0;
    std::uint64_t dies_start    = 0;

    std::unordered_map<unsigned, abbrev_decl> abbrevs;
    std::vector<raw_die> raw_dies;
    std::unordered_set<std::uint64_t> die_boundaries;

    type_graph graph;
    std::unordered_map<std::uint64_t, type_node*> node_by_offset;
    std::unordered_set<std::uint64_t> value_reachable;
    std::unordered_map<std::uint64_t, std::uint64_t> size_cache;
    std::unordered_set<std::uint64_t> in_progress;
};

} // namespace tlayout_detail

//------------------------------------------------------------------------------
//! Build an owning type graph from a root type DIE.
//!
//! \param reader      Loaded, read-only ELF image.
//! \param unit_offset Section offset of the compilation unit header in
//!                    .debug_info.
//! \param root_offset Section offset of the root type DIE.
//! \return A type_graph on success, or a type_layout_error carrying a
//!         section-relative position.
inline type_layout_result
build_type_layout( const elfio& reader, std::uint64_t unit_offset,
                   std::uint64_t root_offset )
{
    try {
        tlayout_detail::layout_builder builder( reader, unit_offset,
                                                root_offset );
        return builder.run();
    }
    catch ( const tlayout_detail::layout_failure& f ) {
        return f.error;
    }
    catch ( const type_layout_error& e ) {
        return e;
    }
}

} // namespace ELFIO

#endif // ELFIO_TYPE_LAYOUT_HPP
