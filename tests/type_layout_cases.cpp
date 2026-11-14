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

// Tests for the read-only DWARF4 type layout interface. Every fixture is a
// hand-built .debug_info/.debug_abbrev/.debug_str triple wrapped into an
// ELF file through ELFIO itself: no external debugger or compiler output is
// required.

#include <elfio/elfio.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace ELFIO;

namespace {

int g_failures = 0;

void test_unions_and_extras();

void check( bool condition, const std::string& message )
{
    if ( !condition ) {
        std::cout << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

using Bytes = std::vector<std::uint8_t>;

void put_u( Bytes& out, std::size_t at, std::uint64_t v, unsigned width,
            bool big_endian )
{
    for ( unsigned i = 0; i < width; ++i ) {
        unsigned shift = big_endian ? ( width - 1 - i ) * 8 : i * 8;
        out[at + i] = static_cast<std::uint8_t>( v >> shift );
    }
}

void append_u( Bytes& out, std::uint64_t v, unsigned width,
               bool big_endian )
{
    std::size_t at = out.size();
    out.resize( at + width );
    put_u( out, at, v, width, big_endian );
}

void put_uleb( Bytes& out, std::uint64_t v )
{
    do {
        std::uint8_t b = static_cast<std::uint8_t>( v & 0x7f );
        v >>= 7;
        if ( v != 0 ) {
            b |= 0x80;
        }
        out.push_back( b );
    } while ( v != 0 );
}

void put_sleb( Bytes& out, std::int64_t v )
{
    bool more = true;
    while ( more ) {
        std::uint8_t b = static_cast<std::uint8_t>( v & 0x7f );
        bool sign_bit  = ( b & 0x40 ) != 0;
        v >>= 7;
        if ( ( v == 0 && !sign_bit ) || ( v == -1 && sign_bit ) ) {
            more = false;
        }
        else {
            b |= 0x80;
        }
        out.push_back( b );
    }
}

void put_cstr( Bytes& out, const std::string& s )
{
    out.insert( out.end(), s.begin(), s.end() );
    out.push_back( 0 );
}

// Builds one DWARF32 version 4 compilation unit together with its private
// abbreviation table.
class UnitBuilder
{
  public:
    UnitBuilder( bool big_endian, std::uint8_t address_size )
        : be( big_endian ), addr_size( address_size )
    {
        body.assign( 11, 0 ); // unit header placeholder
        strtab.push_back( 0 ); // offset 0 is the empty string
    }

    unsigned add_abbrev(
        unsigned tag, bool children,
        const std::vector<std::pair<unsigned, unsigned>>& attrs )
    {
        unsigned code = next_code++;
        put_uleb( abbrev, code );
        put_uleb( abbrev, tag );
        abbrev.push_back( children ? 1 : 0 );
        for ( const auto& [attr, form] : attrs ) {
            put_uleb( abbrev, attr );
            put_uleb( abbrev, form );
        }
        put_uleb( abbrev, 0 );
        put_uleb( abbrev, 0 );
        return code;
    }

    std::uint64_t pos() const { return body.size(); }

    void enter_die( unsigned code ) { put_uleb( body, code ); }

    //! Emits a DIE and returns its true unit-relative start offset.
    std::uint64_t enter_die_marked( unsigned code )
    {
        std::uint64_t start = pos();
        put_uleb( body, code );
        return start;
    }

    void exit_die()
    {
        body.push_back( 0 ); // null DIE terminates the sibling chain
    }

    void attr_udata( unsigned form, std::uint64_t value )
    {
        switch ( form ) {
        case DW_FORM_data1:
            append_u( body, value, 1, be );
            break;
        case DW_FORM_data2:
            append_u( body, value, 2, be );
            break;
        case DW_FORM_data4:
            append_u( body, value, 4, be );
            break;
        case DW_FORM_data8:
            append_u( body, value, 8, be );
            break;
        case DW_FORM_udata:
            put_uleb( body, value );
            break;
        default:
            check( false, "test bug: bad unsigned form" );
        }
    }

    void attr_sdata( std::int64_t value ) { put_sleb( body, value ); }

    void attr_string( const std::string& value )
    {
        put_cstr( body, value );
    }

    std::uint32_t add_debug_str( const std::string& value )
    {
        std::uint32_t off = static_cast<std::uint32_t>( strtab.size() );
        put_cstr( strtab, value );
        return off;
    }

    void attr_strp( const std::string& value )
    {
        append_u( body, add_debug_str( value ), 4, be );
    }

    //! Emits a ref4 and returns the body position of the 4-byte slot so a
    //! forward reference can be patched later.
    std::size_t attr_ref4_placeholder()
    {
        std::size_t at = body.size();
        append_u( body, 0, 4, be );
        return at;
    }

    void attr_ref4( std::uint64_t unit_relative_target )
    {
        append_u( body, unit_relative_target, 4, be );
    }

    void patch_ref4( std::size_t at, std::uint64_t target )
    {
        put_u( body, at, target, 4, be );
    }

    void append_raw( std::uint8_t byte ) { body.push_back( byte ); }

    void finish_abbrev()
    {
        if ( !abbrev_closed ) {
            abbrev.push_back( 0 ); // zero code ends the abbreviation table
            abbrev_closed = true;
        }
    }

    Bytes finalize( std::uint32_t abbrev_offset ) const
    {
        Bytes out = body;
        std::uint32_t unit_length =
            static_cast<std::uint32_t>( out.size() - 4 );
        // Header: unit_length(4) version(2) abbrev_offset(4) addr_size(1)
        put_u( out, 0, unit_length, 4, be );
        put_u( out, 4, 4, 2, be ); // DWARF version 4
        put_u( out, 6, abbrev_offset, 4, be );
        out[10] = addr_size;
        return out;
    }

    Bytes abbrev_bytes()
    {
        finish_abbrev();
        return abbrev;
    }
    Bytes strtab_bytes() const { return strtab; }

  private:
    bool be;
    std::uint8_t addr_size;
    unsigned next_code = 1;
    bool abbrev_closed = false;
    Bytes body;
    Bytes abbrev;
    Bytes strtab;
};

// Wraps section bytes into an in-memory ELF and reloads it read-only.
std::unique_ptr<elfio> make_elf( bool big_endian, const Bytes& info,
                                 const Bytes& abbrev, const Bytes& str )
{
    elfio writer;
    writer.create( ELFCLASS64,
                   big_endian ? ELFDATA2MSB : ELFDATA2LSB );
    writer.set_type( ET_EXEC );
    writer.set_machine( EM_X86_64 );

    auto add = [&]( const char* name, const Bytes& data ) {
        section* sec = writer.sections.add( name );
        sec->set_type( SHT_PROGBITS );
        sec->set_data( reinterpret_cast<const char*>( data.data() ),
                       data.size() );
    };
    add( ".debug_info", info );
    add( ".debug_abbrev", abbrev );
    add( ".debug_str", str );

    auto stream = std::make_unique<std::stringstream>(
        std::ios::in | std::ios::out | std::ios::binary );
    check( writer.save( *stream ), "failed to synthesize ELF" );

    auto reader = std::make_unique<elfio>();
    check( reader->load( *stream ), "failed to reload synthesized ELF" );
    return reader;
}

const type_layout_error* expect_error( const type_layout_result& result )
{
    if ( !std::holds_alternative<type_layout_error>( result ) ) {
        check( false, "expected an error but got a type graph" );
        return nullptr;
    }
    return &std::get<type_layout_error>( result );
}

const type_graph* expect_graph( const type_layout_result& result )
{
    if ( !std::holds_alternative<type_graph>( result ) ) {
        const auto& e = std::get<type_layout_error>( result );
        check( false, "expected a graph but got error: " + e.message +
                          " at " + e.section + "+0x" +
                          std::to_string( e.offset ) );
        return nullptr;
    }
    return &std::get<type_graph>( result );
}

//------------------------------------------------------------------------------
// Two compilation units in one section, each with its own abbreviation
// table. Also covers pointer sizing from the unit header and bad references.
void test_units()
{
    UnitBuilder u0( false, 8 );
    unsigned cu0      = u0.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned ab_base0 = u0.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned ab_struct0 = u0.add_abbrev(
        DW_TAG_structure_type, true,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data2 } } );
    unsigned ab_member0 = u0.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_data_member_location, DW_FORM_data1 },
          { DW_AT_type, DW_FORM_ref4 } } );
    unsigned ab_ptr0 = u0.add_abbrev(
        DW_TAG_pointer_type, false, { { DW_AT_type, DW_FORM_ref4 } } );

    u0.enter_die( cu0 );
    std::uint64_t int0 = u0.enter_die_marked( ab_base0 );
    u0.attr_string( "int" );
    u0.attr_udata( DW_FORM_data1, 4 );

    std::uint64_t ptr0 = u0.enter_die_marked( ab_ptr0 );
    u0.attr_ref4( int0 );

    std::uint64_t point0 = u0.enter_die_marked( ab_struct0 );
    u0.attr_string( "Point" );
    u0.attr_udata( DW_FORM_data2, 8 );
    u0.enter_die( ab_member0 );
    u0.attr_string( "x" );
    u0.attr_udata( DW_FORM_data1, 0 );
    u0.attr_ref4( int0 );
    u0.enter_die( ab_member0 );
    u0.attr_string( "y" );
    u0.attr_udata( DW_FORM_data1, 4 );
    u0.attr_ref4( int0 );
    u0.exit_die(); // struct
    u0.exit_die(); // CU

    // Second unit: address width 4, its own abbreviation table at a
    // different .debug_abbrev offset.
    UnitBuilder u1( false, 4 );
    unsigned cu1      = u1.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned ab_base1 = u1.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_strp },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned ab_struct1 = u1.add_abbrev(
        DW_TAG_structure_type, true,
        { { DW_AT_name, DW_FORM_strp },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned ab_mem1 = u1.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_data_member_location, DW_FORM_data1 },
          { DW_AT_type, DW_FORM_ref4 } } );
    unsigned ab_ptr1 = u1.add_abbrev(
        DW_TAG_pointer_type, false, { { DW_AT_type, DW_FORM_ref4 } } );

    u1.enter_die( cu1 );
    std::uint64_t char1 = u1.enter_die_marked( ab_base1 );
    u1.attr_strp( "char" );
    u1.attr_udata( DW_FORM_data1, 1 );

    std::uint64_t ptr1 = u1.enter_die_marked( ab_ptr1 );
    u1.attr_ref4( char1 );

    std::uint64_t tiny1 = u1.enter_die_marked( ab_struct1 );
    u1.attr_strp( "Tiny" );
    u1.attr_udata( DW_FORM_data1, 1 );
    u1.enter_die( ab_mem1 );
    u1.attr_string( "c" );
    u1.attr_udata( DW_FORM_data1, 0 );
    u1.attr_ref4( char1 );
    u1.exit_die();
    u1.exit_die();

    Bytes abbrev = u0.abbrev_bytes();
    std::uint32_t abbrev_off1 =
        static_cast<std::uint32_t>( abbrev.size() );
    abbrev.insert( abbrev.end(), u1.abbrev_bytes().begin(),
                   u1.abbrev_bytes().end() );

    Bytes info = u0.finalize( 0 );
    std::uint64_t unit1_offset = info.size();
    Bytes tail                = u1.finalize( abbrev_off1 );
    info.insert( info.end(), tail.begin(), tail.end() );

    auto elf = make_elf( false, info, abbrev, u1.strtab_bytes() );

    // Unit 0 graph.
    auto r0 = build_type_layout( *elf, 0, point0 );
    const type_graph* g0 = expect_graph( r0 );
    if ( g0 != nullptr ) {
        check( g0->root->kind == type_kind::structure_type,
               "unit0 root is a structure" );
        check( g0->root->name == "Point", "unit0 root name" );
        check( g0->root->has_byte_size && g0->root->byte_size == 8,
               "unit0 root size" );
        check( g0->root->members.size() == 2, "unit0 member count" );
        check( g0->root->members[0]->name == "x" &&
                   g0->root->members[1]->name == "y",
               "unit0 member order and names" );
        check( g0->root->members[0]->member_info.byte_offset == 0 &&
                   g0->root->members[1]->member_info.byte_offset == 4,
               "unit0 member locations" );
        check( g0->root->members[0]->type ==
                   g0->root->members[1]->type,
               "both members share one base type node" );
        check( g0->root->members[0]->type->name == "int" &&
                   g0->root->members[0]->type->byte_size == 4,
               "shared base type layout" );
    }

    // The pointer DIE is a CU sibling, so root the layout at it directly:
    // its size must come from unit 0's 8-byte address width.
    auto rs0 = build_type_layout( *elf, 0, ptr0 );
    const type_graph* gs0 = expect_graph( rs0 );
    if ( gs0 != nullptr ) {
        check( gs0->root->kind == type_kind::pointer_type,
               "pointer node kind" );
        check( gs0->root->byte_size == 8,
               "unit0 pointer defaults to 8-byte address width" );
        check( gs0->root->type != nullptr &&
                   gs0->root->type->name == "int",
               "pointer keeps its pointee reference" );
    }

    // Unit 1 graph: ref4 is relative to unit 1's own start.
    auto r1 = build_type_layout( *elf, unit1_offset,
                                 unit1_offset + tiny1 );
    const type_graph* g1 = expect_graph( r1 );
    if ( g1 != nullptr ) {
        check( g1->root->name == "Tiny" && g1->root->byte_size == 1,
               "unit1 root parsed through its own abbrev table" );
        check( g1->root->members.size() == 1 &&
                   g1->root->members[0]->type->name == "char",
               "unit1 strp name resolves against .debug_str" );
        auto rs1 = build_type_layout( *elf, unit1_offset,
                                      unit1_offset + ptr1 );
        const type_graph* gs1 = expect_graph( rs1 );
        check( gs1 != nullptr && gs1->root->byte_size == 4,
               "unit1 pointer defaults to 4-byte address width" );
    }

    // Bad reference: ref4 landing inside another DIE's attribute bytes.
    UnitBuilder bad( false, 8 );
    unsigned bcu   = bad.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned bbase = bad.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned bstruct = bad.add_abbrev(
        DW_TAG_structure_type, true,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned bmem = bad.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_data_member_location, DW_FORM_data1 },
          { DW_AT_type, DW_FORM_ref4 } } );
    bad.enter_die( bcu );
    std::uint64_t bint = bad.enter_die_marked( bbase );
    bad.attr_string( "int" );
    bad.attr_udata( DW_FORM_data1, 4 );
    std::uint64_t broot = bad.enter_die_marked( bstruct );
    bad.attr_string( "Bad" );
    bad.attr_udata( DW_FORM_data1, 4 );
    bad.enter_die( bmem );
    bad.attr_string( "m" );
    bad.attr_udata( DW_FORM_data1, 0 );
    bad.attr_ref4( bint + 2 ); // points at name bytes, not a DIE boundary
    bad.exit_die();
    bad.exit_die();

    auto belf = make_elf( false, bad.finalize( 0 ),
                          bad.abbrev_bytes(), bad.strtab_bytes() );
    auto br  = build_type_layout( *belf, 0, broot );
    const type_layout_error* be = expect_error( br );
    check( be != nullptr && be->section == ".debug_info",
           "interior reference reports a .debug_info position" );

    // Cross-unit reference: rel4 points past the end of the unit.
    UnitBuilder cross( false, 8 );
    unsigned ccu = cross.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned cstruct = cross.add_abbrev(
        DW_TAG_structure_type, true,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned cmem = cross.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_data_member_location, DW_FORM_data1 },
          { DW_AT_type, DW_FORM_ref4 } } );
    cross.enter_die( ccu );
    std::uint64_t croot = cross.enter_die_marked( cstruct );
    cross.attr_string( "Cross" );
    cross.attr_udata( DW_FORM_data1, 4 );
    cross.enter_die( cmem );
    cross.attr_string( "m" );
    cross.attr_udata( DW_FORM_data1, 0 );
    cross.attr_ref4( 0x10000 ); // outside this unit
    cross.exit_die();
    cross.exit_die();

    auto celf = make_elf( false, cross.finalize( 0 ),
                          cross.abbrev_bytes(), cross.strtab_bytes() );
    auto cr  = build_type_layout( *celf, 0, croot );
    const type_layout_error* ce = expect_error( cr );
    check( ce != nullptr && !ce->message.empty(),
           "cross-unit reference is rejected with a located error" );
}

//------------------------------------------------------------------------------
// Pointer self-reference keeps one shared node; by-value self-containment is
// rejected; void pointers stay null-typed.
void test_recursion()
{
    UnitBuilder u( false, 8 );
    unsigned cu     = u.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned a_base = u.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned a_struct = u.add_abbrev(
        DW_TAG_structure_type, true,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data2 } } );
    unsigned a_member = u.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_data_member_location, DW_FORM_data1 },
          { DW_AT_type, DW_FORM_ref4 } } );
    unsigned a_ptr = u.add_abbrev(
        DW_TAG_pointer_type, false, { { DW_AT_type, DW_FORM_ref4 } } );
    unsigned a_void_ptr =
        u.add_abbrev( DW_TAG_pointer_type, false, {} );

    u.enter_die( cu );
    std::uint64_t int_off = u.enter_die_marked( a_base );
    u.attr_string( "int" );
    u.attr_udata( DW_FORM_data1, 4 );

    // Pointer DIEs are CU children. They may use forward references to the
    // not-yet-emitted structure DIE.
    std::uint64_t next_ptr = u.enter_die_marked( a_ptr );
    std::size_t next_ptr_slot = u.attr_ref4_placeholder();

    std::uint64_t void_off = u.enter_die_marked( a_void_ptr );

    std::uint64_t node_off = u.enter_die_marked( a_struct );
    u.patch_ref4( next_ptr_slot, node_off );
    u.attr_string( "Node" );
    u.attr_udata( DW_FORM_data2, 16 );
    u.enter_die( a_member );
    u.attr_string( "value" );
    u.attr_udata( DW_FORM_data1, 0 );
    u.attr_ref4( int_off );
    std::uint64_t next_member = u.enter_die_marked( a_member );
    u.attr_string( "next" );
    u.attr_udata( DW_FORM_data1, 8 );
    u.attr_ref4( next_ptr );
    u.exit_die(); // Node
    u.exit_die(); // CU

    auto elf = make_elf( false, u.finalize( 0 ), u.abbrev_bytes(),
                         u.strtab_bytes() );
    auto res = build_type_layout( *elf, 0, node_off );
    const type_graph* g = expect_graph( res );
    if ( g != nullptr ) {
        check( g->root->die_offset == node_off, "recursion root identity" );
        check( g->root->members.size() == 2, "node member count" );
        const type_node* next = g->root->members[1];
        check( next->die_offset == next_member, "next member identity" );
        check( next->type != nullptr &&
                   next->type->kind == type_kind::pointer_type,
               "next member is a pointer node" );
        check( next->type->die_offset == next_ptr,
               "pointer DIE identity preserved" );
        check( next->type->type == g->root,
               "pointer target is the same shared struct node" );
        check( next->type->byte_size == 8,
               "pointer size resolved from address width" );
        check( g->root->members[0]->type->name == "int",
               "value member still resolved by value" );

        // The void pointer is a CU sibling, reached through its own root.
        auto vr = build_type_layout( *elf, 0, void_off );
        const type_graph* vg = expect_graph( vr );
        check( vg != nullptr && vg->root->type == nullptr,
               "pointer without DW_AT_type is a void pointer" );
        check( vg != nullptr && vg->root->byte_size == 8,
               "void pointer still takes the address width" );
    }

    // Pointer to an incomplete (declaration-only) struct: the graph must
    // still build, and nothing may invent a size for the pointee.
    UnitBuilder inc( false, 8 );
    unsigned icu = inc.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned iptr = inc.add_abbrev(
        DW_TAG_pointer_type, false, { { DW_AT_type, DW_FORM_ref4 } } );
    unsigned idecl = inc.add_abbrev(
        DW_TAG_structure_type, false,
        { { DW_AT_name, DW_FORM_string },
          { 0x00, DW_FORM_flag_present } } ); // DW_AT_declaration
    inc.enter_die( icu );
    std::uint64_t ip = inc.enter_die_marked( iptr );
    std::size_t ip_slot = inc.attr_ref4_placeholder();
    std::uint64_t istruct = inc.enter_die_marked( idecl );
    inc.patch_ref4( ip_slot, istruct );
    inc.attr_string( "Opaque" );
    inc.append_raw( 0 ); // flag_present carries no data
    inc.exit_die(); // CU
    auto ielf = make_elf( false, inc.finalize( 0 ),
                          inc.abbrev_bytes(), inc.strtab_bytes() );
    auto ir = build_type_layout( *ielf, 0, ip );
    const type_graph* ig = expect_graph( ir );
    if ( ig != nullptr ) {
        check( ig->root->kind == type_kind::pointer_type &&
                   ig->root->byte_size == 8,
               "pointer to incomplete type still takes address width" );
        check( ig->root->type != nullptr &&
                   ig->root->type->name == "Opaque" &&
                   !ig->root->type->has_byte_size,
               "incomplete pointee keeps identity without a fake size" );
    }

    // By-value cycle in its own unit.
    UnitBuilder cyc( false, 8 );
    unsigned ccu = cyc.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned cstruct = cyc.add_abbrev(
        DW_TAG_structure_type, true,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned cmem = cyc.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_data_member_location, DW_FORM_data1 },
          { DW_AT_type, DW_FORM_ref4 } } );
    cyc.enter_die( ccu );
    std::uint64_t bad_root = cyc.enter_die_marked( cstruct );
    cyc.attr_string( "Self" );
    cyc.attr_udata( DW_FORM_data1, 8 );
    cyc.enter_die( cmem );
    cyc.attr_string( "self" );
    cyc.attr_udata( DW_FORM_data1, 0 );
    cyc.attr_ref4( bad_root ); // direct by-value self containment
    cyc.exit_die();
    cyc.exit_die();

    auto celf = make_elf( false, cyc.finalize( 0 ),
                          cyc.abbrev_bytes(), cyc.strtab_bytes() );
    auto cr  = build_type_layout( *celf, 0, bad_root );
    const type_layout_error* ce = expect_error( cr );
    check( ce != nullptr && ce->section == ".debug_info",
           "by-value cycle is a located error" );
}

//------------------------------------------------------------------------------
// Nested arrays, bounds inference and bitfields in both endians.
void test_layout()
{
    for ( bool be : { false, true } ) {
        UnitBuilder u( be, 8 );
        unsigned cu = u.add_abbrev( DW_TAG_compile_unit, true, {} );
        unsigned ab_base = u.add_abbrev(
            DW_TAG_base_type, false,
            { { DW_AT_name, DW_FORM_string },
              { DW_AT_byte_size, DW_FORM_data1 } } );
        unsigned ab_array = u.add_abbrev(
            DW_TAG_array_type, true,
            { { DW_AT_type, DW_FORM_ref4 } } );
        unsigned ab_count = u.add_abbrev(
            DW_TAG_subrange_type, false,
            { { DW_AT_count, DW_FORM_data1 } } );
        unsigned ab_bounds = u.add_abbrev(
            DW_TAG_subrange_type, false,
            { { DW_AT_lower_bound, DW_FORM_sdata },
              { DW_AT_upper_bound, DW_FORM_sdata } } );
        unsigned ab_count_bounds = u.add_abbrev(
            DW_TAG_subrange_type, false,
            { { DW_AT_count, DW_FORM_data1 },
              { DW_AT_lower_bound, DW_FORM_sdata },
              { DW_AT_upper_bound, DW_FORM_sdata } } );
        unsigned ab_struct = u.add_abbrev(
            DW_TAG_structure_type, true,
            { { DW_AT_name, DW_FORM_string },
              { DW_AT_byte_size, DW_FORM_data2 } } );
        unsigned ab_plain = u.add_abbrev(
            DW_TAG_member, false,
            { { DW_AT_name, DW_FORM_string },
              { DW_AT_data_member_location, DW_FORM_data1 },
              { DW_AT_type, DW_FORM_ref4 } } );
        unsigned ab_modern = u.add_abbrev(
            DW_TAG_member, false,
            { { DW_AT_name, DW_FORM_string },
              { DW_AT_data_bit_offset, DW_FORM_data1 },
              { DW_AT_bit_size, DW_FORM_data1 },
              { DW_AT_byte_size, DW_FORM_data1 },
              { DW_AT_type, DW_FORM_ref4 } } );
        unsigned ab_legacy = u.add_abbrev(
            DW_TAG_member, false,
            { { DW_AT_name, DW_FORM_string },
              { DW_AT_data_member_location, DW_FORM_data1 },
              { DW_AT_bit_offset, DW_FORM_data1 },
              { DW_AT_bit_size, DW_FORM_data1 },
              { DW_AT_byte_size, DW_FORM_data1 },
              { DW_AT_type, DW_FORM_ref4 } } );
        // Unrelated DIE whose forms are all supported: it must be skipped
        // structurally rather than found by byte scanning.
        unsigned ab_unrelated = u.add_abbrev(
            0x34, false, // DW_TAG_variable, outside the supported tag set
            { { DW_AT_name, DW_FORM_string } } );

        u.enter_die( cu );
        std::uint64_t i32 = u.enter_die_marked( ab_base );
        u.attr_string( "int" );
        u.attr_udata( DW_FORM_data1, 4 );
        std::uint64_t u8 = u.enter_die_marked( ab_base );
        u.attr_string( "u8" );
        u.attr_udata( DW_FORM_data1, 1 );

        u.enter_die( ab_unrelated );
        u.attr_string( "skipped_variable" );

        // int[2][3]: first dimension by count, second by bounds 1..3.
        std::uint64_t arr = u.enter_die_marked( ab_array );
        u.attr_ref4( i32 );
        u.enter_die( ab_count );
        u.attr_udata( DW_FORM_data1, 2 );
        u.enter_die( ab_bounds );
        u.attr_sdata( 1 );
        u.attr_sdata( 3 );
        u.exit_die(); // array

        // Matching count and bounds.
        std::uint64_t arr_ok = u.enter_die_marked( ab_array );
        u.attr_ref4( i32 );
        u.enter_die( ab_count_bounds );
        u.attr_udata( DW_FORM_data1, 3 );
        u.attr_sdata( 0 );
        u.attr_sdata( 2 );
        u.exit_die();

        // Bitfield container, two bytes large.
        std::uint64_t bits = u.enter_die_marked( ab_struct );
        u.attr_string( "Bits" );
        u.attr_udata( DW_FORM_data2, 2 );

        std::uint64_t modern = u.enter_die_marked( ab_modern );
        u.attr_string( "modern" );
        u.attr_udata( DW_FORM_data1, 2 ); // data_bit_offset
        u.attr_udata( DW_FORM_data1, 3 ); // bit_size
        u.attr_udata( DW_FORM_data1, 1 ); // storage byte size
        u.attr_ref4( u8 );

        std::uint64_t legacy0 = u.enter_die_marked( ab_legacy );
        u.attr_string( "legacy0" );
        u.attr_udata( DW_FORM_data1, 0 ); // member byte offset
        u.attr_udata( DW_FORM_data1, 2 ); // bit_offset
        u.attr_udata( DW_FORM_data1, 3 ); // bit_size
        u.attr_udata( DW_FORM_data1, 1 ); // storage byte size
        u.attr_ref4( u8 );

        std::uint64_t legacy1 = u.enter_die_marked( ab_legacy );
        u.attr_string( "legacy1" );
        u.attr_udata( DW_FORM_data1, 1 ); // member byte offset
        u.attr_udata( DW_FORM_data1, 0 ); // bit_offset
        u.attr_udata( DW_FORM_data1, 4 ); // bit_size
        u.attr_udata( DW_FORM_data1, 1 ); // storage byte size
        u.attr_ref4( u8 );
        u.exit_die(); // Bits

        // Plain members with a strict size boundary.
        std::uint64_t plain = u.enter_die_marked( ab_struct );
        u.attr_string( "Plain" );
        u.attr_udata( DW_FORM_data2, 8 );
        u.enter_die( ab_plain );
        u.attr_string( "a" );
        u.attr_udata( DW_FORM_data1, 0 );
        u.attr_ref4( i32 );
        u.enter_die( ab_plain );
        u.attr_string( "b" );
        u.attr_udata( DW_FORM_data1, 4 );
        u.attr_ref4( u8 );
        u.exit_die();

        u.exit_die(); // CU

        auto elf = make_elf( be, u.finalize( 0 ), u.abbrev_bytes(),
                             u.strtab_bytes() );

        // Nested arrays.
        auto ar = build_type_layout( *elf, 0, arr );
        const type_graph* ag = expect_graph( ar );
        if ( ag != nullptr ) {
            check( ag->root->kind == type_kind::array_type,
                   "array root kind" );
            check( ag->root->subranges.size() == 2,
                   "nested array keeps both subranges in order" );
            const subrange_info& r0 = ag->root->subranges[0]->range;
            const subrange_info& r1 = ag->root->subranges[1]->range;
            check( r0.has_count && r0.element_count == 2,
                   "first dimension from count" );
            check( !r1.has_count && r1.has_lower_bound &&
                       r1.has_upper_bound && r1.lower_bound == 1 &&
                       r1.upper_bound == 3 && r1.element_count == 3,
                   "second dimension inferred from signed bounds" );
            check( ag->root->type->die_offset == i32,
                   "array element type reference" );
            check( ag->root->byte_size == 24,
                   "nested array size is 2*3*4" );
        }
        auto aok = build_type_layout( *elf, 0, arr_ok );
        check( std::holds_alternative<type_graph>( aok ),
               "matching count and bounds accepted" );

        // Bitfields.
        auto br = build_type_layout( *elf, 0, bits );
        const type_graph* bg = expect_graph( br );
        if ( bg != nullptr ) {
            check( bg->root->members.size() == 3,
                   "bitfield members preserved in order" );
            const type_node* m  = bg->root->members[0];
            const type_node* l0 = bg->root->members[1];
            const type_node* l1 = bg->root->members[2];

            check( m->die_offset == modern &&
                       m->member_info.has_data_bit_offset &&
                       m->member_info.data_bit_offset == 2 &&
                       m->member_info.bit_size == 3,
                   "modern raw attributes retained" );
            check( m->member_info.bit_offset == 2 &&
                       m->member_info.bit_width == 3,
                   "modern unified bit placement" );

            check( l0->die_offset == legacy0 &&
                       l0->member_info.has_legacy_bit_offset &&
                       l0->member_info.legacy_bit_offset == 2 &&
                       l0->member_info.storage_byte_size == 1,
                   "legacy raw attributes retained" );
            std::uint64_t want0 = be ? 2 : 3;
            check( l0->member_info.bit_offset == want0 &&
                       l0->member_info.bit_width == 3,
                   "legacy byte-0 bitfield follows endian rule" );

            std::uint64_t want1 = be ? 8 : 12;
            check( l1->die_offset == legacy1 &&
                       l1->member_info.bit_offset == want1 &&
                       l1->member_info.bit_width == 4,
                   "legacy byte-1 bitfield follows endian rule" );
            check( l1->member_info.byte_offset == 1,
                   "legacy storage unit byte start kept" );
        }

        // Plain layout.
        auto pr = build_type_layout( *elf, 0, plain );
        const type_graph* pg = expect_graph( pr );
        if ( pg != nullptr ) {
            check( pg->root->members[0]->member_info.byte_offset == 0 &&
                       pg->root->members[1]->member_info.byte_offset == 4,
                   "plain member offsets" );
            check( pg->root->members[0]->member_info.bit_width == 32 &&
                       pg->root->members[1]->member_info.bit_width == 8,
                   "plain member widths" );
        }
    }

    // Member reaching past the structure size.
    UnitBuilder over( false, 8 );
    unsigned ocu = over.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned obase = over.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned ostruct = over.add_abbrev(
        DW_TAG_structure_type, true,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned omem = over.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_data_member_location, DW_FORM_data1 },
          { DW_AT_type, DW_FORM_ref4 } } );
    over.enter_die( ocu );
    std::uint64_t oint = over.enter_die_marked( obase );
    over.attr_string( "int" );
    over.attr_udata( DW_FORM_data1, 4 );
    std::uint64_t oroot = over.enter_die_marked( ostruct );
    over.attr_string( "Over" );
    over.attr_udata( DW_FORM_data1, 4 );
    over.enter_die( omem );
    over.attr_string( "m" );
    over.attr_udata( DW_FORM_data1, 4 ); // 4 + 4 > 4
    over.attr_ref4( oint );
    over.exit_die();
    over.exit_die();
    auto oelf = make_elf( false, over.finalize( 0 ),
                          over.abbrev_bytes(), over.strtab_bytes() );
    check( std::holds_alternative<type_layout_error>(
               build_type_layout( *oelf, 0, oroot ) ),
           "member overflowing the structure is rejected" );

    // Count disagreeing with the bounds.
    UnitBuilder mm( false, 8 );
    unsigned mcu = mm.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned marray = mm.add_abbrev(
        DW_TAG_array_type, true, { { DW_AT_type, DW_FORM_ref4 } } );
    unsigned mbase = mm.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned msub = mm.add_abbrev(
        DW_TAG_subrange_type, false,
        { { DW_AT_count, DW_FORM_data1 },
          { DW_AT_lower_bound, DW_FORM_sdata },
          { DW_AT_upper_bound, DW_FORM_sdata } } );
    mm.enter_die( mcu );
    std::uint64_t mint = mm.enter_die_marked( mbase );
    mm.attr_string( "int" );
    mm.attr_udata( DW_FORM_data1, 4 );
    std::uint64_t marr = mm.enter_die_marked( marray );
    mm.attr_ref4( mint );
    mm.enter_die( msub );
    mm.attr_udata( DW_FORM_data1, 3 );
    mm.attr_sdata( 0 );
    mm.attr_sdata( 3 ); // span 4 disagrees with count 3
    mm.exit_die();
    mm.exit_die();
    auto melf = make_elf( false, mm.finalize( 0 ),
                          mm.abbrev_bytes(), mm.strtab_bytes() );
    check( std::holds_alternative<type_layout_error>(
               build_type_layout( *melf, 0, marr ) ),
           "count/bounds mismatch is rejected" );

    // Unsupported attribute form is reported as unsupported instead of
    // skipping an unknown number of bytes.
    UnitBuilder uf( false, 8 );
    unsigned ucu = uf.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned ubase = uf.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned ustruct = uf.add_abbrev(
        DW_TAG_structure_type, true,
        { { DW_AT_name, DW_FORM_string },
          { 0x99, 0x99 } } ); // unknown attr/form pair
    uf.enter_die( ucu );
    uf.enter_die( ubase );
    uf.attr_string( "int" );
    uf.attr_udata( DW_FORM_data1, 4 );
    std::uint64_t uroot = uf.enter_die_marked( ustruct );
    uf.attr_string( "U" );
    uf.append_raw( 0x11 ); // one byte belonging to the unknown form
    uf.exit_die();
    uf.exit_die();
    auto uelf = make_elf( false, uf.finalize( 0 ),
                          uf.abbrev_bytes(), uf.strtab_bytes() );
    auto ur  = build_type_layout( *uelf, 0, uroot );
    const type_layout_error* ue = expect_error( ur );
    check( ue != nullptr && ue->unsupported,
           "unsupported form is flagged explicitly" );

    test_unions_and_extras();
}

//------------------------------------------------------------------------------
// Unions, anonymous/duplicate members, qualified chains, ELF32 big-endian,
// malformed sections and overflow handling.
void test_unions_and_extras()
{
    // Union: overlapping members are fine; one member overflowing the union
    // must be rejected. Anonymous and same-named members are preserved.
    UnitBuilder u( false, 8 );
    unsigned cu = u.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned ab_base = u.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned ab_union = u.add_abbrev(
        DW_TAG_union_type, true,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned ab_anon_member = u.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_data_member_location, DW_FORM_data1 },
          { DW_AT_type, DW_FORM_ref4 } } );
    unsigned ab_named_member = u.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_type, DW_FORM_ref4 } } );
    unsigned ab_td = u.add_abbrev(
        DW_TAG_typedef, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_type, DW_FORM_ref4 } } );
    unsigned ab_const = u.add_abbrev(
        DW_TAG_const_type, false, { { DW_AT_type, DW_FORM_ref4 } } );
    unsigned ab_volatile = u.add_abbrev(
        DW_TAG_volatile_type, false, { { DW_AT_type, DW_FORM_ref4 } } );
    unsigned ab_both_offsets = u.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_data_member_location, DW_FORM_data1 },
          { DW_AT_data_bit_offset, DW_FORM_data1 },
          { DW_AT_bit_offset, DW_FORM_data1 },
          { DW_AT_bit_size, DW_FORM_data1 },
          { DW_AT_type, DW_FORM_ref4 } } );

    u.enter_die( cu );
    std::uint64_t i32 = u.enter_die_marked( ab_base );
    u.attr_string( "int" );
    u.attr_udata( DW_FORM_data1, 4 );
    std::uint64_t u8 = u.enter_die_marked( ab_base );
    u.attr_string( "u8" );
    u.attr_udata( DW_FORM_data1, 1 );

    // typedef -> const -> volatile -> int
    std::uint64_t td = u.enter_die_marked( ab_td );
    u.attr_string( "MyInt" );
    u.attr_ref4( i32 );
    std::uint64_t ct = u.enter_die_marked( ab_const );
    u.attr_ref4( td );
    std::uint64_t vt = u.enter_die_marked( ab_volatile );
    u.attr_ref4( ct );

    std::uint64_t uni = u.enter_die_marked( ab_union );
    u.attr_string( "U" );
    u.attr_udata( DW_FORM_data1, 4 );
    u.enter_die( ab_named_member ); // no location: union default zero
    u.attr_string( "dup" );
    u.attr_ref4( i32 );
    u.enter_die( ab_named_member ); // same name, later in order
    u.attr_string( "dup" );
    u.attr_ref4( u8 );
    u.enter_die( ab_anon_member );  // anonymous, explicit zero location
    u.attr_udata( DW_FORM_data1, 0 );
    u.attr_ref4( vt );
    // Modern and legacy offsets both present; little-endian legacy with
    // byte offset 0, bit_offset 1, bit_size 4 in a 1-byte storage unit is
    // 8-1-4 = 3, matching data_bit_offset 3.
    u.enter_die( ab_both_offsets );
    u.attr_string( "both" );
    u.attr_udata( DW_FORM_data1, 0 );
    u.attr_udata( DW_FORM_data1, 3 );
    u.attr_udata( DW_FORM_data1, 1 );
    u.attr_udata( DW_FORM_data1, 4 );
    u.attr_ref4( u8 );
    u.exit_die(); // union
    u.exit_die(); // CU

    auto elf = make_elf( false, u.finalize( 0 ), u.abbrev_bytes(),
                         u.strtab_bytes() );
    auto gr = build_type_layout( *elf, 0, uni );
    const type_graph* g = expect_graph( gr );
    if ( g != nullptr ) {
        check( g->root->kind == type_kind::union_type &&
                   g->root->byte_size == 4,
               "union root" );
        check( g->root->members.size() == 4,
               "union keeps every member including duplicates" );
        check( g->root->members[0]->name == "dup" &&
                   g->root->members[1]->name == "dup",
               "same-named members preserved in order" );
        check( g->root->members[2]->name.empty(),
               "anonymous member keeps an empty name" );
        for ( const type_node* m : g->root->members ) {
            check( m->member_info.byte_offset == 0,
                   "union members overlap at offset zero" );
        }
        check( g->root->members[3]->member_info.bit_offset == 3 &&
                   g->root->members[3]->member_info.has_data_bit_offset &&
                   g->root->members[3]->member_info
                           .has_legacy_bit_offset,
               "matching modern and legacy bit offsets accepted" );

        const type_node* anon_type = g->root->members[2]->type;
        check( anon_type->kind == type_kind::volatile_type &&
                   anon_type->type->kind == type_kind::const_type &&
                   anon_type->type->type->kind == type_kind::typedef_type &&
                   anon_type->type->type->type->die_offset == i32,
               "typedef and qualifier nodes kept in reference order" );
        check( anon_type->byte_size == 4 &&
                   anon_type->type->byte_size == 4 &&
                   anon_type->type->type->byte_size == 4,
               "sizes resolved through typedef and qualifiers" );
        (void)vt;
    }

    // Union member overflowing its container.
    UnitBuilder ov( false, 8 );
    unsigned ocu = ov.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned obase = ov.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned ounion = ov.add_abbrev(
        DW_TAG_union_type, true,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned omem = ov.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_type, DW_FORM_ref4 } } );
    ov.enter_die( ocu );
    std::uint64_t oint = ov.enter_die_marked( obase );
    ov.attr_string( "int" );
    ov.attr_udata( DW_FORM_data1, 4 );
    std::uint64_t oroot = ov.enter_die_marked( ounion );
    ov.attr_string( "Small" );
    ov.attr_udata( DW_FORM_data1, 1 );
    ov.enter_die( omem );
    ov.attr_string( "m" );
    ov.attr_ref4( oint );
    ov.exit_die();
    ov.exit_die();
    auto oelf = make_elf( false, ov.finalize( 0 ),
                          ov.abbrev_bytes(), ov.strtab_bytes() );
    check( std::holds_alternative<type_layout_error>(
               build_type_layout( *oelf, 0, oroot ) ),
           "union member larger than the union is rejected" );

    // ELF32 big-endian: byte order and address width come from the unit
    // header rather than the ELF class.
    UnitBuilder be32( true, 2 );
    unsigned bcu = be32.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned bbase = be32.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned bptr = be32.add_abbrev(
        DW_TAG_pointer_type, false, { { DW_AT_type, DW_FORM_ref4 } } );
    be32.enter_die( bcu );
    std::uint64_t bint = be32.enter_die_marked( bbase );
    be32.attr_string( "short" );
    be32.attr_udata( DW_FORM_data1, 2 );
    std::uint64_t bp = be32.enter_die_marked( bptr );
    be32.attr_ref4( bint );
    be32.exit_die();
    {
        elfio writer;
        writer.create( ELFCLASS32, ELFDATA2MSB );
        writer.set_type( ET_EXEC );
        writer.set_machine( EM_386 );
        Bytes info = be32.finalize( 0 );
        auto put_sec = [&]( const char* name, const Bytes& d ) {
            section* sec = writer.sections.add( name );
            sec->set_type( SHT_PROGBITS );
            sec->set_data( reinterpret_cast<const char*>( d.data() ),
                           d.size() );
        };
        put_sec( ".debug_info", info );
        put_sec( ".debug_abbrev", be32.abbrev_bytes() );
        put_sec( ".debug_str", be32.strtab_bytes() );
        std::stringstream stream(
            std::ios::in | std::ios::out | std::ios::binary );
        check( writer.save( stream ), "save ELF32 BE" );
        elfio reader;
        check( reader.load( stream ), "load ELF32 BE" );
        auto rr = build_type_layout( reader, 0, bp );
        const type_graph* bg = expect_graph( rr );
        check( bg != nullptr && bg->root->byte_size == 2,
               "big-endian ELF32 pointer uses 2-byte unit address width" );
        check( bg != nullptr && bg->root->type != nullptr &&
                   bg->root->type->byte_size == 2,
               "big-endian base type parsed correctly" );
    }

    // Abbreviation offset outside .debug_abbrev.
    UnitBuilder badab( false, 8 );
    unsigned acu = badab.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned abase2 = badab.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    badab.enter_die( acu );
    std::uint64_t aint = badab.enter_die_marked( abase2 );
    badab.attr_string( "int" );
    badab.attr_udata( DW_FORM_data1, 4 );
    badab.exit_die();
    {
        Bytes info = badab.finalize( 0xffffff );
        auto aelf = make_elf( false, info, badab.abbrev_bytes(),
                              badab.strtab_bytes() );
        auto ar = build_type_layout( *aelf, 0, aint );
        const type_layout_error* ae = expect_error( ar );
        check( ae != nullptr && ae->section == ".debug_abbrev",
               "bad abbreviation offset is a located error" );
    }

    // Unterminated FORM_string inside an otherwise bounded unit.
    {
        Bytes raw;
        raw.assign( 11, 0 );
        raw.push_back( 1 ); // abbrev code 1
        raw.push_back( 'x' ); // string with no NUL before unit end
        std::uint32_t length = static_cast<std::uint32_t>( raw.size() - 4 );
        for ( int i = 0; i < 4; ++i ) {
            raw[i] = static_cast<std::uint8_t>(
                length >> ( i * 8 ) ); // little endian
        }
        raw[5] = 4;
        raw[10] = 8;
        Bytes abbrev;
        put_uleb( abbrev, 1 );
        put_uleb( abbrev, DW_TAG_base_type );
        abbrev.push_back( 0 );
        put_uleb( abbrev, DW_AT_name );
        put_uleb( abbrev, DW_FORM_string );
        put_uleb( abbrev, 0 );
        put_uleb( abbrev, 0 );
        abbrev.push_back( 0 );
        Bytes str;
        str.push_back( 0 );
        auto self = make_elf( false, raw, abbrev, str );
        auto sr  = build_type_layout( *self, 0, 11 );
        const type_layout_error* se = expect_error( sr );
        check( se != nullptr && !se->message.empty(),
               "unterminated string is rejected with a position" );
    }

    // Array size overflow: huge element size times a non-trivial count.
    UnitBuilder of( false, 8 );
    unsigned ofcu = of.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned ofbase = of.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data8 } } );
    unsigned ofarr = of.add_abbrev(
        DW_TAG_array_type, true, { { DW_AT_type, DW_FORM_ref4 } } );
    unsigned ofsub = of.add_abbrev(
        DW_TAG_subrange_type, false,
        { { DW_AT_count, DW_FORM_data1 } } );
    of.enter_die( ofcu );
    std::uint64_t ofint = of.enter_die_marked( ofbase );
    of.attr_string( "huge" );
    of.attr_udata( DW_FORM_data8, 0xffffffffffffffffULL );
    std::uint64_t ofarr_off = of.enter_die_marked( ofarr );
    of.attr_ref4( ofint );
    of.enter_die( ofsub );
    of.attr_udata( DW_FORM_data1, 2 );
    of.exit_die();
    of.exit_die();
    auto ofelf = make_elf( false, of.finalize( 0 ),
                           of.abbrev_bytes(), of.strtab_bytes() );
    check( std::holds_alternative<type_layout_error>(
               build_type_layout( *ofelf, 0, ofarr_off ) ),
           "array size overflow is rejected" );

    // Missing byte size is never turned into a zero-length object.
    UnitBuilder ms( false, 8 );
    unsigned mcu = ms.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned mbase = ms.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned mstruct = ms.add_abbrev(
        DW_TAG_structure_type, true,
        { { DW_AT_name, DW_FORM_string } } );
    unsigned mmem2 = ms.add_abbrev(
        DW_TAG_member, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_data_member_location, DW_FORM_data1 },
          { DW_AT_type, DW_FORM_ref4 } } );
    ms.enter_die( mcu );
    std::uint64_t mint = ms.enter_die_marked( mbase );
    ms.attr_string( "int" );
    ms.attr_udata( DW_FORM_data1, 4 );
    std::uint64_t mroot = ms.enter_die_marked( mstruct );
    ms.attr_string( "NoSize" );
    ms.enter_die( mmem2 );
    ms.attr_string( "m" );
    ms.attr_udata( DW_FORM_data1, 0 );
    ms.attr_ref4( mint );
    ms.exit_die();
    ms.exit_die();
    auto mself = make_elf( false, ms.finalize( 0 ),
                           ms.abbrev_bytes(), ms.strtab_bytes() );
    check( std::holds_alternative<type_layout_error>(
               build_type_layout( *mself, 0, mroot ) ),
           "structure missing byte size is rejected, not zeroed" );

    // Unsupported construct affecting the type: an inheritance DIE inside a
    // structure must be reported rather than silently dropped.
    UnitBuilder inh( false, 8 );
    unsigned hcu = inh.add_abbrev( DW_TAG_compile_unit, true, {} );
    unsigned hbase = inh.add_abbrev(
        DW_TAG_base_type, false,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned hstruct = inh.add_abbrev(
        DW_TAG_structure_type, true,
        { { DW_AT_name, DW_FORM_string },
          { DW_AT_byte_size, DW_FORM_data1 } } );
    unsigned hinherit = inh.add_abbrev(
        0x1c, false, // DW_TAG_inheritance: not in the supported tag set
        { { DW_AT_type, DW_FORM_ref4 } } );
    inh.enter_die( hcu );
    std::uint64_t hint = inh.enter_die_marked( hbase );
    inh.attr_string( "Base" );
    inh.attr_udata( DW_FORM_data1, 4 );
    std::uint64_t hroot = inh.enter_die_marked( hstruct );
    inh.attr_string( "Derived" );
    inh.attr_udata( DW_FORM_data1, 4 );
    inh.enter_die( hinherit );
    inh.attr_ref4( hint );
    inh.exit_die();
    inh.exit_die();
    auto helf = make_elf( false, inh.finalize( 0 ),
                          inh.abbrev_bytes(), inh.strtab_bytes() );
    auto hr = build_type_layout( *helf, 0, hroot );
    const type_layout_error* he = expect_error( hr );
    check( he != nullptr && he->unsupported &&
               he->section == ".debug_info",
           "unsupported child construct is a located unsupported error" );
}

} // namespace

int main()
{
    test_units();
    test_recursion();
    test_layout();

    if ( g_failures != 0 ) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "PASS units\n";
    std::cout << "PASS recursion\n";
    std::cout << "PASS layout\n";
    std::cout << "TYPE_LAYOUT_OK\n";
    return 0;
}
