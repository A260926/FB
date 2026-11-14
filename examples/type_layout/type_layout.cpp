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

// Read-only DWARF4 type layout usage example. Given an ELF file, the section
// offset of a DWARF32 v4 compilation unit header and the section offset of a
// root type DIE (both hexadecimal), it prints the recovered owning type
// graph. The input image is never executed or modified.
//
// Usage: type_layout <elf_file> <unit_offset> <root_die_offset>

#include <elfio/elfio.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>

using namespace ELFIO;

namespace {

const char* kind_name( type_kind kind )
{
    switch ( kind ) {
    case type_kind::compile_unit:
        return "compile_unit";
    case type_kind::base_type:
        return "base_type";
    case type_kind::structure_type:
        return "structure_type";
    case type_kind::union_type:
        return "union_type";
    case type_kind::member:
        return "member";
    case type_kind::pointer_type:
        return "pointer_type";
    case type_kind::typedef_type:
        return "typedef";
    case type_kind::const_type:
        return "const_type";
    case type_kind::volatile_type:
        return "volatile_type";
    case type_kind::array_type:
        return "array_type";
    case type_kind::subrange_type:
        return "subrange_type";
    }
    return "unknown";
}

void print_indent( int depth )
{
    for ( int i = 0; i < depth; ++i ) {
        std::cout << "  ";
    }
}

void print_node( const type_node& node, int depth,
                 std::unordered_set<std::uint64_t>& seen )
{
    print_indent( depth );
    std::cout << kind_name( node.kind ) << " @0x" << std::hex
              << node.die_offset << std::dec;
    if ( !node.name.empty() ) {
        std::cout << " \"" << node.name << "\"";
    }
    if ( node.has_byte_size ) {
        std::cout << " size=" << node.byte_size;
    }
    std::cout << "\n";

    if ( node.kind == type_kind::member ) {
        const member_layout_info& mi = node.member_info;
        print_indent( depth + 1 );
        std::cout << "bit_offset=" << mi.bit_offset
                  << " bit_width=" << mi.bit_width;
        if ( mi.has_byte_offset ) {
            std::cout << " byte_offset=" << mi.byte_offset;
        }
        std::cout << "\n";
    }
    if ( node.kind == type_kind::subrange_type ) {
        print_indent( depth + 1 );
        std::cout << "element_count=" << node.range.element_count << "\n";
    }

    if ( node.type != nullptr ) {
        print_indent( depth + 1 );
        std::cout << "type -> @0x" << std::hex << node.type->die_offset
                  << std::dec << "\n";
        if ( node.kind != type_kind::pointer_type &&
             seen.insert( node.type->die_offset ).second ) {
            print_node( *node.type, depth + 1, seen );
        }
    }

    for ( const type_node* sub : node.subranges ) {
        print_node( *sub, depth + 1, seen );
    }
    for ( const type_node* member : node.members ) {
        print_node( *member, depth + 1, seen );
    }
}

std::uint64_t parse_hex( const char* text )
{
    return static_cast<std::uint64_t>(
        std::strtoull( text, nullptr, 16 ) );
}

} // namespace

int main( int argc, char** argv )
{
    if ( argc != 4 ) {
        std::cout << "Usage: " << argv[0]
                  << " <elf_file> <unit_offset> <root_die_offset>\n";
        return 1;
    }

    elfio reader;
    if ( !reader.load( argv[1] ) ) {
        std::cout << "Cannot load ELF file " << argv[1] << "\n";
        return 2;
    }

    std::uint64_t unit_offset = parse_hex( argv[2] );
    std::uint64_t root_offset = parse_hex( argv[3] );

    type_layout_result result =
        build_type_layout( reader, unit_offset, root_offset );
    if ( const auto* error = std::get_if<type_layout_error>( &result ) ) {
        std::cout << "type layout failed in " << error->section
                  << " at offset 0x" << std::hex << error->offset
                  << std::dec << ": " << error->message
                  << ( error->unsupported ? " (unsupported)" : "" )
                  << "\n";
        return 3;
    }

    const type_graph& graph = std::get<type_graph>( result );
    std::cout << "recovered " << graph.nodes.size()
              << " type node(s)\n";
    std::unordered_set<std::uint64_t> seen;
    seen.insert( graph.root->die_offset );
    print_node( *graph.root, 0, seen );
    return 0;
}
