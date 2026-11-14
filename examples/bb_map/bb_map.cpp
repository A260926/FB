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

// bb_map: query LLVM basic block address maps of one ELF file.
//
// Usage: bb_map <elf-path> <code-section-index> <decimal-offset>
//
// Prints one TSV row per basic block covering the offset, or UNCOVERED when
// no block contains it. Columns are documented in doc/bb-map.md.

#include <elfio/elfio.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using namespace ELFIO;

namespace {

std::string callsite_list( const bb_map_block& block )
{
    std::string result;
    for ( size_t i = 0; i < block.callsites.size(); ++i ) {
        if ( i != 0 ) {
            result += ',';
        }
        result += std::to_string( block.callsites[i].offset );
    }
    return result;
}

void print_row( const bb_map_index& index, const bb_map_block& block )
{
    const bb_map_function* function =
        index.find_function( block.map_section, block.function_offset );
    const bb_map_range* range = nullptr;
    if ( function != nullptr ) {
        for ( const auto& candidate : function->ranges ) {
            if ( candidate.ordinal == block.range_ordinal ) {
                range = &candidate;
                break;
            }
        }
    }

    std::string rela = "-";
    std::string symbol = "-";
    std::string addend = "-";
    if ( range != nullptr && range->relocation.present ) {
        rela = std::to_string( range->relocation.rela_section ) + ':' +
               std::to_string( range->relocation.rela_index );
        symbol = std::to_string( range->relocation.symbol_table ) + ':' +
                 std::to_string( range->relocation.symbol_index );
        addend = std::to_string( range->relocation.addend );
    }

    std::cout << block.map_section << '\t' << block.function_offset << '\t'
              << block.range_ordinal << '\t' << block.block_ordinal << '\t'
              << block.id << '\t' << block.code_section << '\t' << block.begin
              << '\t' << block.end << '\t' << block.metadata << '\t'
              << callsite_list( block ) << '\t' << block.map_begin << '\t'
              << block.map_end;
    if ( range != nullptr ) {
        std::cout << '\t' << range->base_file_offset << '\t'
                  << range->base_offset << '\t' << range->raw_base << '\t'
                  << rela << '\t' << symbol << '\t' << addend;
    }
    std::cout << '\n';
}

} // namespace

int main( int argc, char** argv )
{
    if ( argc != 4 ) {
        std::cerr << "usage: " << argv[0]
                  << " <elf-path> <code-section-index> <decimal-offset>\n";
        return 1;
    }

    char*          section_end = nullptr;
    char*          offset_end = nullptr;
    long           parsed_section = std::strtol( argv[2], &section_end, 10 );
    unsigned long  parsed_offset = std::strtoul( argv[3], &offset_end, 10 );
    if ( *section_end != '\0' || *offset_end != '\0' ||
         parsed_section < 0 ) {
        std::cerr << "section index and offset must be decimal numbers\n";
        return 1;
    }

    // Build the index in a scope so the query below proves the index owns its
    // data and survives the source elfio object.
    std::unique_ptr<bb_map_index> index;
    {
        elfio reader;
        if ( !reader.load( argv[1] ) ) {
            std::cerr << "cannot load ELF file: " << argv[1] << '\n';
            return 2;
        }
        bb_map_error error;
        index = bb_map_index::load( reader, error );
        if ( index == nullptr ) {
            std::cerr << "bb map error";
            if ( error.has_section ) {
                std::cerr << " section=" << error.section_index;
            }
            if ( error.has_offset ) {
                std::cerr << " offset=" << error.section_offset;
            }
            std::cerr << ": " << error.message << '\n';
            return 3;
        }
    }

    auto hits = index->find( static_cast<Elf_Half>( parsed_section ),
                             static_cast<Elf_Xword>( parsed_offset ) );
    if ( hits.empty() ) {
        std::cout << "UNCOVERED\n";
        return 0;
    }
    for ( const auto* block : hits ) {
        print_row( *index, *block );
    }
    return 0;
}
