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
 * @header      ThreadState.hpp
 * @copyright   (c) 2020, Jean-David Gadina - www.xs-labs.com
 *
 * @abstract    Owned value types for LC_THREAD / LC_UNIXTHREAD records and
 *              their decoded register contexts.
 *
 * @discussion  Every value in these types is owned (no stream references), so
 *              copies of a MachO::Thread / MachO::File and destruction of the
 *              originating input stream keep the data readable.
 */

#ifndef MACHO_THREAD_STATE_HPP
#define MACHO_THREAD_STATE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace MachO
{
    /*!
     * @enum        ThreadArchitecture
     * @abstract    CPU architecture a decoded thread state belongs to.
     */
    enum class ThreadArchitecture
    {
        Unknown,
        I386,
        X86_64,
        ARM,
        ARM64
    };

    /*!
     * @struct      ThreadRegister
     * @abstract    One decoded register, using its original ABI name.
     * @discussion  32 bit registers are stored zero-extended in `value`.
     */
    struct ThreadRegister
    {
        std::string name;
        uint64_t    value = 0;
    };

    /*!
     * @class       ThreadContext
     * @abstract    Decoded register state of one thread record.
     *
     * @discussion  For an x86 unified (flavor 7) record, `flavor()` keeps the
     *              outer record identity (7) while `innerFlavor()` reports the
     *              flavor (1 or 4) that selected the actual layout.
     */
    class ThreadContext
    {
        public:

            ThreadContext() = default;
            ThreadContext( ThreadArchitecture architecture,
                           uint32_t outerFlavor,
                           std::optional< uint32_t > innerFlavor,
                           std::vector< ThreadRegister > registers,
                           uint64_t pc,
                           uint64_t sp );

            ThreadArchitecture                   architecture()   const;
            std::string                          architectureName() const;
            uint32_t                             flavor()         const;
            std::optional< uint32_t >            innerFlavor()    const;
            const std::vector< ThreadRegister > & registers()     const;
            std::optional< uint64_t >            registerValue( const std::string & name ) const;
            uint64_t                             pc()             const;
            uint64_t                             sp()             const;

        private:

            ThreadArchitecture        _architecture = ThreadArchitecture::Unknown;
            uint32_t                  _flavor       = 0;
            std::optional< uint32_t > _innerFlavor;
            std::vector< ThreadRegister > _registers;
            uint64_t                  _pc = 0;
            uint64_t                  _sp = 0;
    };

    /*!
     * @alias       ThreadState
     * @abstract    Alternate name for the decoded context.
     */
    using ThreadState = ThreadContext;

    /*!
     * @class       ThreadRecord
     * @abstract    One flavor/count record inside a thread load command.
     *
     * @discussion  `offset()` is measured from the first byte of the load
     *              command, so the first record starts at offset 8 (after the
     *              cmd/cmdsize header). `count()` is expressed in 4 byte
     *              units, as on disk. `bytes()` owns the whole record
     *              (flavor, count and payload) in the image's byte order.
     *              `context()` is empty for unknown flavors and for records
     *              produced by the legacy CPU-less constructor.
     */
    class ThreadRecord
    {
        public:

            ThreadRecord( uint32_t flavor,
                          uint32_t count,
                          uint32_t offset,
                          std::vector< uint8_t > bytes,
                          std::vector< uint8_t > payload,
                          std::optional< ThreadContext > context );

            uint32_t                             flavor()          const;
            uint32_t                             count()           const;
            uint32_t                             offset()          const;
            uint32_t                             offsetInCommand() const;
            const std::vector< uint8_t > &       bytes()           const;
            const std::vector< uint8_t > &       payload()         const;
            const std::optional< ThreadContext > & context()       const;
            const std::optional< ThreadContext > & state()         const;
            bool                                 decoded()         const;

        private:

            uint32_t                       _flavor = 0;
            uint32_t                       _count  = 0;
            uint32_t                       _offset = 0;
            std::vector< uint8_t >         _bytes;
            std::vector< uint8_t >         _payload;
            std::optional< ThreadContext > _context;
    };
}

#endif /* MACHO_THREAD_STATE_HPP */
