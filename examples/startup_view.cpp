/*
 * Example: read-only startup view of a Mach-O or fat Mach-O file.
 *
 * Shows, for every image:
 *   - each thread record's flavor/count, offset, and decoded PC/SP
 *   - each startup source in load command order, with the VA and optional
 *     file coordinate produced by every matching segment
 *
 * The image is parsed only; it is never loaded, mapped, or executed.
 *
 * Build (from the repository root, after task_support/build.sh):
 *   c++ -std=c++17 -O0 -Ilib-macho/include -Istdxs/include \
 *       examples/startup_view.cpp .task-build/libmacho.a -o .task-build/startup_view
 * Run:
 *   .task-build/startup_view /path/to/binary
 */

#include <MachO/File.hpp>
#include <MachO/FatFile.hpp>
#include <MachO/LoadCommands/Thread.hpp>
#include <XS.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    void printThreads( const MachO::File & file )
    {
        auto threads = file.loadCommands< MachO::LoadCommands::Thread >();

        for( size_t i = 0; i < threads.size(); i++ )
        {
            auto records = threads[ i ].records();

            std::cout << "  thread command " << i
                      << " (" << threads[ i ].commandName() << ")"
                      << " records=" << records.size() << "\n";

            for( size_t r = 0; r < records.size(); r++ )
            {
                const auto & record = records[ r ];

                std::cout << "    record " << r
                          << " offset=" << record.offset
                          << " flavor=" << record.flavor
                          << " count=" << record.count
                          << " payload=" << record.payload.size() << " bytes";

                if( record.state.has_value() )
                {
                    const auto & state = *( record.state );

                    std::cout << " state=" << state.architectureName()
                              << " PC=" << XS::ToString::Hex( state.pc() )
                              << " SP=" << XS::ToString::Hex( state.sp() );
                }
                else
                {
                    std::cout << " state=raw";
                }

                std::cout << "\n";
            }
        }
    }

    void printEntryPoints( const MachO::File & file )
    {
        auto sources = file.entryPoints();

        for( size_t i = 0; i < sources.size(); i++ )
        {
            const auto & source = sources[ i ];

            std::cout << "  source " << i
                      << " kind=" << ( source.kind == MachO::File::EntryPointKind::Main ? "LC_MAIN" : "LC_UNIXTHREAD" )
                      << " command=" << source.commandIndex;

            if( source.recordIndex.has_value() )
            {
                std::cout << " record=" << source.recordIndex.value();
            }

            std::cout << " coordinate=" << XS::ToString::Hex( source.coordinate )
                      << " candidates=" << source.segments.size() << "\n";

            for( const auto & segment: source.segments )
            {
                std::cout << "    segment " << segment.commandIndex
                          << " " << segment.name
                          << " VA=" << XS::ToString::Hex( segment.address );

                if( segment.mappedFileOffset.has_value() )
                {
                    std::cout << " file=" << XS::ToString::Hex( segment.mappedFileOffset.value() );
                }
                else
                {
                    std::cout << " file=<zero-fill>";
                }

                std::cout << "\n";
            }
        }
    }

    void printFile( const std::string & label, const MachO::File & file )
    {
        std::cout << label << ": " << file.cpu().description()
                  << " (" << ( file.kind() == MachO::File::Kind::MachO64 ? "Mach-O 64" : "Mach-O 32" ) << ")\n";

        printThreads( file );
        printEntryPoints( file );
    }
}

int main( int argc, char * argv[] )
{
    if( argc != 2 )
    {
        std::cerr << "usage: " << argv[ 0 ] << " <mach-o-or-fat-file>\n";

        return 1;
    }

    try
    {
        /*
         * Detect the fat magic without committing to a parser. FatFile and
         * File each open the path themselves, so no stream is shared.
         */
        uint32_t magic = 0;

        {
            XS::IO::BinaryFileStream probe( argv[ 1 ] );

            magic = probe.readBigEndianUInt32();
        }

        if( magic == 0xCAFEBABE )
        {
            MachO::FatFile fat( argv[ 1 ] );

            for( const auto & architecture: fat.architectures() )
            {
                printFile
                (
                    "slice " + XS::ToString::Hex( static_cast< uint64_t >( architecture.first.offset() ) ),
                    architecture.second
                );
            }
        }
        else
        {
            MachO::File file( argv[ 1 ] );

            printFile( "image", file );
        }
    }
    catch( const std::exception & e )
    {
        std::cerr << "error: " << e.what() << "\n";

        return 1;
    }

    return 0;
}
