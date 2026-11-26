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
 * @file        ThreadState.cpp
 * @copyright   (c) 2020, Jean-David Gadina - www.xs-labs.com
 */

#include <MachO/ThreadState.hpp>

namespace MachO
{
    ThreadContext::ThreadContext( ThreadArchitecture architecture,
                                  uint32_t outerFlavor,
                                  std::optional< uint32_t > innerFlavor,
                                  std::vector< ThreadRegister > registers,
                                  uint64_t pc,
                                  uint64_t sp ):
        _architecture( std::move( architecture ) ),
        _flavor( outerFlavor ),
        _innerFlavor( std::move( innerFlavor ) ),
        _registers( std::move( registers ) ),
        _pc( pc ),
        _sp( sp )
    {}

    ThreadArchitecture ThreadContext::architecture() const
    {
        return this->_architecture;
    }

    std::string ThreadContext::architectureName() const
    {
        switch( this->_architecture )
        {
            case ThreadArchitecture::I386:   return "i386";
            case ThreadArchitecture::X86_64: return "x86_64";
            case ThreadArchitecture::ARM:    return "arm";
            case ThreadArchitecture::ARM64:  return "arm64";
            case ThreadArchitecture::Unknown: break;
        }

        return "unknown";
    }

    uint32_t ThreadContext::flavor() const
    {
        return this->_flavor;
    }

    std::optional< uint32_t > ThreadContext::innerFlavor() const
    {
        return this->_innerFlavor;
    }

    const std::vector< ThreadRegister > & ThreadContext::registers() const
    {
        return this->_registers;
    }

    std::optional< uint64_t > ThreadContext::registerValue( const std::string & name ) const
    {
        for( const auto & reg: this->_registers )
        {
            if( reg.name == name )
            {
                return reg.value;
            }
        }

        return std::nullopt;
    }

    uint64_t ThreadContext::pc() const
    {
        return this->_pc;
    }

    uint64_t ThreadContext::sp() const
    {
        return this->_sp;
    }

    ThreadRecord::ThreadRecord( uint32_t flavor,
                                uint32_t count,
                                uint32_t offset,
                                std::vector< uint8_t > bytes,
                                std::vector< uint8_t > payload,
                                std::optional< ThreadContext > context ):
        _flavor( flavor ),
        _count( count ),
        _offset( offset ),
        _bytes( std::move( bytes ) ),
        _payload( std::move( payload ) ),
        _context( std::move( context ) )
    {}

    uint32_t ThreadRecord::flavor() const
    {
        return this->_flavor;
    }

    uint32_t ThreadRecord::count() const
    {
        return this->_count;
    }

    uint32_t ThreadRecord::offset() const
    {
        return this->_offset;
    }

    uint32_t ThreadRecord::offsetInCommand() const
    {
        return this->_offset;
    }

    const std::vector< uint8_t > & ThreadRecord::bytes() const
    {
        return this->_bytes;
    }

    const std::vector< uint8_t > & ThreadRecord::payload() const
    {
        return this->_payload;
    }

    const std::optional< ThreadContext > & ThreadRecord::context() const
    {
        return this->_context;
    }

    const std::optional< ThreadContext > & ThreadRecord::state() const
    {
        return this->_context;
    }

    bool ThreadRecord::decoded() const
    {
        return this->_context.has_value();
    }
}
