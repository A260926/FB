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
 * @header      EntryPoints.hpp
 * @copyright   (c) 2020, Jean-David Gadina - www.xs-labs.com
 *
 * @abstract    Value types for MachO::File::entryPoints().
 *
 * @discussion  LC_MAIN produces one EntryPointSource. LC_UNIXTHREAD produces
 *              one source per decoded thread record (using that state's PC).
 *              LC_THREAD states are display-only and never produce sources.
 *
 *              Command indices are zero based ordinals in the Mach-O load
 *              command list (the same order as ncmd parsing). Record indices
 *              are zero based into MachO::LoadCommands::Thread::records().
 *
 *              All address/file intervals are half-open. MAIN coordinates are
 *              file offsets: a segment candidate matches through its file
 *              range and yields a virtualAddress. UNIXTHREAD coordinates are
 *              PCs: candidates match through the segment virtual range and may
 *              have no mappedFileOffset when the PC falls in the zero-filled
 *              tail (displacement at or above fileSize but below vmSize).
 *
 *              Every matching segment is kept, in load command order, so
 *              overlapping segments and duplicate addresses are preserved.
 *              A source with no matching segment is still returned with an
 *              empty candidates vector; its original coordinate is not lost.
 */

#ifndef MACHO_ENTRY_POINTS_HPP
#define MACHO_ENTRY_POINTS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace MachO
{
    /*!
     * @enum        EntryPointKind
     * @abstract    Provenance load command kind of an entry source.
     */
    enum class EntryPointKind
    {
        Main,
        UnixThread,

        /* On-disk constant aliases. */
        LC_MAIN       = Main,
        LC_UNIXTHREAD = UnixThread
    };

    /*!
     * @struct      EntryPointCandidate
     * @abstract    One segment mapping candidate for an entry source.
     *
     * @field       segmentIndex     Ordinal of the segment load command.
     * @field       segmentName      Segment name, if available.
     * @field       vmAddress        Segment vmaddr.
     * @field       vmSize           Segment vmsize.
     * @field       fileOffset       Segment fileoff.
     * @field       fileSize         Segment filesize.
     * @field       address          Resolved entry virtual address for this mapping.
     * @field       mappedFileOffset Resolved file coordinate; empty for a zero-fill tail.
     */
    struct EntryPointCandidate
    {
        size_t                     segmentIndex = 0;
        std::string                segmentName;
        uint64_t                   vmAddress   = 0;
        uint64_t                   vmSize      = 0;
        uint64_t                   fileOffset  = 0;
        uint64_t                   fileSize    = 0;
        uint64_t                   address     = 0;
        std::optional< uint64_t >  mappedFileOffset;
    };

    /*!
     * @struct      EntryPointSource
     * @abstract    One entry provenance item, in load command order.
     *
     * @field       commandIndex Zero based ordinal of the load command.
     * @field       recordIndex  Thread record ordinal for LC_UNIXTHREAD sources.
     * @field       kind         LC_MAIN or LC_UNIXTHREAD.
     * @field       command      Raw load command identifier.
     * @field       coordinate   Raw LC_MAIN offset or decoded thread PC.
     * @field       candidates   Every matching segment, in command order.
     */
    struct EntryPointSource
    {
        size_t                                commandIndex = 0;
        std::optional< size_t >               recordIndex;
        EntryPointKind                        kind = EntryPointKind::Main;
        uint32_t                              command = 0;
        uint64_t                              coordinate = 0;
        std::vector< EntryPointCandidate >    candidates;
    };

    /*!
     * @alias       EntryPoint
     * @abstract    Shorter alternate name for an entry source.
     */
    using EntryPoint = EntryPointSource;
}

#endif /* MACHO_ENTRY_POINTS_HPP */
