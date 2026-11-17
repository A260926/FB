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
 * @header      File.hpp
 * @copyright   (c) 2020, Jean-David Gadina - www.xs-labs.com
 */

#ifndef MACHO_FILE_HPP
#define MACHO_FILE_HPP

#include <memory>
#include <algorithm>
#include <string>
#include <vector>
#include <optional>
#include <XS.hpp>
#include <MachO/LoadCommand.hpp>
#include <MachO/FileFlags.hpp>
#include <MachO/FileType.hpp>
#include <MachO/CPU.hpp>

namespace MachO
{
    class Symbol;
    
    class File: public XS::Info::Object
    {
        public:
            
            enum class Kind
            {
                MachO32,
                MachO64
            };
            
            enum class Endianness
            {
                LittleEndian,
                BigEndian
            };

            /*!
             * How an entry source was discovered.
             *
             * Main       - one LC_MAIN command; coordinate() is its raw file
             *              offset (relative to this Mach-O image).
             * UnixThread - one decoded record of an LC_UNIXTHREAD command;
             *              coordinate() is the record's PC.
             *
             * LC_THREAD records never produce entry sources; they only carry
             * thread state.
             */
            enum class EntryPointKind
            {
                Main,
                UnixThread
            };

            /*!
             * One segment that maps the source coordinate.
             *
             * commandIndex identifies the segment load command by its position
             * in loadCommands(). All matching segments are reported, including
             * overlapping segments and different sources mapping the same
             * address.
             *
             * address    - the virtual address corresponding to the source
             *              coordinate. Always present.
             * fileOffset - the corresponding file coordinate inside this
             *              Mach-O image, or an empty optional when the
             *              coordinate lies in the zero-fill tail of the segment
             *              (or, for LC_MAIN, outside the segment's file
             *              contents).
             *
             * All segment intervals are half-open.
             */
            struct SegmentMapping
            {
                size_t                   commandIndex;
                std::string              name;
                uint64_t                 vmAddress;
                uint64_t                 vmSize;
                uint64_t                 fileOffset;
                uint64_t                 fileSize;
                uint64_t                 address;
                std::optional< uint64_t > mappedFileOffset;
            };

            /*!
             * One startup source, in load command order.
             *
             * commandIndex - index of the originating load command in
             *                loadCommands().
             * recordIndex  - for UnixThread sources, the index of the decoded
             *                record inside Thread::records(). Empty for Main.
             * kind         - the source command kind.
             * coordinate   - the raw coordinate as read from the image: the
             *                LC_MAIN file offset, or the thread record's PC.
             *                Preserved even when no segment maps it.
             * segments     - every segment mapping candidate, in load command
             *                order. Empty when no segment matches.
             *
             * No symbol lookup, dependency traversal or address-space
             * allocation is performed and the image bytes are not modified.
             */
            struct EntryPointSource
            {
                size_t                            commandIndex;
                std::optional< size_t >           recordIndex;
                EntryPointKind                    kind;
                uint64_t                          coordinate;
                std::vector< SegmentMapping >     segments;
            };

            #ifdef __APPLE__
            static std::optional< std::tuple< File, const void * > > fromCurrentProcess( const std::string & path );
            #endif

            File( const std::string & path );
            File( XS::IO::BinaryStream & stream );
            File( const File & o );
            File( File && o ) noexcept;
            ~File() override;
            
            File & operator =( File o );
            
            XS::Info getInfo() const override;
            
            std::optional< std::string > path()       const;
            Kind                         kind()       const;
            Endianness                   endianness() const;
            CPU                          cpu()        const;
            FileType                     type()       const;
            FileFlags                    flags()      const;
            
            std::vector< std::reference_wrapper< LoadCommand > > loadCommands()    const;
            std::vector< EntryPointSource >                     entryPoints()     const;
            std::vector< std::string >                           linkedLibraries() const;
            std::vector< Symbol >                                symbols()         const;
            std::vector< std::string >                           strings()         const;
            std::vector< std::string >                           objcClasses()     const;
            std::vector< std::string >                           objcMethods()     const;
            
            template< typename T, typename std::enable_if< std::is_base_of< LoadCommand, T >::value >::type * = nullptr >
            std::vector< T > loadCommands() const
            {
                std::vector< T > commands;
                
                for( const auto & p: this->loadCommands() )
                {
                    try
                    {
                        commands.push_back( dynamic_cast< T & >( p.get() ) );
                    }
                    catch( ... )
                    {}
                }
                
                return commands;
            }
            
            friend void swap( File & o1, File & o2 );
            
        private:
            
            class IMPL;
            
            std::unique_ptr< IMPL > impl;
    };
}

#endif /* MACHO_FILE_HPP */
