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
 * @header      Thread.hpp
 * @copyright   (c) 2020, Jean-David Gadina - www.xs-labs.com
 */

#ifndef MACHO_LOAD_COMMANDS_THREAD_HPP
#define MACHO_LOAD_COMMANDS_THREAD_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <MachO/LoadCommand.hpp>
#include <MachO/File.hpp>
#include <MachO/CPU.hpp>
#include <XS.hpp>

namespace MachO
{
    namespace LoadCommands
    {
        /*!
         * LC_THREAD and LC_UNIXTHREAD.
         *
         * The payload of both commands is a sequence of records:
         *
         *     u32 flavor, u32 count, count * 4 bytes of state data
         *
         * Every record is copied out of the command, so records() stays usable
         * after the originating stream has been released. Record offsets are
         * relative to the start of the load command and include the 8 byte
         * command header, so the first record normally starts at offset 8.
         *
         * A record may additionally carry a decoded register state when the
         * current image CPU selects a known flavor/layout. The CPU - not the
         * MachO32/MachO64 kind - decides which family a flavor belongs to:
         * flavor 1 is i386 for x86 CPUs and ARM for ARM CPUs.
         *
         * The legacy four-argument constructor has no CPU information, so it
         * preserves the raw records without any decoded state. The File parser
         * uses the CPU-aware overload.
         */
        class Thread: public LoadCommand
        {
            public:

                enum class Architecture
                {
                    Unknown,
                    I386,
                    X86 = I386,     /*!< Convenience alias: flavor 1 on x86.   */
                    X86_64,
                    ARM,
                    ARM64,
                    AArch64 = ARM64 /*!< Convenience alias for ARM64.          */
                };

                struct Register
                {
                    std::string name;
                    uint64_t    value;
                };

                /*!
                 * Decoded register state. Register values are unsigned and are
                 * never sign-extended. The register order follows the on-disk
                 * layout, using the original register names.
                 */
                class State
                {
                    public:

                        State();
                        State( Architecture architecture, const std::vector< Register > & registers, uint64_t pc, uint64_t sp );

                        Architecture architecture() const;
                        std::string  architectureName() const;

                        std::vector< Register > registers() const;

                        /*!
                         * Looks a register up by its original name. Returns an
                         * empty optional when the state has no such register.
                         */
                        std::optional< uint64_t > registerValue( const std::string & name ) const;

                        uint64_t pc() const;
                        uint64_t sp() const;

                    private:

                        Architecture            _architecture;
                        std::vector< Register > _registers;
                        uint64_t                _pc;
                        uint64_t                _sp;
                };

                struct Record
                {
                    /*! Offset in bytes from the start of the load command. */
                    uint32_t offset;

                    /*! Flavor exactly as stored in the record. */
                    uint32_t flavor;

                    /*! Count exactly as stored in the record, in 4 byte units. */
                    uint32_t count;

                    /*! The count * 4 raw payload bytes owned by this record. */
                    std::vector< uint8_t > payload;

                    /*!
                     * Decoded state, or an empty optional for unknown flavors,
                     * unknown CPUs and flavors belonging to another CPU family.
                     */
                    std::optional< State > state;
                };

                Thread( uint32_t command, uint32_t size, File::Kind kind, XS::IO::BinaryStream & stream );
                Thread( uint32_t command, uint32_t size, File::Kind kind, const CPU & cpu, XS::IO::BinaryStream & stream );
                Thread( const Thread & o );
                Thread( Thread && o ) noexcept;
                ~Thread() override;

                Thread & operator =( Thread o );

                XS::Info getInfo() const override;

                uint32_t command() const override;
                uint32_t size()    const override;

                /*!
                 * Returns the records in their original on-disk order. Each
                 * record owns its payload bytes and refers to the command it
                 * was parsed from through its offset.
                 */
                std::vector< Record > records() const;

                friend void swap( Thread & o1, Thread & o2 );

            private:

                class IMPL;

                std::unique_ptr< IMPL > impl;
        };
    }
}

#endif /* MACHO_LOAD_COMMANDS_THREAD_HPP */
