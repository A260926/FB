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
 * @file        Thread.cpp
 * @copyright   (c) 2020, Jean-David Gadina - www.xs-labs.com
 */

#include <MachO/LoadCommands/Thread.hpp>
#include <MachO/ToString.hpp>
#include <XS.hpp>
#include <limits>
#include <stdexcept>
#include <string>

namespace MachO
{
    namespace LoadCommands
    {
        namespace
        {
            using State = Thread::State;

            enum class Family
            {
                Unknown,
                X86,
                ARM,
                ARM64
            };

            Family familyForCPU( const CPU & cpu )
            {
                /*
                 * The CPU type, not the MachO32/MachO64 kind, selects the
                 * register layout family. ABI bits are part of the type.
                 */
                switch( cpu.type() )
                {
                    case 7:
                    case 0x01000007: return Family::X86;
                    case 12:         return Family::ARM;
                    case 0x0100000c: return Family::ARM64;
                    default:         return Family::Unknown;
                }
            }

            uint32_t readUInt32( const std::vector< uint8_t > & data, size_t offset, bool bigEndian )
            {
                const uint8_t * p = data.data() + offset;

                if( bigEndian )
                {
                    return ( static_cast< uint32_t >( p[ 0 ] ) << 24 )
                         | ( static_cast< uint32_t >( p[ 1 ] ) << 16 )
                         | ( static_cast< uint32_t >( p[ 2 ] ) <<  8 )
                         |   static_cast< uint32_t >( p[ 3 ] );
                }

                return   static_cast< uint32_t >( p[ 0 ] )
                     | ( static_cast< uint32_t >( p[ 1 ] ) <<  8 )
                     | ( static_cast< uint32_t >( p[ 2 ] ) << 16 )
                     | ( static_cast< uint32_t >( p[ 3 ] ) << 24 );
            }

            uint64_t readUInt64( const std::vector< uint8_t > & data, size_t offset, bool bigEndian )
            {
                uint64_t value( 0 );

                if( bigEndian )
                {
                    for( size_t i = 0; i < 8; i++ )
                    {
                        value = ( value << 8 ) | data[ offset + i ];
                    }
                }
                else
                {
                    for( size_t i = 0; i < 8; i++ )
                    {
                        value |= static_cast< uint64_t >( data[ offset + i ] ) << ( 8 * i );
                    }
                }

                return value;
            }

            std::string countError( uint32_t flavor, uint32_t count, uint32_t expected )
            {
                return "Invalid thread record: flavor " + std::to_string( flavor )
                     + " has count " + std::to_string( count )
                     + " (expected " + std::to_string( expected ) + ")";
            }

            std::vector< Thread::Register > readRegisters
            (
                const std::vector< uint8_t > & data,
                size_t                         offset,
                const std::vector< std::string > & names,
                unsigned                       width,
                bool                           bigEndian
            )
            {
                std::vector< Thread::Register > registers;

                registers.reserve( names.size() );

                for( size_t i = 0; i < names.size(); i++ )
                {
                    uint64_t value = ( width == 8 )
                                   ? readUInt64( data, offset + i * width, bigEndian )
                                   : readUInt32( data, offset + i * width, bigEndian );

                    registers.push_back( { names[ i ], value } );
                }

                return registers;
            }

            /*
             * Decodes an i386 record. `data` points at the first register,
             * `size` is the payload size in bytes (already validated).
             */
            State decodeI386( const std::vector< uint8_t > & data, bool bigEndian )
            {
                static const std::vector< std::string > names =
                {
                    "eax", "ebx", "ecx", "edx", "edi", "esi", "ebp", "esp",
                    "ss", "eflags", "eip", "cs", "ds", "es", "fs", "gs"
                };

                auto registers = readRegisters( data, 0, names, 4, bigEndian );

                /* eip is register 10, esp is register 7. */
                return State( Thread::Architecture::I386, registers, registers[ 10 ].value, registers[ 7 ].value );
            }

            State decodeX86_64( const std::vector< uint8_t > & data, bool bigEndian )
            {
                static const std::vector< std::string > names =
                {
                    "rax", "rbx", "rcx", "rdx", "rdi", "rsi", "rbp", "rsp",
                    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
                    "rip", "rflags", "cs", "fs", "gs"
                };

                auto registers = readRegisters( data, 0, names, 8, bigEndian );

                /* rip is register 16, rsp is register 7. */
                return State( Thread::Architecture::X86_64, registers, registers[ 16 ].value, registers[ 7 ].value );
            }

            State decodeARM( const std::vector< uint8_t > & data, bool bigEndian )
            {
                std::vector< std::string > names;

                for( unsigned i = 0; i < 13; i++ )
                {
                    names.push_back( "r" + std::to_string( i ) );
                }

                names.push_back( "sp" );
                names.push_back( "lr" );
                names.push_back( "pc" );
                names.push_back( "cpsr" );

                auto registers = readRegisters( data, 0, names, 4, bigEndian );

                /* pc is register 15, sp is register 13. */
                return State( Thread::Architecture::ARM, registers, registers[ 15 ].value, registers[ 13 ].value );
            }

            State decodeARM64( const std::vector< uint8_t > & data, bool bigEndian )
            {
                std::vector< std::string > names;

                for( unsigned i = 0; i < 29; i++ )
                {
                    names.push_back( "x" + std::to_string( i ) );
                }

                names.push_back( "fp" );
                names.push_back( "lr" );
                names.push_back( "sp" );
                names.push_back( "pc" );

                auto registers = readRegisters( data, 0, names, 8, bigEndian );

                /* cpsr follows the 33 64-bit registers as a single u32. */
                registers.push_back( { "cpsr", readUInt32( data, 33 * 8, bigEndian ) } );

                /* pc is register 32, sp is register 31. */
                return State( Thread::Architecture::ARM64, registers, registers[ 32 ].value, registers[ 31 ].value );
            }

            /*
             * x86 unified flavor 7: u32 inner flavor, u32 inner count, then a
             * fixed 168 byte union. The outer record keeps its identity; only
             * the state is decoded from the inner flavor and count.
             */
            std::optional< State > decodeUnifiedX86
            (
                const std::vector< uint8_t > & data,
                uint32_t                       count,
                bool                           bigEndian
            )
            {
                if( count != 44 )
                {
                    throw std::runtime_error( countError( 7, count, 44 ) );
                }

                uint32_t innerFlavor = readUInt32( data, 0, bigEndian );
                uint32_t innerCount  = readUInt32( data, 4, bigEndian );

                /*
                 * The union is 168 bytes; the inner register data starts right
                 * after the two inner header words. Trailing union bytes are
                 * padding retained in the outer payload.
                 */
                if( innerFlavor == 1 )
                {
                    if( innerCount != 16 )
                    {
                        throw std::runtime_error( countError( innerFlavor, innerCount, 16 ) );
                    }

                    return decodeI386( std::vector< uint8_t >( data.begin() + 8, data.begin() + 8 + 64 ), bigEndian );
                }

                if( innerFlavor == 4 )
                {
                    if( innerCount != 42 )
                    {
                        throw std::runtime_error( countError( innerFlavor, innerCount, 42 ) );
                    }

                    return decodeX86_64( std::vector< uint8_t >( data.begin() + 8, data.begin() + 8 + 168 ), bigEndian );
                }

                /* Unknown inner flavor: the outer record stays undecoded. */
                return {};
            }

            std::optional< State > decodeRecord
            (
                Family                         family,
                uint32_t                       flavor,
                uint32_t                       count,
                const std::vector< uint8_t > & payload,
                bool                           bigEndian
            )
            {
                switch( family )
                {
                    case Family::X86:

                        switch( flavor )
                        {
                            case 1:

                                if( count != 16 )
                                {
                                    throw std::runtime_error( countError( flavor, count, 16 ) );
                                }

                                return decodeI386( payload, bigEndian );

                            case 4:

                                if( count != 42 )
                                {
                                    throw std::runtime_error( countError( flavor, count, 42 ) );
                                }

                                return decodeX86_64( payload, bigEndian );

                            case 7: return decodeUnifiedX86( payload, count, bigEndian );

                            default: return {};
                        }

                    case Family::ARM:

                        switch( flavor )
                        {
                            case 1:

                                if( count != 17 )
                                {
                                    throw std::runtime_error( countError( flavor, count, 17 ) );
                                }

                                return decodeARM( payload, bigEndian );

                            default: return {};
                        }

                    case Family::ARM64:

                        switch( flavor )
                        {
                            case 6:

                                if( count != 68 )
                                {
                                    throw std::runtime_error( countError( flavor, count, 68 ) );
                                }

                                return decodeARM64( payload, bigEndian );

                            default: return {};
                        }

                    case Family::Unknown: return {};
                }

                return {};
            }
        }

        class Thread::IMPL
        {
            public:

                IMPL( uint32_t command, uint32_t size, File::Kind kind, const CPU & cpu, XS::IO::BinaryStream & stream );
                IMPL( const IMPL & o );
                ~IMPL();

                uint32_t              _command;
                uint32_t              _size;
                std::vector< Record > _records;
        };

        State::State():
            _architecture( Architecture::Unknown ),
            _pc(           0 ),
            _sp(           0 )
        {}

        State::State( Architecture architecture, const std::vector< Register > & registers, uint64_t pc, uint64_t sp ):
            _architecture( architecture ),
            _registers(    registers ),
            _pc(           pc ),
            _sp(           sp )
        {}

        Thread::Architecture State::architecture() const
        {
            return this->_architecture;
        }

        std::string State::architectureName() const
        {
            switch( this->_architecture )
            {
                case Architecture::I386:   return "i386";
                case Architecture::X86_64: return "x86_64";
                case Architecture::ARM:    return "ARM";
                case Architecture::ARM64:  return "ARM64";
                case Architecture::Unknown: return "Unknown";
            }

            return "Unknown";
        }

        std::vector< Thread::Register > State::registers() const
        {
            return this->_registers;
        }

        std::optional< uint64_t > State::registerValue( const std::string & name ) const
        {
            for( const auto & reg: this->_registers )
            {
                if( reg.name == name )
                {
                    return reg.value;
                }
            }

            return {};
        }

        uint64_t State::pc() const
        {
            return this->_pc;
        }

        uint64_t State::sp() const
        {
            return this->_sp;
        }

        Thread::Thread( uint32_t command, uint32_t size, File::Kind kind, XS::IO::BinaryStream & stream ):
            impl( std::make_unique< IMPL >( command, size, kind, CPU(), stream ) )
        {}

        Thread::Thread( uint32_t command, uint32_t size, File::Kind kind, const CPU & cpu, XS::IO::BinaryStream & stream ):
            impl( std::make_unique< IMPL >( command, size, kind, cpu, stream ) )
        {}

        Thread::Thread( const Thread & o ):
            impl( std::make_unique< IMPL >( *( o.impl ) ) )
        {}

        Thread::Thread( Thread && o ) noexcept:
            impl( std::move( o.impl ) )
        {}

        Thread::~Thread()
        {}

        Thread & Thread::operator =( Thread o )
        {
            swap( *( this ), o );

            return *( this );
        }

        XS::Info Thread::getInfo() const
        {
            XS::Info i( LoadCommand::getInfo() );

            i.addChild( { "Records", std::to_string( this->impl->_records.size() ) } );

            for( size_t n = 0; n < this->impl->_records.size(); n++ )
            {
                const auto & record = this->impl->_records[ n ];
                XS::Info     info( "Record " + std::to_string( n ) );

                info.addChild( { "Offset", XS::ToString::Hex( static_cast< uint64_t >( record.offset ) ) } );
                info.addChild( { "Flavor", XS::ToString::Hex( static_cast< uint64_t >( record.flavor ) ) } );
                info.addChild( { "Count",  std::to_string( record.count ) } );

                if( record.state.has_value() )
                {
                    const auto & state = *( record.state );
                    XS::Info     regs( "Registers" );

                    info.addChild( { "Architecture", state.architectureName() } );

                    for( const auto & reg: state.registers() )
                    {
                        regs.addChild( { reg.name, XS::ToString::Hex( reg.value ) } );
                    }

                    regs.value( std::to_string( state.registers().size() ) );

                    info.addChild( regs );
                    info.addChild( { "PC", XS::ToString::Hex( state.pc() ) } );
                    info.addChild( { "SP", XS::ToString::Hex( state.sp() ) } );
                }
                else
                {
                    info.addChild( { "State", "raw" } );
                }

                i.addChild( info );
            }

            return i;
        }

        uint32_t Thread::command() const
        {
            return this->impl->_command;
        }

        uint32_t Thread::size() const
        {
            return this->impl->_size;
        }

        std::vector< Thread::Record > Thread::records() const
        {
            return this->impl->_records;
        }

        void swap( Thread & o1, Thread & o2 )
        {
            using std::swap;

            swap( o1.impl, o2.impl );
        }

        Thread::IMPL::IMPL( uint32_t command, uint32_t size, File::Kind kind, const CPU & cpu, XS::IO::BinaryStream & stream ):
            _command( command ),
            _size(    size )
        {
            ( void )kind;

            if( size < 8 )
            {
                throw std::runtime_error( "Invalid thread load command size" );
            }

            bool   bigEndian = ( stream.preferredEndianness() == XS::IO::BinaryStream::Endianness::BigEndian );
            size_t payloadSize = static_cast< size_t >( size ) - 8;
            std::vector< uint8_t > payload;

            try
            {
                payload = stream.read( payloadSize );
            }
            catch( const std::exception & e )
            {
                throw std::runtime_error( "Invalid thread load command payload: " + std::string( e.what() ) );
            }

            Family family = familyForCPU( cpu );
            size_t pos    = 0;

            while( pos < payload.size() )
            {
                size_t remaining = payload.size() - pos;

                if( remaining < 8 )
                {
                    throw std::runtime_error
                    (
                        "Truncated thread record header at command offset "
                        + std::to_string( 8 + pos )
                    );
                }

                uint32_t flavor = readUInt32( payload, pos,     bigEndian );
                uint32_t count  = readUInt32( payload, pos + 4, bigEndian );

                /*
                 * count is expressed in 4 byte units. Compute the payload
                 * span with 64 bit arithmetic so a large count cannot overflow
                 * the check below.
                 */
                uint64_t dataSize = static_cast< uint64_t >( count ) * 4;

                if( dataSize > static_cast< uint64_t >( remaining - 8 ) )
                {
                    throw std::runtime_error
                    (
                        "Thread record at command offset "
                        + std::to_string( 8 + pos )
                        + " extends past the load command"
                    );
                }

                Record record;

                record.offset  = static_cast< uint32_t >( 8 + pos );
                record.flavor  = flavor;
                record.count   = count;
                record.payload.assign
                (
                    payload.begin() + static_cast< ssize_t >( pos + 8 ),
                    payload.begin() + static_cast< ssize_t >( pos + 8 + dataSize )
                );

                record.state = decodeRecord( family, flavor, count, record.payload, bigEndian );

                this->_records.push_back( record );

                pos += 8 + static_cast< size_t >( dataSize );
            }
        }

        Thread::IMPL::IMPL( const IMPL & o ):
            _command( o._command ),
            _size(    o._size ),
            _records( o._records )
        {}

        Thread::IMPL::~IMPL()
        {}
    }
}
