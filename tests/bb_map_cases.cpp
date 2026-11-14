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

// Task test cases for the LLVM basic block address map index.
// All synthetic ELFs are built in memory; every expected value below is hand
// derived from the byte layout and listed independently of the decoder.

#include "../task_support/check.h"

#include <elfio/elfio.hpp>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace ELFIO;

namespace {

void uleb( std::string& s, uint64_t v )
{
    do {
        unsigned char b = static_cast<unsigned char>( v & 0x7F );
        v >>= 7;
        s.push_back( static_cast<char>( b | ( v ? 0x80 : 0 ) ) );
    } while ( v );
}

void addr8( std::string& s, uint64_t v )
{
    for ( int i = 0; i < 8; ++i ) {
        s.push_back( static_cast<char>( v >> ( 8 * i ) ) );
    }
}

struct built_file
{
    std::string bytes;
    Elf_Half    sec1 = 0;
    Elf_Half    sec2 = 0;
    Elf_Half    symtab = 0;
    Elf_Half    map_a = 0;
    Elf_Half    map_b = 0;
    Elf_Half    rela_a = 0;
    Elf_Half    rela_b = 0;
};

// Hand layout of map A (85 bytes):
//   entry0 v2/f8  @0  : r0 base@3 (4 blocks), r1 base@28 (1 block)
//   entry1 v3/f32 @41 : r0 base@43 (3 blocks, callsites, single range)
//   entry2 v2/f0  @70 : r0 base@72 (1 block, overlaps entry0 ranges)
const std::string& map_a_bytes()
{
    static const std::string bytes = [] {
        std::string s;
        // entry 0: version 2, feature 8 (ULEB range count = 2)
        s.push_back( char( 2 ) );
        s.push_back( char( 8 ) );
        uleb( s, 2 );
        addr8( s, 0 );
        uleb( s, 4 );
        uleb( s, 5 ); // id 5, gap 0, size 4 -> [0,4)
        uleb( s, 0 );
        uleb( s, 4 );
        uleb( s, 0 );
        uleb( s, 2 ); // id 2, gap 2, size 3 -> [6,9)
        uleb( s, 2 );
        uleb( s, 3 );
        uleb( s, 0 );
        uleb( s, 9 ); // id 9, gap 0, size 0 -> [9,9) zero length
        uleb( s, 0 );
        uleb( s, 0 );
        uleb( s, 0 );
        uleb( s, 1 ); // id 1, gap 1, size 6 -> [10,16)
        uleb( s, 1 );
        uleb( s, 6 );
        uleb( s, 0 );
        addr8( s, 0 ); // range 1: cold range of the same function
        uleb( s, 1 );
        uleb( s, 3 ); // id 3, gap 0, size 5
        uleb( s, 0 );
        uleb( s, 5 );
        uleb( s, 0 );
        // entry 1: version 3, feature 32 (callsite encoding, one range)
        s.push_back( char( 3 ) );
        s.push_back( char( 32 ) );
        addr8( s, 0 );
        uleb( s, 3 );
        uleb( s, 7 ); // id 7, gap 0, 2 callsites, tail 4
        uleb( s, 0 );
        uleb( s, 2 );
        uleb( s, 2 ); // delta 2 -> site at begin+2
        uleb( s, 3 ); // delta 3 -> site +3
        uleb( s, 4 ); // tail 4, end = last site + 4
        uleb( s, 0 );
        uleb( s, 6 ); // id 6, gap 0, 0 callsites, tail 0 -> zero length
        uleb( s, 0 );
        uleb( s, 0 );
        uleb( s, 0 );
        uleb( s, 0 );
        uleb( s, 8 ); // id 8, gap 1, 1 callsite delta 0, tail 2
        uleb( s, 1 );
        uleb( s, 1 );
        uleb( s, 0 ); // duplicate offset: site sits on block begin
        uleb( s, 2 );
        uleb( s, 0 );
        // entry 2: version 2, feature 0, single range
        s.push_back( char( 2 ) );
        s.push_back( char( 0 ) );
        addr8( s, 0 );
        uleb( s, 1 );
        uleb( s, 4 ); // id 4, gap 0, size 10 -> overlaps entry 0 blocks
        uleb( s, 0 );
        uleb( s, 10 );
        uleb( s, 0 );
        return s;
    }();
    return bytes;
}

// Map B: one plain v2/f0 entry, one block of size 2.
std::string map_b_bytes()
{
    std::string s;
    s.push_back( char( 2 ) );
    s.push_back( char( 0 ) );
    addr8( s, 0 );
    uleb( s, 1 );
    uleb( s, 1 );
    uleb( s, 0 );
    uleb( s, 2 );
    uleb( s, 0 );
    return s;
}

// Map B layout with the base field replaced by a virtual address.
std::string map_b_va_bytes( uint64_t base )
{
    std::string s = map_b_bytes();
    for ( int i = 0; i < 8; ++i ) {
        s[2 + i] = char( base >> ( 8 * i ) );
    }
    return s;
}

section* add_text( elfio& w, const char* name, uint64_t address,
                   Elf_Xword size = 64 )
{
    section* s = w.sections.add( name );
    s->set_type( SHT_PROGBITS );
    s->set_flags( SHF_ALLOC | SHF_EXECINSTR );
    s->set_addr_align( 16 );
    s->set_address( address );
    s->set_data( std::string( static_cast<size_t>( size ), char( 0x90 ) ) );
    return s;
}

// Build the relocatable variant. With two_maps, a second map section is
// added so the decoder cannot rely on a name or a single map section.
bool build_rel( bool two_maps, built_file& out )
{
    elfio w;
    w.create( ELFCLASS64, ELFDATA2LSB );
    w.set_type( ET_REL );
    w.set_machine( EM_X86_64 );

    section* s1 = add_text( w, ".text.one", 0 );
    section* s2 = add_text( w, ".text.two", 0 );
    out.sec1 = s1->get_index();
    out.sec2 = s2->get_index();

    section* str = w.sections.add( ".strtab" );
    str->set_type( SHT_STRTAB );
    string_section_accessor strings( str );
    section* sym = w.sections.add( ".symtab" );
    sym->set_type( SHT_SYMTAB );
    sym->set_link( str->get_index() );
    sym->set_info( 3 );
    sym->set_addr_align( 8 );
    sym->set_entry_size( w.get_default_entry_size( SHT_SYMTAB ) );
    out.symtab = sym->get_index();
    symbol_section_accessor syms( w, sym );
    // The section symbol for s1 is created to pin symbol indices, even
    // though the relocations below address s1 through the function symbol.
    syms.add_symbol( strings, "", 0, 0, STB_LOCAL, STT_SECTION, 0,
                     s1->get_index() );
    Elf_Word k_sec2 =
        syms.add_symbol( strings, "", 0, 0, STB_LOCAL, STT_SECTION, 0,
                         s2->get_index() );
    Elf_Word k_func =
        syms.add_symbol( strings, "worker", 0, 0, STB_GLOBAL, STT_FUNC, 0,
                         s1->get_index() );
    syms.add_symbol( strings, "cold_worker", 0, 0, STB_GLOBAL, STT_FUNC, 0,
                     s2->get_index() );

    section* map = w.sections.add( ".llvm_bb_addr_map.a" );
    map->set_type( SHT_LLVM_BB_ADDR_MAP );
    map->set_link( s1->get_index() );
    map->set_flags( SHF_LINK_ORDER );
    map->set_addr_align( 1 );
    map->set_data( map_a_bytes() );
    out.map_a = map->get_index();

    section* rela = w.sections.add( ".rela.map.a" );
    rela->set_type( SHT_RELA );
    rela->set_link( sym->get_index() );
    rela->set_info( map->get_index() );
    rela->set_addr_align( 8 );
    rela->set_entry_size( w.get_default_entry_size( SHT_RELA ) );
    out.rela_a = rela->get_index();
    relocation_section_accessor ra( w, rela );
    ra.add_entry( 3, k_func, R_X86_64_64, 0 );    // entry0 r0 -> sec1 + 0
    ra.add_entry( 28, k_sec2, R_X86_64_64, 4 );   // entry0 r1 -> sec2 + 4
    ra.add_entry( 43, k_func, R_X86_64_64, 32 );  // entry1    -> sec1 + 32
    ra.add_entry( 72, k_func, R_X86_64_64, 8 );   // entry2    -> sec1 + 8

    if ( two_maps ) {
        section* map2 = w.sections.add( ".llvm_bb_addr_map.b" );
        map2->set_type( SHT_LLVM_BB_ADDR_MAP );
        map2->set_link( s2->get_index() );
        map2->set_flags( SHF_LINK_ORDER );
        map2->set_addr_align( 1 );
        map2->set_data( map_b_bytes() );
        out.map_b = map2->get_index();

        section* rela2 = w.sections.add( ".rela.map.b" );
        rela2->set_type( SHT_RELA );
        rela2->set_link( sym->get_index() );
        rela2->set_info( map2->get_index() );
        rela2->set_addr_align( 8 );
        rela2->set_entry_size( w.get_default_entry_size( SHT_RELA ) );
        out.rela_b = rela2->get_index();
        relocation_section_accessor rb( w, rela2 );
        rb.add_entry( 2, k_sec2, R_X86_64_64, 0 );
    }

    std::ostringstream os( std::ios::binary );
    if ( !w.save( os ) ) {
        return false;
    }
    out.bytes = os.str();
    return true;
}

// Linked variant: same geometry, virtual addresses instead of relocations.
bool build_exec( built_file& out )
{
    elfio w;
    w.create( ELFCLASS64, ELFDATA2LSB );
    w.set_type( ET_EXEC );
    w.set_machine( EM_X86_64 );

    section* s1 = add_text( w, ".text.one", 0x1000, 128 );
    section* s2 = add_text( w, ".text.two", 0x2000, 128 );
    out.sec1 = s1->get_index();
    out.sec2 = s2->get_index();

    const std::string& s = map_a_bytes();
    std::string bytes;
    bytes.reserve( s.size() + 16 );
    bytes.push_back( char( 2 ) );
    bytes.push_back( char( 8 ) );
    uleb( bytes, 2 );
    addr8( bytes, 0x1000 ); // entry0 r0
    bytes.append( s, 11, 17 ); // block count + 4 block records
    addr8( bytes, 0x2004 ); // entry0 r1
    bytes.append( s, 36, 5 ); // count + one block
    bytes.push_back( char( 3 ) );
    bytes.push_back( char( 32 ) );
    addr8( bytes, 0x1020 ); // entry1
    bytes.append( s, 51, 19 );
    bytes.push_back( char( 2 ) );
    bytes.push_back( char( 0 ) );
    addr8( bytes, 0x1008 ); // entry2
    bytes.append( s, 80, 5 );
    bytes.push_back( char( 3 ) );
    bytes.push_back( char( 40 ) );
    uleb( bytes, 1 ); // feature 40 carries an explicit range count
    addr8( bytes, 0x1050 );
    uleb( bytes, 1 );
    uleb( bytes, 2 );
    uleb( bytes, 0 );
    uleb( bytes, 1 );
    uleb( bytes, 3 ); // delta 3
    uleb( bytes, 0 ); // tail 0: end == callsite
    uleb( bytes, 0 );

    section* map = w.sections.add( ".llvm_bb_addr_map" );
    map->set_type( SHT_LLVM_BB_ADDR_MAP );
    map->set_addr_align( 1 );
    map->set_data( bytes );
    out.map_a = map->get_index();

    std::ostringstream os( std::ios::binary );
    if ( !w.save( os ) ) {
        return false;
    }
    out.bytes = os.str();
    return true;
}

bool load_index( const std::string& bytes, elfio& reader,
                 bb_map_error& error,
                 std::unique_ptr<bb_map_index>& index )
{
    std::istringstream is( bytes, std::ios::binary );
    if ( !reader.load( is ) ) {
        return false;
    }
    index = bb_map_index::load( reader, error );
    return index != nullptr;
}

bool block_of( const bb_map_index& index, Elf_Xword function, Elf_Xword block,
               const bb_map_block*& out )
{
    const bb_map_function& fn = index.get_function( function );
    if ( block >= fn.blocks.size() ) {
        return false;
    }
    out = &fn.blocks[static_cast<size_t>( block )];
    return true;
}

// ---------------------------------------------------------------------------
// ranges group
// ---------------------------------------------------------------------------

int ranges_split_and_gaps()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );
    TASK_CHECK( index->get_functions_num() == 3 );

    const bb_map_function& fn0 = index->get_function( 0 );
    TASK_CHECK( fn0.map_section == f.map_a );
    TASK_CHECK( fn0.entry_offset == 0 );
    TASK_CHECK( fn0.version == 2 && fn0.feature == 8 );
    TASK_CHECK( fn0.ranges.size() == 2 );
    TASK_CHECK( fn0.ranges[0].ordinal == 0 );
    TASK_CHECK( fn0.ranges[1].ordinal == 1 );
    TASK_CHECK( fn0.ranges[0].code_section == f.sec1 );
    TASK_CHECK( fn0.ranges[1].code_section == f.sec2 );
    TASK_CHECK( fn0.ranges[0].base_offset == 0 );
    TASK_CHECK( fn0.ranges[1].base_offset == 4 );
    TASK_CHECK( fn0.blocks.size() == 5 );

    // Hand-listed coordinates, sec1 base 0 then sec2 base 4.
    const bb_map_block* b0 = nullptr;
    const bb_map_block* b1 = nullptr;
    const bb_map_block* b2 = nullptr;
    const bb_map_block* b3 = nullptr;
    const bb_map_block* b4 = nullptr;
    TASK_CHECK( block_of( *index, 0, 0, b0 ) );
    TASK_CHECK( block_of( *index, 0, 1, b1 ) );
    TASK_CHECK( block_of( *index, 0, 2, b2 ) );
    TASK_CHECK( block_of( *index, 0, 3, b3 ) );
    TASK_CHECK( block_of( *index, 0, 4, b4 ) );
    TASK_CHECK( b0->code_section == f.sec1 && b0->begin == 0 &&
                b0->end == 4 );
    TASK_CHECK( b1->code_section == f.sec1 && b1->begin == 6 &&
                b1->end == 9 );
    TASK_CHECK( b2->code_section == f.sec1 && b2->begin == 9 &&
                b2->end == 9 ); // zero length, retained
    TASK_CHECK( b3->code_section == f.sec1 && b3->begin == 10 &&
                b3->end == 16 );
    TASK_CHECK( b4->code_section == f.sec2 && b4->begin == 4 &&
                b4->end == 9 );
    TASK_CHECK( b0->block_ordinal == 0 && b4->block_ordinal == 4 );

    // Gaps are never filled: [4,6) and [9,10) have no covering block.
    TASK_CHECK( index->find( f.sec1, 4 ).empty() );
    TASK_CHECK( index->find( f.sec1, 5 ).empty() );
    TASK_CHECK( index->find( f.sec1, 9 ).size() ==
                1 ); // zero block does not hit
    TASK_CHECK( index->find( f.sec1, 10 ).size() >= 1 );
    return 0;
}

int ranges_non_sequential_ids()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );

    const Elf_Xword expected[4] = { 5, 2, 9, 1 };
    const bb_map_function& fn0 = index->get_function( 0 );
    for ( Elf_Xword i = 0; i < 4; ++i ) {
        TASK_CHECK( fn0.blocks[i].id == expected[i] );
    }
    // IDs may be reused by another function.
    const bb_map_function& fn2 = index->get_function( 2 );
    TASK_CHECK( fn2.blocks[0].id == 4 );
    return 0;
}

int ranges_overlap_order()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );

    // Offset 11 is covered by entry0 id1 [10,16) and entry2 id4 [8,18).
    auto hits = index->find( f.sec1, 11 );
    TASK_CHECK( hits.size() == 2 );
    TASK_CHECK( hits[0]->id == 1 && hits[1]->id == 4 );
    TASK_CHECK( hits[0]->function_offset == 0 &&
                hits[1]->function_offset == 70 );

    // Half-open boundaries: begin hits, end does not.
    TASK_CHECK( index->find( f.sec1, 10 )[0]->id == 1 );
    TASK_CHECK( index->find( f.sec1, 16 ).size() == 1 );
    TASK_CHECK( index->find( f.sec1, 16 )[0]->id == 4 );
    TASK_CHECK( index->find( f.sec1, 18 ).empty() );
    return 0;
}

int ranges_cross_section_same_offset()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );

    // Numeric offset 6 exists in both code sections but means different
    // addresses; the query must not mix sections. Sec1: id2 [6,9), sec2:
    // the cold id3 block resolved to [4,9).
    auto in_sec1 = index->find( f.sec1, 6 );
    auto in_sec2 = index->find( f.sec2, 6 );
    TASK_CHECK( in_sec1.size() == 1 && in_sec1[0]->id == 2 );
    TASK_CHECK( in_sec2.size() == 1 && in_sec2[0]->id == 3 );
    TASK_CHECK( in_sec1[0]->code_section == f.sec1 );
    TASK_CHECK( in_sec2[0]->code_section == f.sec2 );
    return 0;
}

int ranges_zero_length()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );

    const bb_map_block* z1 = nullptr; // id 9
    const bb_map_block* z2 = nullptr; // id 6
    TASK_CHECK( block_of( *index, 0, 2, z1 ) );
    TASK_CHECK( block_of( *index, 1, 1, z2 ) );
    TASK_CHECK( z1->id == 9 && z1->begin == z1->end );
    TASK_CHECK( z2->id == 6 && z2->begin == z2->end );
    TASK_CHECK( z1->map_end > z1->map_begin );
    TASK_CHECK( index->find( f.sec1, 9 ).size() == 1 ); // only id4 [8,18)
    TASK_CHECK( index->find( f.sec1, 41 ).empty() );
    return 0;
}

int ranges_linked_mapping()
{
    built_file f;
    TASK_CHECK( build_exec( f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );
    TASK_CHECK( index->get_functions_num() == 4 );

    // VA 0x1000 -> sec1 offset 0; VA 0x2004 -> sec2 offset 4.
    const bb_map_function& fn0 = index->get_function( 0 );
    TASK_CHECK( fn0.ranges[0].code_section == f.sec1 );
    TASK_CHECK( fn0.ranges[0].base_offset == 0 );
    TASK_CHECK( fn0.ranges[1].code_section == f.sec2 );
    TASK_CHECK( fn0.ranges[1].base_offset == 4 );
    TASK_CHECK( !fn0.ranges[0].relocation.present );

    const bb_map_block* b0 = nullptr;
    TASK_CHECK( block_of( *index, 0, 0, b0 ) );
    TASK_CHECK( b0->begin == 0 && b0->end == 4 );
    const bb_map_block* cold = nullptr;
    TASK_CHECK( block_of( *index, 0, 4, cold ) );
    TASK_CHECK( cold->begin == 4 && cold->end == 9 );

    // Last entry: block at sec1 [0x50,0x53), callsite exactly at end.
    const bb_map_function& fn3 = index->get_function( 3 );
    TASK_CHECK( fn3.blocks[0].begin == 0x50 && fn3.blocks[0].end == 0x53 );
    TASK_CHECK( fn3.blocks[0].callsites.size() == 1 );
    TASK_CHECK( fn3.blocks[0].callsites[0].offset == 0x53 );
    TASK_CHECK( index->find( f.sec1, 0x52 ).size() == 1 );
    TASK_CHECK( index->find( f.sec1, 0x53 ).empty() );
    return 0;
}

int ranges_empty_index()
{
    elfio w;
    w.create( ELFCLASS64, ELFDATA2LSB );
    w.set_type( ET_EXEC );
    w.set_machine( EM_X86_64 );
    add_text( w, ".text", 0x1000 );
    std::ostringstream os( std::ios::binary );
    TASK_CHECK( w.save( os ) );

    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( os.str(), reader, error, index ) );
    TASK_CHECK( index->get_functions_num() == 0 );
    TASK_CHECK( index->get_blocks_num() == 0 );
    TASK_CHECK( index->find( 1, 0 ).empty() );
    return 0;
}

int ranges_v3_split_in_dyn()
{
    // Version 3, feature 8: explicit range count, plain size encoding.
    // Built as ET_DYN so the linked-object path covers that file type too.
    elfio w;
    w.create( ELFCLASS64, ELFDATA2LSB );
    w.set_type( ET_DYN );
    w.set_machine( EM_X86_64 );
    section* a = add_text( w, ".text.a", 0x5000 );
    section* b = add_text( w, ".text.b", 0x6000 );

    std::string bytes;
    bytes.push_back( char( 3 ) );
    bytes.push_back( char( 8 ) );
    uleb( bytes, 2 );
    addr8( bytes, 0x5000 );
    uleb( bytes, 1 );
    uleb( bytes, 11 ); // id 11, gap 0, size 2 -> [0,2)
    uleb( bytes, 0 );
    uleb( bytes, 2 );
    uleb( bytes, 0 );
    addr8( bytes, 0x6000 );
    uleb( bytes, 1 );
    uleb( bytes, 12 ); // id 12, gap 1, size 3 -> [1,4)
    uleb( bytes, 1 );
    uleb( bytes, 3 );
    uleb( bytes, 1 );

    section* map = w.sections.add( ".llvm_bb_addr_map" );
    map->set_type( SHT_LLVM_BB_ADDR_MAP );
    map->set_data( bytes );
    std::ostringstream os( std::ios::binary );
    TASK_CHECK( w.save( os ) );

    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( os.str(), reader, error, index ) );
    TASK_CHECK( index->get_functions_num() == 1 );
    const bb_map_function& fn = index->get_function( 0 );
    TASK_CHECK( fn.version == 3 && fn.feature == 8 );
    TASK_CHECK( fn.ranges.size() == 2 );
    TASK_CHECK( fn.blocks.size() == 2 );
    TASK_CHECK( fn.blocks[0].callsites.empty() );
    TASK_CHECK( fn.blocks[1].callsites.empty() );
    TASK_CHECK( fn.blocks[0].code_section == a->get_index() &&
                fn.blocks[0].begin == 0 && fn.blocks[0].end == 2 );
    TASK_CHECK( fn.blocks[1].code_section == b->get_index() &&
                fn.blocks[1].begin == 1 && fn.blocks[1].end == 4 );
    TASK_CHECK( index->find( a->get_index(), 1 ).size() == 1 );
    TASK_CHECK( index->find( b->get_index(), 3 )[0]->id == 12 );
    TASK_CHECK( index->find( b->get_index(), 0 ).empty() ); // gap
    return 0;
}

// ---------------------------------------------------------------------------
// callsites group
// ---------------------------------------------------------------------------

int calls_successive_deltas()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );

    // Entry 1 range base resolves to sec1 + 32. Block id7 [32,41):
    // deltas 2 then 3 give callsites 32+2=34 and 34+3=37.
    const bb_map_block* b = nullptr;
    TASK_CHECK( block_of( *index, 1, 0, b ) );
    TASK_CHECK( b->id == 7 );
    TASK_CHECK( b->callsites.size() == 2 );
    TASK_CHECK( b->callsites[0].offset == 34 );
    TASK_CHECK( b->callsites[1].offset == 37 );
    // Hand-listed delta ULEB positions in the map section.
    TASK_CHECK( b->callsites[0].map_offset == 55 );
    TASK_CHECK( b->callsites[1].map_offset == 56 );
    TASK_CHECK( b->begin == 32 && b->end == 41 );

    auto hits = index->find( f.sec1, 34 );
    TASK_CHECK( hits.size() == 1 && hits[0]->id == 7 );
    return 0;
}

int calls_zero_count_and_duplicate()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );

    const bb_map_block* zero = nullptr; // id6, count 0
    TASK_CHECK( block_of( *index, 1, 1, zero ) );
    TASK_CHECK( zero->callsites.empty() );
    TASK_CHECK( zero->begin == zero->end );

    const bb_map_block* dup = nullptr; // id8, delta 0
    TASK_CHECK( block_of( *index, 1, 2, dup ) );
    TASK_CHECK( dup->begin == 42 && dup->end == 44 );
    TASK_CHECK( dup->callsites.size() == 1 );
    TASK_CHECK( dup->callsites[0].offset == 42 );
    TASK_CHECK( dup->callsites[0].map_offset == 67 );
    return 0;
}

int calls_v2_blocks_have_none()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );

    const bb_map_function& fn0 = index->get_function( 0 );
    for ( const auto& block : fn0.blocks ) {
        TASK_CHECK( block.callsites.empty() );
    }
    const bb_map_function& fn2 = index->get_function( 2 );
    TASK_CHECK( fn2.blocks[0].callsites.empty() );
    return 0;
}

int calls_inside_block_check()
{
    built_file f;
    TASK_CHECK( build_exec( f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );

    // Callsite at block end is accepted; offset beyond end still misses.
    const bb_map_function& fn3 = index->get_function( 3 );
    const bb_map_block& b = fn3.blocks[0];
    TASK_CHECK( b.callsites[0].offset == b.end );
    TASK_CHECK( index->find( f.sec1, b.end ).empty() );
    return 0;
}

// ---------------------------------------------------------------------------
// origins group
// ---------------------------------------------------------------------------

int origins_record_identity()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );

    // Hand-listed entry byte offsets in map A.
    TASK_CHECK( index->get_function( 0 ).entry_offset == 0 );
    TASK_CHECK( index->get_function( 1 ).entry_offset == 41 );
    TASK_CHECK( index->get_function( 2 ).entry_offset == 70 );
    TASK_CHECK( index->find_function( f.map_a, 0 ) ==
                &index->get_function( 0 ) );
    TASK_CHECK( index->find_function( f.map_a, 41 ) ==
                &index->get_function( 1 ) );
    TASK_CHECK( index->find_function( f.map_a, 70 ) ==
                &index->get_function( 2 ) );
    TASK_CHECK( index->find_function( f.map_a, 1 ) == nullptr );
    TASK_CHECK( index->find_function( f.map_b, 0 ) == nullptr );
    return 0;
}

int origins_byte_spans()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );

    section* map = reader.sections[f.map_a];
    TASK_CHECK( map != nullptr );
    const Elf64_Off map_file = map->get_offset();

    const bb_map_function& fn0 = index->get_function( 0 );
    // Hand-listed block record spans.
    TASK_CHECK( fn0.blocks[0].map_begin == 12 &&
                fn0.blocks[0].map_end == 16 );
    TASK_CHECK( fn0.blocks[3].map_begin == 24 &&
                fn0.blocks[3].map_end == 28 );
    TASK_CHECK( fn0.blocks[4].map_begin == 37 &&
                fn0.blocks[4].map_end == 41 );
    // Range base field locations: section offset and physical file offset.
    TASK_CHECK( fn0.ranges[0].map_offset == 3 );
    TASK_CHECK( fn0.ranges[1].map_offset == 28 );
    TASK_CHECK( fn0.ranges[0].base_file_offset == map_file + 3 );
    TASK_CHECK( fn0.ranges[1].base_file_offset == map_file + 28 );

    const bb_map_function& fn1 = index->get_function( 1 );
    TASK_CHECK( fn1.ranges[0].map_offset == 43 );
    TASK_CHECK( fn1.ranges[0].base_file_offset == map_file + 43 );
    TASK_CHECK( fn1.blocks[2].map_begin == 64 &&
                fn1.blocks[2].map_end == 70 );
    return 0;
}

int origins_relocation_identity()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );

    const bb_map_function& fn0 = index->get_function( 0 );
    const bb_map_relocation_info& r0 = fn0.ranges[0].relocation;
    const bb_map_relocation_info& r1 = fn0.ranges[1].relocation;
    TASK_CHECK( r0.present && r1.present );
    TASK_CHECK( r0.rela_section == f.rela_a && r0.rela_index == 0 );
    TASK_CHECK( r1.rela_section == f.rela_a && r1.rela_index == 1 );
    TASK_CHECK( r0.symbol_table == f.symtab && r0.symbol_index == 3 );
    TASK_CHECK( r1.symbol_table == f.symtab && r1.symbol_index == 2 );
    TASK_CHECK( r0.addend == 0 && r1.addend == 4 );

    const bb_map_function& fn1 = index->get_function( 1 );
    TASK_CHECK( fn1.ranges[0].relocation.rela_index == 2 );
    TASK_CHECK( fn1.ranges[0].relocation.addend == 32 );
    const bb_map_function& fn2 = index->get_function( 2 );
    TASK_CHECK( fn2.ranges[0].relocation.rela_index == 3 );
    TASK_CHECK( fn2.ranges[0].relocation.addend == 8 );
    return 0;
}

int origins_two_map_sections()
{
    built_file f;
    TASK_CHECK( build_rel( true, f ) );
    elfio reader;
    bb_map_error error;
    std::unique_ptr<bb_map_index> index;
    TASK_CHECK( load_index( f.bytes, reader, error, index ) );
    TASK_CHECK( index->get_functions_num() == 4 ); // 3 in A, 1 in B
    TASK_CHECK( index->get_function( 3 ).map_section == f.map_b );
    TASK_CHECK( index->get_function( 3 ).entry_offset == 0 );

    // Same section offset 0: distinct blocks in two code sections.
    auto one = index->find( f.sec1, 0 );
    auto two = index->find( f.sec2, 0 );
    TASK_CHECK( one.size() == 1 && one[0]->id == 5 );
    TASK_CHECK( two.size() == 1 && two[0]->id == 1 );
    return 0;
}

int origins_ownership_after_elfio_gone()
{
    built_file f;
    TASK_CHECK( build_rel( false, f ) );
    std::unique_ptr<bb_map_index> index;
    std::vector<const bb_map_block*> hits;
    {
        elfio reader;
        bb_map_error error;
        TASK_CHECK( load_index( f.bytes, reader, error, index ) );
        hits = index->find( f.sec1, 11 );
        TASK_CHECK( hits.size() == 2 );
    } // reader destroyed here; the index must keep working

    TASK_CHECK( index->get_functions_num() == 3 );
    TASK_CHECK( index->get_blocks_num() == 9 );
    TASK_CHECK( hits[0]->id == 1 && hits[1]->id == 4 );
    TASK_CHECK( hits[0]->begin == 10 && hits[1]->end == 18 );
    TASK_CHECK( index->find( f.sec2, 8 ).size() == 1 );
    TASK_CHECK( index->find_function( f.map_a, 41 )->feature == 32 );

    // The index is movable and remains usable afterwards.
    bb_map_index moved = std::move( *index );
    TASK_CHECK( moved.get_functions_num() == 3 );
    TASK_CHECK( moved.find( f.sec1, 11 ).size() == 2 );
    return 0;
}

int origins_rejected_inputs()
{
    // Unsupported feature byte.
    {
        std::string bad = map_a_bytes();
        bad[1] = char( 1 );
        elfio w;
        w.create( ELFCLASS64, ELFDATA2LSB );
        w.set_type( ET_REL );
        w.set_machine( EM_X86_64 );
        section* s = add_text( w, ".text", 0 );
        section* map = w.sections.add( ".llvm_bb_addr_map" );
        map->set_type( SHT_LLVM_BB_ADDR_MAP );
        map->set_link( s->get_index() );
        map->set_data( bad );
        std::ostringstream os( std::ios::binary );
        TASK_CHECK( w.save( os ) );
        elfio reader;
        bb_map_error error;
        std::unique_ptr<bb_map_index> index;
        TASK_CHECK( !load_index( os.str(), reader, error, index ) );
        TASK_CHECK( error.has_section && error.has_offset );
        TASK_CHECK( error.section_offset == 0 );
        TASK_CHECK( !error.message.empty() );
    }
    // Truncated record: drop the final metadata byte.
    {
        std::string bad = map_a_bytes();
        bad.pop_back();
        elfio w;
        w.create( ELFCLASS64, ELFDATA2LSB );
        w.set_type( ET_EXEC );
        w.set_machine( EM_X86_64 );
        add_text( w, ".text", 0x1000 );
        section* map = w.sections.add( ".llvm_bb_addr_map" );
        map->set_type( SHT_LLVM_BB_ADDR_MAP );
        map->set_data( bad );
        std::ostringstream os( std::ios::binary );
        TASK_CHECK( w.save( os ) );
        elfio reader;
        bb_map_error error;
        std::unique_ptr<bb_map_index> index;
        TASK_CHECK( !load_index( os.str(), reader, error, index ) );
        TASK_CHECK( error.has_section );
    }
    // ET_REL with a missing base relocation.
    {
        elfio w;
        w.create( ELFCLASS64, ELFDATA2LSB );
        w.set_type( ET_REL );
        w.set_machine( EM_X86_64 );
        section* s = add_text( w, ".text", 0 );
        section* map = w.sections.add( ".llvm_bb_addr_map" );
        map->set_type( SHT_LLVM_BB_ADDR_MAP );
        map->set_link( s->get_index() );
        map->set_data( map_b_bytes() );
        std::ostringstream os( std::ios::binary );
        TASK_CHECK( w.save( os ) );
        elfio reader;
        bb_map_error error;
        std::unique_ptr<bb_map_index> index;
        TASK_CHECK( !load_index( os.str(), reader, error, index ) );
        TASK_CHECK( error.has_section && error.has_offset );
        TASK_CHECK( error.section_offset == 2 ); // base field offset
    }
    // ET_REL with the wrong relocation type.
    {
        elfio w;
        w.create( ELFCLASS64, ELFDATA2LSB );
        w.set_type( ET_REL );
        w.set_machine( EM_X86_64 );
        section* s = add_text( w, ".text", 0 );
        section* str = w.sections.add( ".strtab" );
        str->set_type( SHT_STRTAB );
        string_section_accessor strings( str );
        section* sym = w.sections.add( ".symtab" );
        sym->set_type( SHT_SYMTAB );
        sym->set_link( str->get_index() );
        sym->set_info( 2 );
        sym->set_entry_size( w.get_default_entry_size( SHT_SYMTAB ) );
        symbol_section_accessor syms( w, sym );
        Elf_Word k = syms.add_symbol( strings, "", 0, 0, STB_LOCAL,
                                      STT_SECTION, 0, s->get_index() );
        section* map = w.sections.add( ".llvm_bb_addr_map" );
        map->set_type( SHT_LLVM_BB_ADDR_MAP );
        map->set_link( s->get_index() );
        map->set_data( map_b_bytes() );
        section* rela = w.sections.add( ".rela.map" );
        rela->set_type( SHT_RELA );
        rela->set_link( sym->get_index() );
        rela->set_info( map->get_index() );
        rela->set_entry_size( w.get_default_entry_size( SHT_RELA ) );
        relocation_section_accessor ra( w, rela );
        ra.add_entry( 2, k, R_X86_64_PC32, 0 );
        std::ostringstream os( std::ios::binary );
        TASK_CHECK( w.save( os ) );
        elfio reader;
        bb_map_error error;
        std::unique_ptr<bb_map_index> index;
        TASK_CHECK( !load_index( os.str(), reader, error, index ) );
        TASK_CHECK( error.has_section );
    }
    // Linked object: base VA outside every executable section.
    {
        elfio w;
        w.create( ELFCLASS64, ELFDATA2LSB );
        w.set_type( ET_EXEC );
        w.set_machine( EM_X86_64 );
        add_text( w, ".text", 0x1000 );
        section* map = w.sections.add( ".llvm_bb_addr_map" );
        map->set_type( SHT_LLVM_BB_ADDR_MAP );
        map->set_data( map_b_bytes() ); // raw base 0 -> no section at 0
        std::ostringstream os( std::ios::binary );
        TASK_CHECK( w.save( os ) );
        elfio reader;
        bb_map_error error;
        std::unique_ptr<bb_map_index> index;
        TASK_CHECK( !load_index( os.str(), reader, error, index ) );
        TASK_CHECK( error.has_section );
    }
    // Linked object: ambiguous mapping, two sections at the same address.
    {
        elfio w;
        w.create( ELFCLASS64, ELFDATA2LSB );
        w.set_type( ET_DYN );
        w.set_machine( EM_X86_64 );
        add_text( w, ".text.a", 0x3000 );
        add_text( w, ".text.b", 0x3000 );
        std::string bytes = map_b_bytes();
        for ( int i = 2; i < 10; ++i ) {
            bytes[i] = char( uint64_t( 0x3000 ) >> ( 8 * ( i - 2 ) ) );
        }
        section* map = w.sections.add( ".llvm_bb_addr_map" );
        map->set_type( SHT_LLVM_BB_ADDR_MAP );
        map->set_data( bytes );
        std::ostringstream os( std::ios::binary );
        TASK_CHECK( w.save( os ) );
        elfio reader;
        bb_map_error error;
        std::unique_ptr<bb_map_index> index;
        TASK_CHECK( !load_index( os.str(), reader, error, index ) );
        TASK_CHECK( error.has_section && error.has_offset );
    }
    // Duplicate block id inside one function (across two ranges counts too).
    {
        elfio w;
        w.create( ELFCLASS64, ELFDATA2LSB );
        w.set_type( ET_REL );
        w.set_machine( EM_X86_64 );
        section* s1 = add_text( w, ".text.one", 0 );
        section* s2 = add_text( w, ".text.two", 0 );
        section* str = w.sections.add( ".strtab" );
        str->set_type( SHT_STRTAB );
        string_section_accessor strings( str );
        section* sym = w.sections.add( ".symtab" );
        sym->set_type( SHT_SYMTAB );
        sym->set_link( str->get_index() );
        sym->set_info( 3 );
        sym->set_entry_size( w.get_default_entry_size( SHT_SYMTAB ) );
        symbol_section_accessor syms( w, sym );
        Elf_Word k1 = syms.add_symbol( strings, "", 0, 0, STB_LOCAL,
                                       STT_SECTION, 0, s1->get_index() );
        Elf_Word k2 = syms.add_symbol( strings, "", 0, 0, STB_LOCAL,
                                       STT_SECTION, 0, s2->get_index() );
        std::string bytes;
        bytes.push_back( char( 2 ) );
        bytes.push_back( char( 8 ) );
        uleb( bytes, 2 );
        addr8( bytes, 0 );
        uleb( bytes, 1 );
        uleb( bytes, 1 ); // id 1, size 1
        uleb( bytes, 0 );
        uleb( bytes, 1 );
        uleb( bytes, 0 );
        addr8( bytes, 0 );
        uleb( bytes, 1 );
        uleb( bytes, 1 ); // id 1 again in the second range: rejected
        uleb( bytes, 0 );
        uleb( bytes, 1 );
        uleb( bytes, 0 );
        section* map = w.sections.add( ".llvm_bb_addr_map" );
        map->set_type( SHT_LLVM_BB_ADDR_MAP );
        map->set_link( s1->get_index() );
        map->set_data( bytes );
        section* rela = w.sections.add( ".rela.map" );
        rela->set_type( SHT_RELA );
        rela->set_link( sym->get_index() );
        rela->set_info( map->get_index() );
        rela->set_entry_size( w.get_default_entry_size( SHT_RELA ) );
        relocation_section_accessor ra( w, rela );
        ra.add_entry( 3, k1, R_X86_64_64, 0 );
        ra.add_entry( 16, k2, R_X86_64_64, 0 );
        std::ostringstream os( std::ios::binary );
        TASK_CHECK( w.save( os ) );
        elfio reader;
        bb_map_error error;
        std::unique_ptr<bb_map_index> index;
        TASK_CHECK( !load_index( os.str(), reader, error, index ) );
        TASK_CHECK( error.has_section );
    }
    // Linked object carrying a relocation against the map section.
    {
        elfio w;
        w.create( ELFCLASS64, ELFDATA2LSB );
        w.set_type( ET_EXEC );
        w.set_machine( EM_X86_64 );
        add_text( w, ".text", 0x1000 );
        section* map = w.sections.add( ".llvm_bb_addr_map" );
        map->set_type( SHT_LLVM_BB_ADDR_MAP );
        map->set_data( map_b_va_bytes( 0x1000 ) );
        section* rela = w.sections.add( ".rela.map" );
        rela->set_type( SHT_RELA );
        rela->set_info( map->get_index() );
        rela->set_entry_size( w.get_default_entry_size( SHT_RELA ) );
        rela->set_data( std::string( 24, char( 0 ) ) );
        std::ostringstream os( std::ios::binary );
        TASK_CHECK( w.save( os ) );
        elfio reader;
        bb_map_error error;
        std::unique_ptr<bb_map_index> index;
        TASK_CHECK( !load_index( os.str(), reader, error, index ) );
        TASK_CHECK( error.has_section );
    }
    return 0;
}

} // namespace

const task_case task_cases[] = {
    { "ranges", "split_and_gaps", ranges_split_and_gaps },
    { "ranges", "non_sequential_ids", ranges_non_sequential_ids },
    { "ranges", "overlap_order", ranges_overlap_order },
    { "ranges", "cross_section_same_offset", ranges_cross_section_same_offset },
    { "ranges", "zero_length", ranges_zero_length },
    { "ranges", "linked_mapping", ranges_linked_mapping },
    { "ranges", "v3_split_in_dyn", ranges_v3_split_in_dyn },
    { "ranges", "empty_index", ranges_empty_index },
    { "callsites", "successive_deltas", calls_successive_deltas },
    { "callsites", "zero_count_and_duplicate",
      calls_zero_count_and_duplicate },
    { "callsites", "v2_blocks_have_none", calls_v2_blocks_have_none },
    { "callsites", "inside_block_check", calls_inside_block_check },
    { "origins", "record_identity", origins_record_identity },
    { "origins", "byte_spans", origins_byte_spans },
    { "origins", "relocation_identity", origins_relocation_identity },
    { "origins", "two_map_sections", origins_two_map_sections },
    { "origins", "ownership_after_elfio_gone",
      origins_ownership_after_elfio_gone },
    { "origins", "rejected_inputs", origins_rejected_inputs },
};

const size_t task_case_count =
    sizeof( task_cases ) / sizeof( task_cases[0] );
