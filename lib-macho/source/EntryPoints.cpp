/*******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Jean-David Gadina - www.xs-labs.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

/*!
 * @file        EntryPoints.cpp
 * @copyright   (c) 2020, Jean-David Gadina - www.xs-labs.com
 */

#include <MachO/File.hpp>
#include <MachO/EntryPoints.hpp>
#include <MachO/LoadCommand.hpp>
#include <MachO/LoadCommands/EntryPoint.hpp>
#include <MachO/LoadCommands/Segment.hpp>
#include <MachO/LoadCommands/Segment64.hpp>
#include <MachO/LoadCommands/Thread.hpp>
#include <limits>
#include <stdexcept>

namespace MachO
{
    namespace
    {
        constexpr uint32_t LC_THREAD    = 0x04;
        constexpr uint32_t LC_UNIXTHREAD = 0x05;

        struct SegmentInfo
        {
            size_t       index;
            std::string  name;
            uint64_t      vmAddress;
            uint64_t      vmSize;
            uint64_t      fileOffset;
            uint64_t      fileSize;
        };

        uint64_t checkedAdd( uint64_t a, uint64_t b, const std::string & what )
        {
            if( b > std::numeric_limits< uint64_t >::max() - a )
            {
                throw std::runtime_error( "Mach-O entry point coordinate overflow while computing " + what );
            }

            return a + b;
        }

        /* Half-open containment of x in [base, base+span). */
        bool halfOpenContains( uint64_t base, uint64_t span, uint64_t x, const std::string & what )
        {
            uint64_t end = checkedAdd( base, span, what );

            return x >= base && x < end;
        }

        EntryPointCandidate makeCandidate( const SegmentInfo & seg )
        {
            EntryPointCandidate c;

            c.segmentIndex = seg.index;
            c.segmentName  = seg.name;
            c.vmAddress    = seg.vmAddress;
            c.vmSize       = seg.vmSize;
            c.fileOffset   = seg.fileOffset;
            c.fileSize     = seg.fileSize;

            return c;
        }
    }

    std::vector< EntryPointSource > File::entryPointSources() const
    {
        return this->entryPoints();
    }

    std::vector< EntryPointSource > File::entryPoints() const
    {
        std::vector< std::reference_wrapper< LoadCommand > > commands = this->loadCommands();
        std::vector< SegmentInfo >                           segments;

        /* Segment table, preserving load command order and indices. */
        for( size_t i = 0; i < commands.size(); ++i )
        {
            LoadCommand & command = commands[ i ].get();

            if( auto * s64 = dynamic_cast< LoadCommands::Segment64 * >( &command ) )
            {
                segments.push_back( {
                    i,
                    s64->name(),
                    s64->vmAddress(),
                    s64->vmSize(),
                    s64->fileOffset(),
                    s64->fileSize()
                } );
            }
            else if( auto * s32 = dynamic_cast< LoadCommands::Segment * >( &command ) )
            {
                segments.push_back( {
                    i,
                    s32->name(),
                    static_cast< uint64_t >( s32->vmAddress() ),
                    static_cast< uint64_t >( s32->vmSize() ),
                    static_cast< uint64_t >( s32->fileOffset() ),
                    static_cast< uint64_t >( s32->fileSize() )
                } );
            }
        }

        std::vector< EntryPointSource > sources;

        for( size_t i = 0; i < commands.size(); ++i )
        {
            LoadCommand & command = commands[ i ].get();

            if( auto * mainCmd = dynamic_cast< LoadCommands::EntryPoint * >( &command ) )
            {
                /* LC_MAIN: offset is a file coordinate. */
                EntryPointSource source;

                source.commandIndex = i;
                source.kind         = EntryPointKind::Main;
                source.command      = mainCmd->command();
                source.coordinate   = mainCmd->offset();

                uint64_t offset = mainCmd->offset();

                for( const SegmentInfo & seg: segments )
                {
                    /* File interval must contain the offset (half-open). */
                    if( halfOpenContains( seg.fileOffset, seg.fileSize, offset, "segment file range" ) == false )
                    {
                        continue;
                    }

                    uint64_t displacement = offset - seg.fileOffset;

                    /* The displacement must also fit the virtual range. */
                    if( displacement >= seg.vmSize )
                    {
                        continue;
                    }

                    EntryPointCandidate candidate = makeCandidate( seg );

                    candidate.address          = checkedAdd( seg.vmAddress, displacement, "LC_MAIN virtual address" );
                    candidate.mappedFileOffset = offset;

                    source.candidates.push_back( candidate );
                }

                sources.push_back( std::move( source ) );
            }
            else if( auto * threadCmd = dynamic_cast< LoadCommands::Thread * >( &command ) )
            {
                /*
                 * LC_THREAD states are display-only and never claim an entry
                 * location; only LC_UNIXTHREAD decoded records do.
                 */
                uint32_t threadCommandId = threadCmd->command();

                if( threadCommandId == LC_THREAD || threadCommandId != LC_UNIXTHREAD )
                {
                    continue;
                }

                std::vector< ThreadRecord > records = threadCmd->records();

                for( size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex )
                {
                    const auto & context = records[ recordIndex ].context();

                    /* Every decoded record yields one entry source. */
                    if( context.has_value() == false )
                    {
                        continue;
                    }

                    uint64_t pc = context->pc();

                    EntryPointSource source;

                    source.commandIndex = i;
                    source.recordIndex  = recordIndex;
                    source.kind         = EntryPointKind::UnixThread;
                    source.command      = threadCmd->command();
                    source.coordinate   = pc;

                    for( const SegmentInfo & seg: segments )
                    {
                        /* Virtual interval must contain the PC (half-open). */
                        if( halfOpenContains( seg.vmAddress, seg.vmSize, pc, "segment virtual range" ) == false )
                        {
                            continue;
                        }

                        uint64_t displacement = pc - seg.vmAddress;

                        EntryPointCandidate candidate = makeCandidate( seg );

                        candidate.address = pc;

                        /* File bytes exist only below filesize; otherwise this is zero-fill. */
                        if( displacement < seg.fileSize )
                        {
                            candidate.mappedFileOffset = checkedAdd( seg.fileOffset, displacement, "thread PC file offset" );
                        }

                        source.candidates.push_back( candidate );
                    }

                    sources.push_back( std::move( source ) );
                }
            }
        }

        return sources;
    }
}
