/*
 * startup_dump.cpp
 *
 * Small C++17 example for the startup/thread-state API added to lib-macho.
 *
 * Build (from the repository root, after task_support/build.sh):
 *
 *   c++ -std=c++17 -O0 -I. -Ilib-macho/include -Istdxs/include \
 *       examples/startup_dump.cpp .task-build/libmacho.a \
 *       -o .task-build/startup_dump
 *
 * Run:
 *
 *   .task-build/startup_dump /path/to/MachO/or/FatFile
 *
 * This is a read-only viewer: it never loads or executes the image and never
 * follows dependent libraries.
 */

#include <MachO.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace
{
    std::string hex( uint64_t value )
    {
        return XS::ToString::Hex( value );
    }

    void printContext( const MachO::ThreadRecord & record )
    {
        const auto & context = record.context();

        if( context.has_value() == false )
        {
            std::cout << "      (undecoded record)\n";

            return;
        }

        std::cout << "      arch=" << context->architectureName()
                  << " flavor=" << context->flavor();

        if( context->innerFlavor().has_value() )
        {
            std::cout << " innerFlavor=" << *context->innerFlavor();
        }

        std::cout << " PC=" << hex( context->pc() )
                  << " SP=" << hex( context->sp() ) << '\n';

        for( const auto & reg: context->registers() )
        {
            std::cout << "        " << reg.name << " = " << hex( reg.value ) << '\n';
        }
    }

    void printFile( const MachO::File & file, const std::string & indent )
    {
        std::cout << indent << "cpu=" << hex( file.cpu().type() )
                  << " commands=" << file.loadCommands().size() << '\n';

        for( const auto & cmd: file.loadCommands() )
        {
            auto * thread = dynamic_cast< const MachO::LoadCommands::Thread * >( &cmd.get() );

            if( thread == nullptr )
            {
                continue;
            }

            std::cout << indent << "thread command " << thread->commandName()
                      << " size=" << thread->size() << '\n';

            size_t index = 0;

            for( const auto & record: thread->records() )
            {
                std::cout << indent << "  record[" << index << "]"
                          << " flavor=" << record.flavor()
                          << " count=" << record.count()
                          << " offset=" << record.offset()
                          << " payloadBytes=" << record.payload().size() << '\n';

                printContext( record );
                ++index;
            }
        }

        for( const auto & entry: file.entryPoints() )
        {
            std::cout << indent << "entry commandIndex=" << entry.commandIndex
                      << " kind=" << ( entry.kind == MachO::EntryPointKind::Main ? "LC_MAIN" : "LC_UNIXTHREAD" )
                      << " coordinate=" << hex( entry.coordinate );

            if( entry.recordIndex.has_value() )
            {
                std::cout << " recordIndex=" << *entry.recordIndex;
            }

            std::cout << '\n';

            for( const auto & candidate: entry.candidates )
            {
                std::cout << indent << "  segment commandIndex=" << candidate.segmentIndex
                          << " name='" << candidate.segmentName << "'"
                          << " address=" << hex( candidate.address );

                if( candidate.mappedFileOffset.has_value() )
                {
                    std::cout << " fileOffset=" << hex( *candidate.mappedFileOffset );
                }
                else
                {
                    std::cout << " fileOffset=<zero-fill tail>";
                }

                std::cout << '\n';
            }

            if( entry.candidates.empty() )
            {
                std::cout << indent << "  (no segment candidate; coordinate preserved)\n";
            }
        }
    }
}

int main( int argc, char ** argv )
{
    if( argc != 2 )
    {
        std::cerr << "usage: " << ( argc > 0 ? argv[ 0 ] : "startup_dump" ) << " <mach-o-file>\n";

        return 2;
    }

    try
    {
        /* A fat file is detected by its big endian 0xCAFEBABE magic. */
        XS::IO::BinaryFileStream probe( argv[ 1 ] );
        uint32_t                  magic = probe.readBigEndianUInt32();

        if( magic == 0xCAFEBABEu )
        {
            MachO::FatFile fat( argv[ 1 ] );

            for( const auto & arch: fat.architectures() )
            {
                std::cout << "slice cpu=" << hex( arch.first.cpu().type() )
                          << " offset=" << arch.first.offset() << '\n';
                printFile( arch.second, "  " );
            }
        }
        else
        {
            MachO::File file( argv[ 1 ] );

            printFile( file, "" );
        }
    }
    catch( const std::exception & e )
    {
        std::cerr << "error: " << e.what() << '\n';

        return 1;
    }

    return 0;
}
