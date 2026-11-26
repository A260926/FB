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

namespace MachO
{
    namespace
    {
        /* Raw cputype values from <mach/machine.h>. The ABI64 bit is kept. */
        constexpr uint32_t CPU_TYPE_X86     = 7;
        constexpr uint32_t CPU_TYPE_X86_64  = 0x01000007;
        constexpr uint32_t CPU_TYPE_ARM     = 12;
        constexpr uint32_t CPU_TYPE_ARM64   = 0x0100000C;

        uint64_t loadInteger( const uint8_t * p, size_t n, bool bigEndian )
        {
            uint64_t v = 0;

            if( bigEndian )
            {
                for( size_t i = 0; i < n; ++i )
                {
                    v = ( v << 8 ) | p[ i ];
                }
            }
            else
            {
                for( size_t i = 0; i < n; ++i )
                {
                    v |= static_cast< uint64_t >( p[ i ] ) << ( 8 * i );
                }
            }

            return v;
        }

        uint32_t u32( const std::vector< uint8_t > & b, size_t off, bool bigEndian )
        {
            return static_cast< uint32_t >( loadInteger( b.data() + off, 4, bigEndian ) );
        }

        uint64_t u64( const std::vector< uint8_t > & b, size_t off, bool bigEndian )
        {
            return loadInteger( b.data() + off, 8, bigEndian );
        }

        uint64_t checkedAdd( uint64_t a, uint64_t b, const std::string & what )
        {
            if( b > std::numeric_limits< uint64_t >::max() - a )
            {
                throw std::runtime_error( "Mach-O thread command overflow while computing " + what );
            }

            return a + b;
        }

        bool hostIsBigEndian()
        {
            const uint16_t v = 0x0102;

            return *reinterpret_cast< const uint8_t * >( &v ) == 0x01;
        }

        std::vector< ThreadRegister > decodeI386( const std::vector< uint8_t > & p, bool bigEndian )
        {
            static const char * names[] =
            {
                "eax", "ebx", "ecx", "edx", "edi", "esi", "ebp", "esp",
                "ss", "eflags", "eip", "cs", "ds", "es", "fs", "gs"
            };

            std::vector< ThreadRegister > regs;

            regs.reserve( 16 );

            for( size_t i = 0; i < 16; ++i )
            {
                /* 32 bit values are deliberately not sign extended. */
                regs.push_back( { names[ i ], static_cast< uint64_t >( u32( p, i * 4, bigEndian ) ) } );
            }

            return regs;
        }

        std::vector< ThreadRegister > decodeX86_64( const std::vector< uint8_t > & p, bool bigEndian )
        {
            static const char * names[] =
            {
                "rax", "rbx", "rcx", "rdx", "rdi", "rsi", "rbp", "rsp",
                "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
                "rip", "rflags", "cs", "fs", "gs"
            };

            std::vector< ThreadRegister > regs;

            regs.reserve( 21 );

            for( size_t i = 0; i < 21; ++i )
            {
                regs.push_back( { names[ i ], u64( p, i * 8, bigEndian ) } );
            }

            return regs;
        }

        std::vector< ThreadRegister > decodeARM( const std::vector< uint8_t > & p, bool bigEndian )
        {
            std::vector< ThreadRegister > regs;

            regs.reserve( 17 );

            for( int i = 0; i <= 12; ++i )
            {
                regs.push_back( { "r" + std::to_string( i ), static_cast< uint64_t >( u32( p, static_cast< size_t >( i ) * 4, bigEndian ) ) } );
            }

            static const char * named[] = { "sp", "lr", "pc", "cpsr" };

            for( int i = 0; i < 4; ++i )
            {
                regs.push_back( { named[ i ], static_cast< uint64_t >( u32( p, static_cast< size_t >( 13 + i ) * 4, bigEndian ) ) } );
            }

            return regs;
        }

        std::vector< ThreadRegister > decodeARM64( const std::vector< uint8_t > & p, bool bigEndian )
        {
            std::vector< ThreadRegister > regs;

            regs.reserve( 34 );

            /* x0 .. x28 (29 registers), then fp, lr, sp, pc. */
            for( int i = 0; i <= 28; ++i )
            {
                regs.push_back( { "x" + std::to_string( i ), u64( p, static_cast< size_t >( i ) * 8, bigEndian ) } );
            }

            static const char * named[] = { "fp", "lr", "sp", "pc" };

            for( int i = 0; i < 4; ++i )
            {
                regs.push_back( { named[ i ], u64( p, static_cast< size_t >( 29 + i ) * 8, bigEndian ) } );
            }

            /* cpsr is a 32 bit register right after the 33 64 bit ones. */
            regs.push_back( { "cpsr", static_cast< uint64_t >( u32( p, 33 * 8, bigEndian ) ) } );

            /* The final 4 bytes are padding and intentionally not a register. */

            return regs;
        }

        uint64_t pcFor( ThreadArchitecture arch, const std::vector< ThreadRegister > & regs )
        {
            switch( arch )
            {
                case ThreadArchitecture::I386:   return regs[ 10 ].value; /* eip */
                case ThreadArchitecture::X86_64: return regs[ 16 ].value; /* rip */
                case ThreadArchitecture::ARM:    return regs[ 15 ].value; /* pc  */
                case ThreadArchitecture::ARM64:  return regs[ 32 ].value; /* pc  */
                case ThreadArchitecture::Unknown: break;
            }

            return 0;
        }

        uint64_t spFor( ThreadArchitecture arch, const std::vector< ThreadRegister > & regs )
        {
            switch( arch )
            {
                case ThreadArchitecture::I386:   return regs[ 7 ].value;  /* esp */
                case ThreadArchitecture::X86_64: return regs[ 7 ].value;  /* rsp */
                case ThreadArchitecture::ARM:    return regs[ 13 ].value; /* sp  */
                case ThreadArchitecture::ARM64:  return regs[ 31 ].value; /* sp  */
                case ThreadArchitecture::Unknown: break;
            }

            return 0;
        }

        enum class KnownKind
        {
            I386,
            X86_64,
            UnifiedX86,
            ARM,
            ARM64
        };

        std::optional< KnownKind > knownKind( uint32_t cpuType, uint32_t flavor )
        {
            switch( cpuType )
            {
                case CPU_TYPE_X86:
                    if( flavor == 1 ) { return KnownKind::I386; }
                    if( flavor == 7 ) { return KnownKind::UnifiedX86; }
                    break;

                case CPU_TYPE_X86_64:
                    if( flavor == 4 ) { return KnownKind::X86_64; }
                    if( flavor == 7 ) { return KnownKind::UnifiedX86; }
                    break;

                case CPU_TYPE_ARM:
                    if( flavor == 1 ) { return KnownKind::ARM; }
                    break;

                case CPU_TYPE_ARM64:
                    if( flavor == 6 ) { return KnownKind::ARM64; }
                    break;

                default:
                    break;
            }

            return std::nullopt;
        }

        uint32_t expectedCount( KnownKind kind )
        {
            switch( kind )
            {
                case KnownKind::I386:       return 16;
                case KnownKind::X86_64:     return 42;
                case KnownKind::UnifiedX86: return 44;
                case KnownKind::ARM:        return 17;
                case KnownKind::ARM64:      return 68;
            }

            return 0;
        }

        /*
         * Decodes one contained, CPU known record. A recognized state with a
         * wrong count is a hard error; an unrecognized flavor simply yields no
         * context.
         */
        std::optional< ThreadContext > decodeRecord( uint32_t cpuType,
                                                     uint32_t flavor,
                                                     uint32_t count,
                                                     const std::vector< uint8_t > & payload,
                                                     bool bigEndian,
                                                     size_t recordIndex )
        {
            std::optional< KnownKind > kind = knownKind( cpuType, flavor );

            if( kind.has_value() == false )
            {
                /* Unknown flavor for this CPU family: preserve raw bytes only. */
                return std::nullopt;
            }

            KnownKind k = *kind;

            if( count != expectedCount( k ) )
            {
                throw std::runtime_error(
                    "Mach-O thread record #" + std::to_string( recordIndex )
                    + " has an invalid state length: flavor " + std::to_string( flavor )
                    + ", count " + std::to_string( count )
                    + " (expected " + std::to_string( expectedCount( k ) ) + ")"
                );
            }

            if( k == KnownKind::UnifiedX86 )
            {
                /* count 44 -> 176 payload bytes: inner flavor/count + 168 byte union. */
                uint32_t innerFlavor = u32( payload, 0, bigEndian );
                uint32_t innerCount  = u32( payload, 4, bigEndian );

                /* The fixed union starts at byte 8. */
                std::vector< uint8_t > unionBytes( payload.begin() + 8, payload.begin() + 8 + 168 );

                ThreadArchitecture   arch;
                std::vector< ThreadRegister > regs;

                if( innerFlavor == 1 )
                {
                    if( innerCount != 16 )
                    {
                        throw std::runtime_error(
                            "Mach-O x86 unified thread record #" + std::to_string( recordIndex )
                            + " has an invalid inner i386 count " + std::to_string( innerCount )
                            + " (expected 16)"
                        );
                    }

                    arch = ThreadArchitecture::I386;
                    regs = decodeI386( unionBytes, bigEndian );
                }
                else if( innerFlavor == 4 )
                {
                    if( innerCount != 42 )
                    {
                        throw std::runtime_error(
                            "Mach-O x86 unified thread record #" + std::to_string( recordIndex )
                            + " has an invalid inner x86_64 count " + std::to_string( innerCount )
                            + " (expected 42)"
                        );
                    }

                    arch = ThreadArchitecture::X86_64;
                    regs = decodeX86_64( unionBytes, bigEndian );
                }
                else
                {
                    /*
                     * Unknown inner flavor: the whole outer record stays
                     * undecoded. Do not manufacture an extra record.
                     */
                    return std::nullopt;
                }

                uint64_t pc = pcFor( arch, regs );
                uint64_t sp = spFor( arch, regs );

                return ThreadContext( arch, flavor, innerFlavor, std::move( regs ), pc, sp );
            }

            ThreadArchitecture       arch;
            std::vector< ThreadRegister > regs;

            switch( k )
            {
                case KnownKind::I386:
                    arch = ThreadArchitecture::I386;
                    regs = decodeI386( payload, bigEndian );
                    break;

                case KnownKind::X86_64:
                    arch = ThreadArchitecture::X86_64;
                    regs = decodeX86_64( payload, bigEndian );
                    break;

                case KnownKind::ARM:
                    arch = ThreadArchitecture::ARM;
                    regs = decodeARM( payload, bigEndian );
                    break;

                case KnownKind::ARM64:
                    arch = ThreadArchitecture::ARM64;
                    regs = decodeARM64( payload, bigEndian );
                    break;

                case KnownKind::UnifiedX86:
                    /* Handled above. */
                    return std::nullopt;
            }

            /* Compute before moving the register vector into the context. */
            uint64_t pc = pcFor( arch, regs );
            uint64_t sp = spFor( arch, regs );

            return ThreadContext( arch, flavor, std::nullopt, std::move( regs ), pc, sp );
        }
    }

    namespace LoadCommands
    {
        class Thread::IMPL
        {
            public:

                IMPL( uint32_t command, uint32_t size, File::Kind kind, std::optional< uint32_t > cpuType, XS::IO::BinaryStream & stream );
                IMPL( const IMPL & o );
                ~IMPL();

                uint32_t                    _command;
                uint32_t                    _size;
                std::vector< ThreadRecord > _records;
        };

        Thread::Thread( uint32_t command, uint32_t size, File::Kind kind, XS::IO::BinaryStream & stream ):
            impl( std::make_unique< IMPL >( command, size, kind, std::nullopt, stream ) )
        {}

        Thread::Thread( uint32_t command, uint32_t size, File::Kind kind, uint32_t cpuType, XS::IO::BinaryStream & stream ):
            impl( std::make_unique< IMPL >( command, size, kind, cpuType, stream ) )
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
            XS::Info info( LoadCommand::getInfo() );
            XS::Info recordsInfo( "Thread records", std::to_string( this->impl->_records.size() ) );

            size_t index = 0;

            for( const auto & record: this->impl->_records )
            {
                XS::Info recordInfo( "Record " + std::to_string( index ) );

                recordInfo.addChild( { "Flavor", std::to_string( record.flavor() ) + " (0x" + XS::ToString::Hex( static_cast< uint64_t >( record.flavor() ), false ) + ")" } );
                recordInfo.addChild( { "Count", std::to_string( record.count() ) } );
                recordInfo.addChild( { "Offset", XS::ToString::Hex( static_cast< uint64_t >( record.offset() ) ) } );
                recordInfo.addChild( { "Payload bytes", std::to_string( record.payload().size() ) } );

                const auto & context = record.context();

                if( context.has_value() )
                {
                    XS::Info registersInfo( "Registers", context->architectureName() );

                    for( const auto & reg: context->registers() )
                    {
                        registersInfo.addChild( { reg.name, XS::ToString::Hex( reg.value ) } );
                    }

                    recordInfo.addChild( registersInfo );
                    recordInfo.addChild( { "PC", XS::ToString::Hex( context->pc() ) } );
                    recordInfo.addChild( { "SP", XS::ToString::Hex( context->sp() ) } );
                }
                else
                {
                    recordInfo.addChild( { "Decoded state", "unknown" } );
                }

                recordsInfo.addChild( recordInfo );
                ++index;
            }

            info.addChild( recordsInfo );

            return info;
        }

        uint32_t Thread::command() const
        {
            return this->impl->_command;
        }

        uint32_t Thread::size() const
        {
            return this->impl->_size;
        }

        std::vector< ThreadRecord > Thread::records() const
        {
            return this->impl->_records;
        }

        void swap( Thread & o1, Thread & o2 )
        {
            using std::swap;

            swap( o1.impl, o2.impl );
        }

        Thread::IMPL::IMPL( uint32_t command, uint32_t size, File::Kind kind, std::optional< uint32_t > cpuType, XS::IO::BinaryStream & stream ):
            _command( command ),
            _size(    size )
        {
            ( void )kind;

            if( size < 8 )
            {
                throw std::runtime_error( "Invalid Mach-O thread command: size " + std::to_string( size ) + " is smaller than the 8 byte command header" );
            }

            XS::IO::BinaryStream::Endianness preferred = stream.preferredEndianness();
            bool bigEndian;

            if( preferred == XS::IO::BinaryStream::Endianness::BigEndian )
            {
                bigEndian = true;
            }
            else if( preferred == XS::IO::BinaryStream::Endianness::LittleEndian )
            {
                bigEndian = false;
            }
            else
            {
                bigEndian = hostIsBigEndian();
            }

            /*
             * The stream is positioned right after cmd/cmdsize. `used` counts
             * from the beginning of the load command, including its 8 byte
             * header, so the first record starts at offset 8.
             */
            uint64_t used  = 8;
            uint32_t index = 0;

            while( used < size )
            {
                if( used > static_cast< uint64_t >( size ) - 8 )
                {
                    throw std::runtime_error(
                        "Mach-O thread command ends with " + std::to_string( static_cast< uint64_t >( size ) - used )
                        + " trailing byte(s), not enough for a record header"
                    );
                }

                if( stream.availableBytes() < 8 )
                {
                    throw std::runtime_error( "Mach-O thread record header is outside of the input stream" );
                }

                std::vector< uint8_t > header = stream.read( 8 );

                uint32_t flavor        = u32( header, 0, bigEndian );
                uint32_t count         = u32( header, 4, bigEndian );
                uint64_t payloadLength = static_cast< uint64_t >( count ) * 4;
                uint64_t recordEnd     = checkedAdd( checkedAdd( used, 8, "thread record span" ), payloadLength, "thread record span" );

                if( recordEnd > size )
                {
                    throw std::runtime_error(
                        "Mach-O thread record #" + std::to_string( index )
                        + " (flavor " + std::to_string( flavor ) + ", count " + std::to_string( count )
                        + ") extends past the " + std::to_string( size ) + " byte load command"
                    );
                }

                if( stream.availableBytes() < static_cast< size_t >( payloadLength ) )
                {
                    throw std::runtime_error(
                        "Mach-O thread record #" + std::to_string( index ) + " payload is truncated in the input stream"
                    );
                }

                std::vector< uint8_t > payload = stream.read( static_cast< size_t >( payloadLength ) );
                std::vector< uint8_t > bytes;

                bytes.reserve( header.size() + payload.size() );
                bytes.insert( bytes.end(), header.begin(), header.end() );
                bytes.insert( bytes.end(), payload.begin(), payload.end() );

                std::optional< ThreadContext > context;

                if( cpuType.has_value() )
                {
                    context = decodeRecord( *cpuType, flavor, count, payload, bigEndian, index );
                }

                this->_records.emplace_back(
                    flavor,
                    count,
                    static_cast< uint32_t >( used ),
                    std::move( bytes ),
                    std::move( payload ),
                    std::move( context )
                );

                used = recordEnd;
                ++index;
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
