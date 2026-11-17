/*
 * Startup regression tests.
 *
 * These tests synthesize Mach-O images in memory (and fat images containing
 * two slices) and exercise the read-only parsing chain:
 *
 *   - Thread records/flavors/contexts (group "records", "contexts")
 *   - File::entryPoints() startup sources (group "entrypoints")
 *
 * Nothing is loaded or executed; all inputs are byte vectors.
 */

#include "task_support/check.h"

#include <MachO/File.hpp>
#include <MachO/FatFile.hpp>
#include <MachO/LoadCommands/EntryPoint.hpp>
#include <MachO/LoadCommands/Segment.hpp>
#include <MachO/LoadCommands/Segment64.hpp>
#include <MachO/LoadCommands/Thread.hpp>
#include <XS.hpp>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t LC_SEGMENT       = 0x01;
    constexpr uint32_t LC_THREAD        = 0x04;
    constexpr uint32_t LC_UNIXTHREAD    = 0x05;
    constexpr uint32_t LC_SEGMENT_64    = 0x19;
    constexpr uint32_t LC_MAIN          = 0x80000028;

    constexpr uint32_t CPU_X86          = 7;
    constexpr uint32_t CPU_X86_64       = 0x01000007;
    constexpr uint32_t CPU_ARM          = 12;
    constexpr uint32_t CPU_ARM64        = 0x0100000c;
    constexpr uint32_t CPU_PPC64        = 0x01000012;

    struct Writer
    {
        std::vector< uint8_t > data;
        bool                   bigEndian;

        explicit Writer( bool bigEndian = false ): bigEndian( bigEndian ) {}

        void u8( uint8_t value )
        {
            this->data.push_back( value );
        }

        void u32( uint32_t value )
        {
            if( this->bigEndian )
            {
                this->data.push_back( static_cast< uint8_t >( value >> 24 ) );
                this->data.push_back( static_cast< uint8_t >( value >> 16 ) );
                this->data.push_back( static_cast< uint8_t >( value >>  8 ) );
                this->data.push_back( static_cast< uint8_t >( value       ) );
            }
            else
            {
                this->data.push_back( static_cast< uint8_t >( value       ) );
                this->data.push_back( static_cast< uint8_t >( value >>  8 ) );
                this->data.push_back( static_cast< uint8_t >( value >> 16 ) );
                this->data.push_back( static_cast< uint8_t >( value >> 24 ) );
            }
        }

        void u64( uint64_t value )
        {
            std::vector< uint8_t > bytes;

            for( unsigned i = 0; i < 8; i++ )
            {
                bytes.push_back( static_cast< uint8_t >( value >> ( 8 * i ) ) );
            }

            if( this->bigEndian )
            {
                for( auto it = bytes.rbegin(); it != bytes.rend(); ++it )
                {
                    this->data.push_back( *it );
                }
            }
            else
            {
                this->data.insert( this->data.end(), bytes.begin(), bytes.end() );
            }
        }

        void bytes( const std::vector< uint8_t > & b )
        {
            this->data.insert( this->data.end(), b.begin(), b.end() );
        }

        void zeros( size_t count )
        {
            this->data.insert( this->data.end(), count, 0 );
        }

        void name16( const std::string & name )
        {
            std::vector< uint8_t > b( 16, 0 );

            std::memcpy( b.data(), name.data(), std::min< size_t >( name.size(), 16 ) );
            this->bytes( b );
        }
    };

    std::vector< uint8_t > payloadU32( bool bigEndian, const std::vector< uint32_t > & values )
    {
        Writer w( bigEndian );

        for( auto value: values )
        {
            w.u32( value );
        }

        return w.data;
    }

    std::vector< uint8_t > payloadU64( bool bigEndian, const std::vector< uint64_t > & values )
    {
        Writer w( bigEndian );

        for( auto value: values )
        {
            w.u64( value );
        }

        return w.data;
    }

    /* 16 u32 i386 registers: eax ebx ecx edx edi esi ebp esp ss eflags eip cs ds es fs gs */
    std::vector< uint8_t > i386Registers( bool bigEndian, uint32_t eip, uint32_t esp, uint32_t eflags = 0 )
    {
        std::vector< uint32_t > r( 16, 0 );

        r[ 0 ]  = 0x11111111;
        r[ 1 ]  = 0x22222222;
        r[ 2 ]  = 0x33333333;
        r[ 3 ]  = 0x44444444;
        r[ 4 ]  = 0x55555555;
        r[ 5 ]  = 0x66666666;
        r[ 6 ]  = 0x77777777;
        r[ 7 ]  = esp;
        r[ 8 ]  = 0x2B;
        r[ 9 ]  = eflags;
        r[ 10 ] = eip;
        r[ 11 ] = 0x23;
        r[ 12 ] = 0x2B;
        r[ 13 ] = 0x2B;
        r[ 14 ] = 0x00;
        r[ 15 ] = 0x00;

        return payloadU32( bigEndian, r );
    }

    /* 21 u64 x86_64 registers: rax rbx rcx rdx rdi rsi rbp rsp r8..r15 rip rflags cs fs gs */
    std::vector< uint8_t > x86_64Registers( bool bigEndian, uint64_t rip, uint64_t rsp )
    {
        std::vector< uint64_t > r( 21, 0 );

        r[ 0 ]  = 0x1111111111111111ull;
        r[ 1 ]  = 0x2222222222222222ull;
        r[ 2 ]  = 0x3333333333333333ull;
        r[ 3 ]  = 0x4444444444444444ull;
        r[ 4 ]  = 0x5555555555555555ull;
        r[ 5 ]  = 0x6666666666666666ull;
        r[ 6 ]  = 0x7777777777777777ull;
        r[ 7 ]  = rsp;
        r[ 8 ]  = 0x8888888888888888ull;
        r[ 9 ]  = 0x9999999999999999ull;
        r[ 10 ] = 0xAAAAAAAAAAAAAAAAull;
        r[ 11 ] = 0xBBBBBBBBBBBBBBBBull;
        r[ 12 ] = 0xCCCCCCCCCCCCCCCCull;
        r[ 13 ] = 0xDDDDDDDDDDDDDDDDull;
        r[ 14 ] = 0xEEEEEEEEEEEEEEEEull;
        r[ 15 ] = 0xFFFFFFFFFFFFFFFFull;
        r[ 16 ] = rip;
        r[ 17 ] = 0x202;
        r[ 18 ] = 0x2B;
        r[ 19 ] = 0x00;
        r[ 20 ] = 0x00;

        return payloadU64( bigEndian, r );
    }

    /* 17 u32 ARM registers: r0..r12 sp lr pc cpsr */
    std::vector< uint8_t > armRegisters( bool bigEndian, uint32_t pc )
    {
        std::vector< uint32_t > r;

        for( uint32_t i = 0; i < 13; i++ )
        {
            r.push_back( 0x10000000u + i );
        }

        r.push_back( 0x20000000u ); /* sp  */
        r.push_back( 0x30000000u ); /* lr  */
        r.push_back( pc );          /* pc  */
        r.push_back( 0x60000010u ); /* cpsr */

        return payloadU32( bigEndian, r );
    }

    /* 33 u64 + u32 cpsr + 4 padding bytes */
    std::vector< uint8_t > arm64Registers( bool bigEndian, uint64_t pc, uint64_t sp )
    {
        std::vector< uint64_t > r;

        for( uint64_t i = 0; i < 29; i++ )
        {
            r.push_back( 0x5000000000000000ull + i );
        }

        r.push_back( 0x6000000000000000ull ); /* fp  */
        r.push_back( 0x7000000000000000ull ); /* lr  */
        r.push_back( sp );                    /* sp  */
        r.push_back( pc );                    /* pc  */

        Writer w( bigEndian );

        for( auto value: r )
        {
            w.u64( value );
        }

        w.u32( 0x60000000u );

        std::vector< uint8_t > padding = { 0xDE, 0xAD, 0xBE, 0xEF };

        w.bytes( padding );

        return w.data;
    }

    struct RawRecord
    {
        uint32_t               flavor;
        uint32_t               count;
        std::vector< uint8_t > payload;
    };

    std::vector< uint8_t > threadPayload( bool bigEndian, const std::vector< RawRecord > & records )
    {
        Writer w( bigEndian );

        for( const auto & record: records )
        {
            w.u32( record.flavor );
            w.u32( record.count );
            w.bytes( record.payload );
        }

        return w.data;
    }

    std::vector< uint8_t > threadCommand( uint32_t command, bool bigEndian, const std::vector< RawRecord > & records )
    {
        auto payload = threadPayload( bigEndian, records );

        Writer w( bigEndian );

        w.u32( command );
        w.u32( static_cast< uint32_t >( 8 + payload.size() ) );
        w.bytes( payload );

        return w.data;
    }

    /* Unified x86 flavor 7: 8 byte inner header + fixed 168 byte union. */
    std::vector< uint8_t > unifiedX86( bool bigEndian, uint32_t innerFlavor, uint32_t innerCount, const std::vector< uint8_t > & registers, uint8_t padding )
    {
        Writer w( bigEndian );

        w.u32( innerFlavor );
        w.u32( innerCount );
        w.bytes( registers );

        size_t remaining = 168 - registers.size();

        /* Make padding visible so the test can prove it is retained, not parsed. */
        w.zeros( remaining );

        std::fill( w.data.begin() + static_cast< ssize_t >( 8 + registers.size() ), w.data.end(), padding );

        return w.data;
    }

    std::vector< uint8_t > segment64Command( const std::string & name, uint64_t vmAddress, uint64_t vmSize, uint64_t fileOffset, uint64_t fileSize, bool bigEndian = false )
    {
        Writer w( bigEndian );

        w.u32( LC_SEGMENT_64 );
        w.u32( 72 );
        w.name16( name );
        w.u64( vmAddress );
        w.u64( vmSize );
        w.u64( fileOffset );
        w.u64( fileSize );
        w.u32( 7 );
        w.u32( 5 );
        w.u32( 0 );
        w.u32( 0 );

        return w.data;
    }

    std::vector< uint8_t > segment32Command( const std::string & name, uint32_t vmAddress, uint32_t vmSize, uint32_t fileOffset, uint32_t fileSize )
    {
        Writer w( false );

        w.u32( LC_SEGMENT );
        w.u32( 56 );
        w.name16( name );
        w.u32( vmAddress );
        w.u32( vmSize );
        w.u32( fileOffset );
        w.u32( fileSize );
        w.u32( 7 );
        w.u32( 5 );
        w.u32( 0 );
        w.u32( 0 );

        return w.data;
    }

    std::vector< uint8_t > mainCommand( uint64_t entryOffset, uint64_t stackSize = 0, bool bigEndian = false )
    {
        Writer w( bigEndian );

        w.u32( LC_MAIN );
        w.u32( 24 );
        w.u64( entryOffset );
        w.u64( stackSize );

        return w.data;
    }

    std::vector< uint8_t > image
    (
        uint32_t                                 cpu,
        bool                                     is64,
        bool                                     bigEndian,
        const std::vector< std::vector< uint8_t > > & commands,
        size_t                                   totalSize = 0x4000
    )
    {
        Writer header( bigEndian );
        size_t commandsSize = 0;

        for( const auto & command: commands )
        {
            commandsSize += command.size();
        }

        header.u32( is64 ? 0xFEEDFACF : 0xFEEDFACE );
        header.u32( cpu );
        header.u32( 3 );
        header.u32( 2 );
        header.u32( static_cast< uint32_t >( commands.size() ) );
        header.u32( static_cast< uint32_t >( commandsSize ) );
        header.u32( 0 );

        if( is64 )
        {
            header.u32( 0 );
        }

        for( const auto & command: commands )
        {
            header.bytes( command );
        }

        if( header.data.size() < totalSize )
        {
            header.zeros( totalSize - header.data.size() );
        }

        return header.data;
    }

    std::vector< uint8_t > fatImage( const std::vector< std::vector< uint8_t > > & slices, const std::vector< uint32_t > & cpus )
    {
        Writer w( true );

        w.u32( 0xCAFEBABE );
        w.u32( static_cast< uint32_t >( slices.size() ) );

        std::vector< uint32_t > offsets;
        uint32_t                cursor = 0x1000;

        for( size_t i = 0; i < slices.size(); i++ )
        {
            /* Keep each slice on a 0x1000 boundary and non-overlapping. */
            cursor = ( cursor + 0xFFF ) & ~static_cast< uint32_t >( 0xFFF );
            offsets.push_back( cursor );
            cursor += static_cast< uint32_t >( slices[ i ].size() );

            w.u32( cpus[ i ] );
            w.u32( 3 );
            w.u32( offsets.back() );
            w.u32( static_cast< uint32_t >( slices[ i ].size() ) );
            w.u32( 12 );
        }

        for( size_t i = 0; i < slices.size(); i++ )
        {
            if( w.data.size() < offsets[ i ] )
            {
                w.zeros( offsets[ i ] - w.data.size() );
            }

            w.bytes( slices[ i ] );
        }

        return w.data;
    }

    template< typename F >
    bool throwsRuntimeError( F f )
    {
        try
        {
            f();
        }
        catch( const std::runtime_error & )
        {
            return true;
        }
        catch( ... )
        {
            return false;
        }

        return false;
    }

    MachO::LoadCommands::Thread parseThread
    (
        uint32_t                       command,
        MachO::File::Kind              kind,
        const MachO::CPU &             cpu,
        const std::vector< uint8_t > & payload
    )
    {
        XS::IO::BinaryDataStream stream( payload );

        return MachO::LoadCommands::Thread( command, static_cast< uint32_t >( 8 + payload.size() ), kind, cpu, stream );
    }
}

/* ========================================================================== */
/* records                                                                    */
/* ========================================================================== */

static int case_records_unified_padding_then_unknown( void )
{
    bool be = false;

    std::vector< RawRecord > records;

    records.push_back( { 7, 44, unifiedX86( be, 1, 16, i386Registers( be, 0x401000, 0x7FFF0000, 0xFFFFFFFFu ), 0xAA ) } );
    records.push_back( { 0x1234, 2, payloadU32( be, { 0xDEADBEEF, 0xCAFEBABE } ) } );

    auto payload = threadPayload( be, records );
    auto thread  = parseThread( LC_THREAD, MachO::File::Kind::MachO64, MachO::CPU( CPU_X86_64, 3 ), payload );
    auto parsed  = thread.records();

    TASK_CHECK( parsed.size() == 2 );

    /* First record starts after the 8 byte command header. */
    TASK_CHECK( parsed[ 0 ].offset == 8 );
    TASK_CHECK( parsed[ 0 ].flavor == 7 );
    TASK_CHECK( parsed[ 0 ].count == 44 );

    /* Outer payload is the 8 byte inner header plus the 168 byte union. */
    TASK_CHECK( parsed[ 0 ].payload.size() == 176 );
    TASK_CHECK( parsed[ 0 ].state.has_value() );
    TASK_CHECK( parsed[ 0 ].state->architecture() == MachO::LoadCommands::Thread::Architecture::I386 );

    /* Union bytes after the 64 byte inner i386 state are padding, retained raw. */
    TASK_CHECK( parsed[ 0 ].payload[ 8 + 64 + 0 ] == 0xAA );
    TASK_CHECK( parsed[ 0 ].payload[ 8 + 64 + 3 ] == 0xAA );
    TASK_CHECK( parsed[ 0 ].payload[ 175 ] == 0xAA );

    /* Padding must not be mistaken for the next record header. */
    TASK_CHECK( parsed[ 1 ].offset == 8 + 8 + 176 );
    TASK_CHECK( parsed[ 1 ].flavor == 0x1234 );
    TASK_CHECK( parsed[ 1 ].count == 2 );
    TASK_CHECK( parsed[ 1 ].payload.size() == 8 );
    TASK_CHECK( parsed[ 1 ].payload[ 0 ] == 0xEF );
    TASK_CHECK( parsed[ 1 ].payload[ 4 ] == 0xBE );
    TASK_CHECK( parsed[ 1 ].state.has_value() == false );

    return 0;
}

static int case_records_flavor1_two_cpus( void )
{
    /* Same flavor 1 means i386 for an x86 image... */
    {
        auto payload = payloadU32( false, {} );
        std::vector< RawRecord > records;

        records.push_back( { 1, 16, i386Registers( false, 0x1000, 0x2000 ) } );
        payload = threadPayload( false, records );

        auto thread = parseThread( LC_UNIXTHREAD, MachO::File::Kind::MachO32, MachO::CPU( CPU_X86, 3 ), payload );
        auto parsed = thread.records();

        TASK_CHECK( parsed.size() == 1 );
        TASK_CHECK( parsed[ 0 ].state.has_value() );
        TASK_CHECK( parsed[ 0 ].state->architecture() == MachO::LoadCommands::Thread::Architecture::I386 );
        TASK_CHECK( parsed[ 0 ].state->registerValue( "eip" ).value() == 0x1000 );
        TASK_CHECK( parsed[ 0 ].state->registerValue( "pc" ).has_value() == false );
    }

    /* ...and ARM for an ARM image. */
    {
        std::vector< RawRecord > records;

        records.push_back( { 1, 17, armRegisters( false, 0x8000 ) } );

        auto payload = threadPayload( false, records );
        auto thread  = parseThread( LC_UNIXTHREAD, MachO::File::Kind::MachO32, MachO::CPU( CPU_ARM, 9 ), payload );
        auto parsed  = thread.records();

        TASK_CHECK( parsed.size() == 1 );
        TASK_CHECK( parsed[ 0 ].flavor == 1 );
        TASK_CHECK( parsed[ 0 ].count == 17 );
        TASK_CHECK( parsed[ 0 ].state.has_value() );
        TASK_CHECK( parsed[ 0 ].state->architecture() == MachO::LoadCommands::Thread::Architecture::ARM );
        TASK_CHECK( parsed[ 0 ].state->registerValue( "pc" ).value() == 0x8000 );
        TASK_CHECK( parsed[ 0 ].state->registerValue( "eip" ).has_value() == false );
    }

    return 0;
}

static int case_records_legacy_constructor_keeps_raw_records( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 4, 42, x86_64Registers( false, 0x100000100, 0x700000 ) } );

    auto payload = threadPayload( false, records );
    XS::IO::BinaryDataStream stream( payload );

    /* The legacy signature has no CPU: raw records, no decoded state. */
    MachO::LoadCommands::Thread thread( LC_UNIXTHREAD, static_cast< uint32_t >( 8 + payload.size() ), MachO::File::Kind::MachO64, stream );
    auto parsed = thread.records();

    TASK_CHECK( parsed.size() == 1 );
    TASK_CHECK( parsed[ 0 ].offset == 8 );
    TASK_CHECK( parsed[ 0 ].flavor == 4 );
    TASK_CHECK( parsed[ 0 ].count == 42 );
    TASK_CHECK( parsed[ 0 ].payload.size() == 168 );
    TASK_CHECK( parsed[ 0 ].state.has_value() == false );
    TASK_CHECK( thread.size() == 8 + payload.size() );

    return 0;
}

static int case_records_invalid_known_counts( void )
{
    MachO::CPU x86_64( CPU_X86_64, 3 );

    /* flavor 4 expects exactly 42. */
    {
        std::vector< RawRecord > records;
        records.push_back( { 4, 41, std::vector< uint8_t >( 164, 0 ) } );

        auto payload = threadPayload( false, records );

        TASK_CHECK( throwsRuntimeError( [ & ] { parseThread( LC_UNIXTHREAD, MachO::File::Kind::MachO64, x86_64, payload ); } ) );
    }

    /* flavor 1 expects 16 on x86. */
    {
        std::vector< RawRecord > records;
        records.push_back( { 1, 15, std::vector< uint8_t >( 60, 0 ) } );

        auto payload = threadPayload( false, records );

        TASK_CHECK( throwsRuntimeError( [ & ] { parseThread( LC_THREAD, MachO::File::Kind::MachO64, x86_64, payload ); } ) );
    }

    /* Unified outer count expects 44. */
    {
        std::vector< RawRecord > records;
        records.push_back( { 7, 43, std::vector< uint8_t >( 172, 0 ) } );

        auto payload = threadPayload( false, records );

        TASK_CHECK( throwsRuntimeError( [ & ] { parseThread( LC_THREAD, MachO::File::Kind::MachO64, x86_64, payload ); } ) );
    }

    /* Unified inner count must match the inner flavor. */
    {
        std::vector< RawRecord > records;
        records.push_back( { 7, 44, unifiedX86( false, 4, 41, std::vector< uint8_t >( 168, 0 ), 0 ) } );

        auto payload = threadPayload( false, records );

        TASK_CHECK( throwsRuntimeError( [ & ] { parseThread( LC_THREAD, MachO::File::Kind::MachO64, x86_64, payload ); } ) );
    }

    return 0;
}

static int case_records_truncated_and_overrun( void )
{
    MachO::CPU x86_64( CPU_X86_64, 3 );

    /* Trailing partial header (4 bytes) is rejected. */
    {
        std::vector< uint8_t > payload = { 4, 0, 0, 0 };

        TASK_CHECK( throwsRuntimeError( [ & ] { parseThread( LC_THREAD, MachO::File::Kind::MachO64, x86_64, payload ); } ) );
    }

    /* A record whose count crosses the command end is rejected. */
    {
        Writer w( false );

        w.u32( 4 );
        w.u32( 42 );
        w.u32( 0xDEADBEEF ); /* only 4 of the 168 bytes are present */

        TASK_CHECK( throwsRuntimeError( [ & ] { parseThread( LC_UNIXTHREAD, MachO::File::Kind::MachO64, x86_64, w.data ); } ) );
    }

    /* Unknown flavors keep their payload while contained, including zero count. */
    {
        std::vector< RawRecord > records;
        records.push_back( { 0x99, 0, {} } );

        auto payload = threadPayload( false, records );
        auto thread  = parseThread( LC_THREAD, MachO::File::Kind::MachO64, x86_64, payload );
        auto parsed  = thread.records();

        TASK_CHECK( parsed.size() == 1 );
        TASK_CHECK( parsed[ 0 ].payload.empty() );
        TASK_CHECK( parsed[ 0 ].state.has_value() == false );
    }

    return 0;
}

static int case_records_big_endian( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 4, 42, x86_64Registers( true, 0x100000200ull, 0x700000ull ) } );

    auto payload = threadPayload( true, records );
    XS::IO::BinaryDataStream stream( payload );

    stream.setPreferredEndianness( XS::IO::BinaryStream::Endianness::BigEndian );

    MachO::LoadCommands::Thread thread( LC_UNIXTHREAD, static_cast< uint32_t >( 8 + payload.size() ), MachO::File::Kind::MachO64, MachO::CPU( CPU_X86_64, 3 ), stream );
    auto parsed = thread.records();

    TASK_CHECK( parsed.size() == 1 );
    TASK_CHECK( parsed[ 0 ].state.has_value() );
    TASK_CHECK( parsed[ 0 ].state->pc() == 0x100000200ull );
    TASK_CHECK( parsed[ 0 ].state->sp() == 0x700000ull );
    TASK_CHECK( parsed[ 0 ].state->registerValue( "r15" ).value() == 0xFFFFFFFFFFFFFFFFull );

    return 0;
}

static int case_records_big_endian_file( void )
{
    /* Full File path with a big-endian Mach-O header and record payloads. */
    std::vector< RawRecord > records;

    records.push_back( { 7, 44, unifiedX86( true, 4, 42, x86_64Registers( true, 0x100000600ull, 0x700000ull ), 0 ) } );

    auto bytes = image
    (
        CPU_X86_64,
        true,
        true,
        { threadCommand( LC_UNIXTHREAD, true, records ) }
    );

    XS::IO::BinaryDataStream stream( bytes );
    MachO::File                file( stream );

    TASK_CHECK( file.endianness() == MachO::File::Endianness::BigEndian );

    auto threads = file.loadCommands< MachO::LoadCommands::Thread >();

    TASK_CHECK( threads.size() == 1 );
    TASK_CHECK( threads[ 0 ].records().size() == 1 );
    TASK_CHECK( threads[ 0 ].records()[ 0 ].flavor == 7 );
    TASK_CHECK( threads[ 0 ].records()[ 0 ].state.has_value() );
    TASK_CHECK( threads[ 0 ].records()[ 0 ].state->pc() == 0x100000600ull );
    TASK_CHECK( threads[ 0 ].records()[ 0 ].state->registerValue( "rip" ).value() == 0x100000600ull );

    return 0;
}

static int case_records_copy_and_stream_release( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 4, 42, x86_64Registers( false, 0x100001000ull, 0x800000ull ) } );

    auto bytes = image
    (
        CPU_X86_64,
        true,
        false,
        { threadCommand( LC_UNIXTHREAD, false, records ) }
    );

    std::unique_ptr< MachO::File > file;

    {
        auto stream = std::make_shared< XS::IO::BinaryDataStream >( bytes );

        file = std::make_unique< MachO::File >( *( stream ) );

        /* Release the input stream before reading anything back. */
        stream.reset();
    }

    MachO::File copy( *( file ) );

    file.reset();

    auto threads = copy.loadCommands< MachO::LoadCommands::Thread >();

    TASK_CHECK( threads.size() == 1 );

    auto parsed = threads[ 0 ].records();

    TASK_CHECK( parsed.size() == 1 );
    TASK_CHECK( parsed[ 0 ].state.has_value() );
    TASK_CHECK( parsed[ 0 ].state->registerValue( "rip" ).value() == 0x100001000ull );

    /* Command created by File carries the image CPU. */
    TASK_CHECK( parsed[ 0 ].state->architecture() == MachO::LoadCommands::Thread::Architecture::X86_64 );

    return 0;
}

/* ========================================================================== */
/* contexts                                                                   */
/* ========================================================================== */

static int case_contexts_x86_64( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 4, 42, x86_64Registers( false, 0x100000810ull, 0x7FFEE000ull ) } );

    auto payload = threadPayload( false, records );
    auto thread  = parseThread( LC_UNIXTHREAD, MachO::File::Kind::MachO64, MachO::CPU( CPU_X86_64, 8 ), payload );
    auto parsed  = thread.records();

    TASK_CHECK( parsed.size() == 1 );
    TASK_CHECK( parsed[ 0 ].state.has_value() );

    const auto & state = *( parsed[ 0 ].state );

    TASK_CHECK( state.architecture() == MachO::LoadCommands::Thread::Architecture::X86_64 );
    TASK_CHECK( state.architectureName() == "x86_64" );
    TASK_CHECK( state.registers().size() == 21 );
    TASK_CHECK( state.pc() == 0x100000810ull );
    TASK_CHECK( state.sp() == 0x7FFEE000ull );

    TASK_CHECK( state.registerValue( "rax" ).value() == 0x1111111111111111ull );
    TASK_CHECK( state.registerValue( "rbp" ).value() == 0x7777777777777777ull );
    TASK_CHECK( state.registerValue( "r15" ).value() == 0xFFFFFFFFFFFFFFFFull );
    TASK_CHECK( state.registerValue( "rip" ).value() == 0x100000810ull );
    TASK_CHECK( state.registerValue( "rflags" ).value() == 0x202 );
    TASK_CHECK( state.registerValue( "eip" ).has_value() == false );

    return 0;
}

static int case_contexts_i386_unsigned( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 1, 16, i386Registers( false, 0xFFFFFFFFu, 0xFFFFF000u, 0xFFFFFFFFu ) } );

    auto payload = threadPayload( false, records );
    auto thread  = parseThread( LC_UNIXTHREAD, MachO::File::Kind::MachO32, MachO::CPU( CPU_X86, 3 ), payload );
    auto parsed  = thread.records();

    TASK_CHECK( parsed.size() == 1 );
    TASK_CHECK( parsed[ 0 ].state.has_value() );

    const auto & state = *( parsed[ 0 ].state );

    TASK_CHECK( state.architecture() == MachO::LoadCommands::Thread::Architecture::I386 );
    TASK_CHECK( state.registers().size() == 16 );

    /* 32-bit registers are zero-extended, not sign-extended. */
    TASK_CHECK( state.registerValue( "eip" ).value() == 0xFFFFFFFFull );
    TASK_CHECK( state.registerValue( "eflags" ).value() == 0xFFFFFFFFull );
    TASK_CHECK( state.pc() == 0xFFFFFFFFull );
    TASK_CHECK( state.sp() == 0xFFFFF000ull );
    TASK_CHECK( state.registerValue( "rip" ).has_value() == false );

    return 0;
}

static int case_contexts_unified_inner_x86_64( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 7, 44, unifiedX86( false, 4, 42, x86_64Registers( false, 0x100000300ull, 0x700000ull ), 0 ) } );

    auto payload = threadPayload( false, records );
    auto thread  = parseThread( LC_THREAD, MachO::File::Kind::MachO64, MachO::CPU( CPU_X86_64, 3 ), payload );
    auto parsed  = thread.records();

    TASK_CHECK( parsed.size() == 1 );

    /* Outer identity is preserved: it stays flavor 7/count 44. */
    TASK_CHECK( parsed[ 0 ].flavor == 7 );
    TASK_CHECK( parsed[ 0 ].count == 44 );
    TASK_CHECK( parsed[ 0 ].payload.size() == 176 );

    TASK_CHECK( parsed[ 0 ].state.has_value() );
    TASK_CHECK( parsed[ 0 ].state->architecture() == MachO::LoadCommands::Thread::Architecture::X86_64 );
    TASK_CHECK( parsed[ 0 ].state->pc() == 0x100000300ull );
    TASK_CHECK( parsed[ 0 ].state->registerValue( "rip" ).value() == 0x100000300ull );

    return 0;
}

static int case_contexts_arm32( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 1, 17, armRegisters( false, 0x4000 ) } );

    auto payload = threadPayload( false, records );
    auto thread  = parseThread( LC_UNIXTHREAD, MachO::File::Kind::MachO32, MachO::CPU( CPU_ARM, 9 ), payload );
    auto parsed  = thread.records();

    TASK_CHECK( parsed.size() == 1 );
    TASK_CHECK( parsed[ 0 ].state.has_value() );

    const auto & state = *( parsed[ 0 ].state );

    TASK_CHECK( state.architecture() == MachO::LoadCommands::Thread::Architecture::ARM );
    TASK_CHECK( state.registers().size() == 17 );
    TASK_CHECK( state.registerValue( "r0" ).value() == 0x10000000u );
    TASK_CHECK( state.registerValue( "r12" ).value() == 0x1000000cu );
    TASK_CHECK( state.registerValue( "sp" ).value() == 0x20000000u );
    TASK_CHECK( state.registerValue( "lr" ).value() == 0x30000000u );
    TASK_CHECK( state.registerValue( "pc" ).value() == 0x4000u );
    TASK_CHECK( state.registerValue( "cpsr" ).value() == 0x60000010u );
    TASK_CHECK( state.sp() == 0x20000000u );

    return 0;
}

static int case_contexts_arm64( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 6, 68, arm64Registers( false, 0x100000400ull, 0x16FE00000ull ) } );

    auto payload = threadPayload( false, records );
    auto thread  = parseThread( LC_UNIXTHREAD, MachO::File::Kind::MachO64, MachO::CPU( CPU_ARM64, 0 ), payload );
    auto parsed  = thread.records();

    TASK_CHECK( parsed.size() == 1 );
    TASK_CHECK( parsed[ 0 ].state.has_value() );

    const auto & state = *( parsed[ 0 ].state );

    TASK_CHECK( state.architecture() == MachO::LoadCommands::Thread::Architecture::ARM64 );
    TASK_CHECK( state.registers().size() == 34 );
    TASK_CHECK( state.registerValue( "x0" ).value() == 0x5000000000000000ull );
    TASK_CHECK( state.registerValue( "x28" ).value() == 0x500000000000001cull );
    TASK_CHECK( state.registerValue( "fp" ).value() == 0x6000000000000000ull );
    TASK_CHECK( state.registerValue( "lr" ).value() == 0x7000000000000000ull );
    TASK_CHECK( state.registerValue( "sp" ).value() == 0x16FE00000ull );
    TASK_CHECK( state.registerValue( "pc" ).value() == 0x100000400ull );
    TASK_CHECK( state.registerValue( "cpsr" ).value() == 0x60000000u );

    /* The trailing 4 bytes are padding, not a register. */
    TASK_CHECK( state.registerValue( "padding" ).has_value() == false );
    TASK_CHECK( parsed[ 0 ].payload.size() == 272 );
    TASK_CHECK( parsed[ 0 ].payload[ 268 ] == 0xDE );
    TASK_CHECK( parsed[ 0 ].payload[ 271 ] == 0xEF );

    return 0;
}

static int case_contexts_unknown_cpu_and_family( void )
{
    /* Unknown CPU preserves the raw record only. */
    {
        std::vector< RawRecord > records;
        records.push_back( { 4, 42, x86_64Registers( false, 0x1000, 0x2000 ) } );

        auto payload = threadPayload( false, records );
        auto thread  = parseThread( LC_UNIXTHREAD, MachO::File::Kind::MachO64, MachO::CPU( CPU_PPC64, 0 ), payload );
        auto parsed  = thread.records();

        TASK_CHECK( parsed.size() == 1 );
        TASK_CHECK( parsed[ 0 ].state.has_value() == false );
        TASK_CHECK( parsed[ 0 ].payload.size() == 168 );
    }

    /* A known x86 flavor seen by an ARM64 CPU stays raw (family mismatch). */
    {
        std::vector< RawRecord > records;
        records.push_back( { 4, 42, x86_64Registers( false, 0x1000, 0x2000 ) } );

        auto payload = threadPayload( false, records );
        auto thread  = parseThread( LC_UNIXTHREAD, MachO::File::Kind::MachO64, MachO::CPU( CPU_ARM64, 0 ), payload );
        auto parsed  = thread.records();

        TASK_CHECK( parsed.size() == 1 );
        TASK_CHECK( parsed[ 0 ].flavor == 4 );
        TASK_CHECK( parsed[ 0 ].count == 42 );
        TASK_CHECK( parsed[ 0 ].state.has_value() == false );
    }

    /* A known ARM flavor seen by an x86 CPU stays raw as well. */
    {
        std::vector< RawRecord > records;
        records.push_back( { 6, 68, arm64Registers( false, 0x1000, 0x2000 ) } );

        auto payload = threadPayload( false, records );
        auto thread  = parseThread( LC_UNIXTHREAD, MachO::File::Kind::MachO64, MachO::CPU( CPU_X86_64, 3 ), payload );
        auto parsed  = thread.records();

        TASK_CHECK( parsed.size() == 1 );
        TASK_CHECK( parsed[ 0 ].state.has_value() == false );
    }

    return 0;
}

static int case_contexts_get_info( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 4, 42, x86_64Registers( false, 0x100000810ull, 0x7FFEE000ull ) } );
    records.push_back( { 0x99, 1, payloadU32( false, { 0xABCDEF01 } ) } );

    auto bytes = image
    (
        CPU_X86_64,
        true,
        false,
        { threadCommand( LC_UNIXTHREAD, false, records ) }
    );

    XS::IO::BinaryDataStream stream( bytes );
    MachO::File                file( stream );

    auto threads = file.loadCommands< MachO::LoadCommands::Thread >();

    TASK_CHECK( threads.size() == 1 );

    std::string info = threads[ 0 ].getInfo().toString();

    TASK_CHECK( info.find( "Record 0" ) != std::string::npos );
    TASK_CHECK( info.find( "Record 1" ) != std::string::npos );
    TASK_CHECK( info.find( "Registers" ) != std::string::npos );
    TASK_CHECK( info.find( "rip" ) != std::string::npos );
    TASK_CHECK( info.find( "PC" ) != std::string::npos );
    TASK_CHECK( info.find( "raw" ) != std::string::npos );

    std::string fileInfo = file.getInfo().toString();

    TASK_CHECK( fileInfo.find( "Registers" ) != std::string::npos );
    TASK_CHECK( fileInfo.find( "rip" ) != std::string::npos );

    return 0;
}

/* ========================================================================== */
/* entrypoints                                                                */
/* ========================================================================== */

static int case_entrypoints_main_to_va( void )
{
    auto bytes = image
    (
        CPU_X86_64,
        true,
        false,
        {
            segment64Command( "__TEXT", 0x100000000ull, 0x4000, 0x1000, 0x2000 ),
            mainCommand( 0x1800 )
        }
    );

    XS::IO::BinaryDataStream stream( bytes );
    MachO::File                file( stream );
    auto                       sources = file.entryPoints();

    TASK_CHECK( sources.size() == 1 );
    TASK_CHECK( sources[ 0 ].commandIndex == 1 );
    TASK_CHECK( sources[ 0 ].recordIndex.has_value() == false );
    TASK_CHECK( sources[ 0 ].kind == MachO::File::EntryPointKind::Main );
    TASK_CHECK( sources[ 0 ].coordinate == 0x1800 );
    TASK_CHECK( sources[ 0 ].segments.size() == 1 );

    const auto & mapping = sources[ 0 ].segments[ 0 ];

    TASK_CHECK( mapping.commandIndex == 0 );
    TASK_CHECK( mapping.name == "__TEXT" );
    TASK_CHECK( mapping.vmAddress == 0x100000000ull );
    TASK_CHECK( mapping.address == 0x100000800ull );
    TASK_CHECK( mapping.mappedFileOffset.has_value() );
    TASK_CHECK( mapping.mappedFileOffset.value() == 0x1800 );

    return 0;
}

static int case_entrypoints_main_no_segment( void )
{
    auto bytes = image
    (
        CPU_X86_64,
        true,
        false,
        { mainCommand( 0x5000 ) }
    );

    XS::IO::BinaryDataStream stream( bytes );
    MachO::File                file( stream );
    auto                       sources = file.entryPoints();

    TASK_CHECK( sources.size() == 1 );
    TASK_CHECK( sources[ 0 ].kind == MachO::File::EntryPointKind::Main );
    TASK_CHECK( sources[ 0 ].coordinate == 0x5000 );
    TASK_CHECK( sources[ 0 ].segments.empty() );

    return 0;
}

static int case_entrypoints_thread_file_and_zero_fill( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 4, 42, x86_64Registers( false, 0x100000800ull, 0x700000ull ) } );
    records.push_back( { 4, 42, x86_64Registers( false, 0x100001800ull, 0x700000ull ) } );

    auto bytes = image
    (
        CPU_X86_64,
        true,
        false,
        {
            segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x1000 ),
            threadCommand( LC_UNIXTHREAD, false, records )
        }
    );

    XS::IO::BinaryDataStream stream( bytes );
    MachO::File                file( stream );
    auto                       sources = file.entryPoints();

    TASK_CHECK( sources.size() == 2 );

    TASK_CHECK( sources[ 0 ].kind == MachO::File::EntryPointKind::UnixThread );
    TASK_CHECK( sources[ 0 ].commandIndex == 1 );
    TASK_CHECK( sources[ 0 ].recordIndex.has_value() );
    TASK_CHECK( sources[ 0 ].recordIndex.value() == 0 );
    TASK_CHECK( sources[ 0 ].coordinate == 0x100000800ull );
    TASK_CHECK( sources[ 0 ].segments.size() == 1 );
    TASK_CHECK( sources[ 0 ].segments[ 0 ].address == 0x100000800ull );
    TASK_CHECK( sources[ 0 ].segments[ 0 ].mappedFileOffset.has_value() );
    TASK_CHECK( sources[ 0 ].segments[ 0 ].mappedFileOffset.value() == 0x1800 );

    /* Second PC is in the zero-fill tail: VA candidate, no file offset. */
    TASK_CHECK( sources[ 1 ].recordIndex.value() == 1 );
    TASK_CHECK( sources[ 1 ].coordinate == 0x100001800ull );
    TASK_CHECK( sources[ 1 ].segments.size() == 1 );
    TASK_CHECK( sources[ 1 ].segments[ 0 ].address == 0x100001800ull );
    TASK_CHECK( sources[ 1 ].segments[ 0 ].mappedFileOffset.has_value() == false );

    return 0;
}

static int case_entrypoints_lc_thread_is_not_a_source( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 4, 42, x86_64Registers( false, 0x100000800ull, 0x700000ull ) } );

    auto bytes = image
    (
        CPU_X86_64,
        true,
        false,
        {
            segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x1000 ),
            threadCommand( LC_THREAD, false, records )
        }
    );

    XS::IO::BinaryDataStream stream( bytes );
    MachO::File                file( stream );

    TASK_CHECK( file.entryPoints().empty() );

    /* The state itself is still available through the Thread command. */
    auto threads = file.loadCommands< MachO::LoadCommands::Thread >();

    TASK_CHECK( threads.size() == 1 );
    TASK_CHECK( threads[ 0 ].records().size() == 1 );
    TASK_CHECK( threads[ 0 ].records()[ 0 ].state.has_value() );
    TASK_CHECK( threads[ 0 ].records()[ 0 ].state->pc() == 0x100000800ull );

    return 0;
}

static int case_entrypoints_overlapping_segments( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 4, 42, x86_64Registers( false, 0x100000800ull, 0x700000ull ) } );

    auto bytes = image
    (
        CPU_X86_64,
        true,
        false,
        {
            segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x1000 ),
            segment64Command( "__DATA", 0x200000000ull, 0x1000, 0x4000, 0x800 ),
            /* Overlapping the first segment, same VA range, different file bytes. */
            segment64Command( "__OVERLAP", 0x100000000ull, 0x1800, 0x2000, 0x1000 ),
            threadCommand( LC_UNIXTHREAD, false, records )
        }
    );

    XS::IO::BinaryDataStream stream( bytes );
    MachO::File                file( stream );
    auto                       sources = file.entryPoints();

    TASK_CHECK( sources.size() == 1 );
    TASK_CHECK( sources[ 0 ].segments.size() == 2 );

    TASK_CHECK( sources[ 0 ].segments[ 0 ].commandIndex == 0 );
    TASK_CHECK( sources[ 0 ].segments[ 0 ].mappedFileOffset.value() == 0x1800 );

    TASK_CHECK( sources[ 0 ].segments[ 1 ].commandIndex == 2 );
    TASK_CHECK( sources[ 0 ].segments[ 1 ].mappedFileOffset.value() == 0x2800 );
    TASK_CHECK( sources[ 0 ].segments[ 1 ].address == 0x100000800ull );

    return 0;
}

static int case_entrypoints_segment_after_entry( void )
{
    /* LC_MAIN first, segment later: same bytes, same result. */
    {
        auto bytes = image
        (
            CPU_X86_64,
            true,
            false,
            {
                mainCommand( 0x1800 ),
                segment64Command( "__TEXT", 0x100000000ull, 0x4000, 0x1000, 0x2000 )
            }
        );

        XS::IO::BinaryDataStream stream( bytes );
        MachO::File                file( stream );
        auto                       sources = file.entryPoints();

        TASK_CHECK( sources.size() == 1 );
        TASK_CHECK( sources[ 0 ].segments.size() == 1 );
        TASK_CHECK( sources[ 0 ].segments[ 0 ].commandIndex == 1 );
        TASK_CHECK( sources[ 0 ].segments[ 0 ].address == 0x100000800ull );
    }

    /* Same for LC_UNIXTHREAD preceding its segment. */
    {
        std::vector< RawRecord > records;
        records.push_back( { 4, 42, x86_64Registers( false, 0x100000800ull, 0x700000ull ) } );

        auto bytes = image
        (
            CPU_X86_64,
            true,
            false,
            {
                threadCommand( LC_UNIXTHREAD, false, records ),
                segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x1000 )
            }
        );

        XS::IO::BinaryDataStream stream( bytes );
        MachO::File                file( stream );
        auto                       sources = file.entryPoints();

        TASK_CHECK( sources.size() == 1 );
        TASK_CHECK( sources[ 0 ].segments.size() == 1 );
        TASK_CHECK( sources[ 0 ].segments[ 0 ].commandIndex == 1 );
        TASK_CHECK( sources[ 0 ].segments[ 0 ].mappedFileOffset.value() == 0x1800 );
    }

    return 0;
}

static int case_entrypoints_half_open_boundaries( void )
{
    /* MAIN exactly at fileoff + filesize is outside the half-open file range. */
    {
        auto bytes = image
        (
            CPU_X86_64,
            true,
            false,
            {
                segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x800 ),
                mainCommand( 0x1800 )
            }
        );

        XS::IO::BinaryDataStream stream( bytes );
        MachO::File                file( stream );
        auto                       sources = file.entryPoints();

        TASK_CHECK( sources.size() == 1 );
        TASK_CHECK( sources[ 0 ].segments.empty() );
    }

    /* MAIN at fileoff maps displacement zero. */
    {
        auto bytes = image
        (
            CPU_X86_64,
            true,
            false,
            {
                segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x800 ),
                mainCommand( 0x1000 )
            }
        );

        XS::IO::BinaryDataStream stream( bytes );
        MachO::File                file( stream );
        auto                       sources = file.entryPoints();

        TASK_CHECK( sources.size() == 1 );
        TASK_CHECK( sources[ 0 ].segments.size() == 1 );
        TASK_CHECK( sources[ 0 ].segments[ 0 ].address == 0x100000000ull );
        TASK_CHECK( sources[ 0 ].segments[ 0 ].mappedFileOffset.value() == 0x1000 );
    }

    /* Thread PC exactly at vmaddr + vmsize is outside the half-open VM range. */
    {
        std::vector< RawRecord > records;
        records.push_back( { 4, 42, x86_64Registers( false, 0x100002000ull, 0x700000ull ) } );

        auto bytes = image
        (
            CPU_X86_64,
            true,
            false,
            {
                segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x800 ),
                threadCommand( LC_UNIXTHREAD, false, records )
            }
        );

        XS::IO::BinaryDataStream stream( bytes );
        MachO::File                file( stream );
        auto                       sources = file.entryPoints();

        TASK_CHECK( sources.size() == 1 );
        TASK_CHECK( sources[ 0 ].segments.empty() );
    }

    /* Thread PC at vmaddr + filesize has a VA but no file offset. */
    {
        std::vector< RawRecord > records;
        records.push_back( { 4, 42, x86_64Registers( false, 0x100001800ull, 0x700000ull ) } );

        auto bytes = image
        (
            CPU_X86_64,
            true,
            false,
            {
                segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x800 ),
                threadCommand( LC_UNIXTHREAD, false, records )
            }
        );

        XS::IO::BinaryDataStream stream( bytes );
        MachO::File                file( stream );
        auto                       sources = file.entryPoints();

        TASK_CHECK( sources.size() == 1 );
        TASK_CHECK( sources[ 0 ].segments.size() == 1 );
        TASK_CHECK( sources[ 0 ].segments[ 0 ].mappedFileOffset.has_value() == false );
    }

    return 0;
}

static int case_entrypoints_unixthread_undecoded_skipped( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 0x99, 2, payloadU32( false, { 1, 2 } ) } );
    records.push_back( { 4, 42, x86_64Registers( false, 0x100000800ull, 0x700000ull ) } );
    records.push_back( { 7, 44, unifiedX86( false, 1, 16, i386Registers( false, 0x1000900u, 0x700000u ), 0xAA ) } );

    auto bytes = image
    (
        CPU_X86_64,
        true,
        false,
        {
            segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x1000 ),
            threadCommand( LC_UNIXTHREAD, false, records )
        }
    );

    XS::IO::BinaryDataStream stream( bytes );
    MachO::File                file( stream );
    auto                       sources = file.entryPoints();

    /* Undecoded record 0 is skipped; records 1 and 2 produce sources in order. */
    TASK_CHECK( sources.size() == 2 );
    TASK_CHECK( sources[ 0 ].recordIndex.value() == 1 );
    TASK_CHECK( sources[ 0 ].coordinate == 0x100000800ull );
    TASK_CHECK( sources[ 1 ].recordIndex.value() == 2 );
    TASK_CHECK( sources[ 1 ].coordinate == 0x1000900u );

    return 0;
}

static int case_entrypoints_source_order( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 4, 42, x86_64Registers( false, 0x100000800ull, 0x700000ull ) } );

    auto bytes = image
    (
        CPU_X86_64,
        true,
        false,
        {
            segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x1000 ),
            mainCommand( 0x1800 ),
            threadCommand( LC_UNIXTHREAD, false, records )
        }
    );

    XS::IO::BinaryDataStream stream( bytes );
    MachO::File                file( stream );
    auto                       sources = file.entryPoints();

    TASK_CHECK( sources.size() == 2 );
    TASK_CHECK( sources[ 0 ].kind == MachO::File::EntryPointKind::Main );
    TASK_CHECK( sources[ 0 ].commandIndex == 1 );
    TASK_CHECK( sources[ 0 ].recordIndex.has_value() == false );
    TASK_CHECK( sources[ 1 ].kind == MachO::File::EntryPointKind::UnixThread );
    TASK_CHECK( sources[ 1 ].commandIndex == 2 );
    TASK_CHECK( sources[ 1 ].recordIndex.value() == 0 );

    return 0;
}

static int case_entrypoints_big_endian( void )
{
    std::vector< RawRecord > records;

    records.push_back( { 4, 42, x86_64Registers( true, 0x100000800ull, 0x700000ull ) } );

    auto bytes = image
    (
        CPU_X86_64,
        true,
        true,
        {
            segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x1000, true ),
            threadCommand( LC_UNIXTHREAD, true, records ),
            mainCommand( 0x1800, 0, true )
        }
    );

    XS::IO::BinaryDataStream stream( bytes );
    MachO::File                file( stream );
    auto                       sources = file.entryPoints();

    TASK_CHECK( sources.size() == 2 );

    /* MAIN: file coordinate converted through the big-endian segment table. */
    TASK_CHECK( sources[ 0 ].kind == MachO::File::EntryPointKind::UnixThread );
    TASK_CHECK( sources[ 0 ].coordinate == 0x100000800ull );
    TASK_CHECK( sources[ 0 ].segments.size() == 1 );
    TASK_CHECK( sources[ 0 ].segments[ 0 ].address == 0x100000800ull );
    TASK_CHECK( sources[ 0 ].segments[ 0 ].mappedFileOffset.value() == 0x1800 );

    TASK_CHECK( sources[ 1 ].kind == MachO::File::EntryPointKind::Main );
    TASK_CHECK( sources[ 1 ].coordinate == 0x1800 );
    TASK_CHECK( sources[ 1 ].segments.size() == 1 );
    TASK_CHECK( sources[ 1 ].segments[ 0 ].address == 0x100000800ull );

    return 0;
}

static int case_entrypoints_macho32_segments( void )
{
    auto bytes = image
    (
        CPU_X86,
        false,
        false,
        {
            segment32Command( "__TEXT", 0x1000, 0x2000, 0x400, 0x1000 ),
            mainCommand( 0x800 )
        }
    );

    XS::IO::BinaryDataStream stream( bytes );
    MachO::File                file( stream );
    auto                       sources = file.entryPoints();

    TASK_CHECK( file.kind() == MachO::File::Kind::MachO32 );
    TASK_CHECK( sources.size() == 1 );
    TASK_CHECK( sources[ 0 ].segments.size() == 1 );
    TASK_CHECK( sources[ 0 ].segments[ 0 ].address == 0x1400 );
    TASK_CHECK( sources[ 0 ].segments[ 0 ].mappedFileOffset.value() == 0x800 );

    return 0;
}

static int case_entrypoints_fat_slices( void )
{
    /* x86_64 slice: one segment and an LC_MAIN. */
    auto sliceA = image
    (
        CPU_X86_64,
        true,
        false,
        {
            segment64Command( "__TEXT", 0x100000000ull, 0x2000, 0x1000, 0x1000 ),
            mainCommand( 0x1800 )
        }
    );

    /* ARM64 slice: its own segment table, LC_UNIXTHREAD flavor 6 and a raw
     * x86_64 flavor 4 record that must not decode with an ARM64 CPU. */
    std::vector< RawRecord > records;

    records.push_back( { 6, 68, arm64Registers( false, 0x200000400ull, 0x16FE00000ull ) } );
    records.push_back( { 4, 42, x86_64Registers( false, 0x200000800ull, 0x700000ull ) } );

    auto sliceB = image
    (
        CPU_ARM64,
        true,
        false,
        {
            segment64Command( "__TEXT", 0x200000000ull, 0x2000, 0x1000, 0x1000 ),
            threadCommand( LC_UNIXTHREAD, false, records )
        }
    );

    auto fat = fatImage( { sliceA, sliceB }, { CPU_X86_64, CPU_ARM64 } );

    XS::IO::BinaryDataStream stream( fat );
    MachO::FatFile             fatFile( stream );
    auto                       architectures = fatFile.architectures();

    TASK_CHECK( architectures.size() == 2 );
    TASK_CHECK( architectures[ 0 ].second.cpu().type() == CPU_X86_64 );
    TASK_CHECK( architectures[ 1 ].second.cpu().type() == CPU_ARM64 );

    /* Each slice resolves against its own segments and CPU. */
    auto sourcesA = architectures[ 0 ].second.entryPoints();
    auto sourcesB = architectures[ 1 ].second.entryPoints();

    TASK_CHECK( sourcesA.size() == 1 );
    TASK_CHECK( sourcesA[ 0 ].kind == MachO::File::EntryPointKind::Main );
    TASK_CHECK( sourcesA[ 0 ].segments.size() == 1 );
    TASK_CHECK( sourcesA[ 0 ].segments[ 0 ].address == 0x100000800ull );

    TASK_CHECK( sourcesB.size() == 1 );
    TASK_CHECK( sourcesB[ 0 ].kind == MachO::File::EntryPointKind::UnixThread );
    TASK_CHECK( sourcesB[ 0 ].recordIndex.value() == 0 );
    TASK_CHECK( sourcesB[ 0 ].coordinate == 0x200000400ull );
    TASK_CHECK( sourcesB[ 0 ].segments.size() == 1 );
    TASK_CHECK( sourcesB[ 0 ].segments[ 0 ].address == 0x200000400ull );
    TASK_CHECK( sourcesB[ 0 ].segments[ 0 ].mappedFileOffset.value() == 0x1400 );

    auto threadsB = architectures[ 1 ].second.loadCommands< MachO::LoadCommands::Thread >();

    TASK_CHECK( threadsB.size() == 1 );
    TASK_CHECK( threadsB[ 0 ].records().size() == 2 );
    TASK_CHECK( threadsB[ 0 ].records()[ 0 ].state.has_value() );
    TASK_CHECK( threadsB[ 0 ].records()[ 0 ].state->architecture() == MachO::LoadCommands::Thread::Architecture::ARM64 );
    TASK_CHECK( threadsB[ 0 ].records()[ 1 ].state.has_value() == false );

    /* Slice-relative coordinates: the fat offset is not folded in. */
    TASK_CHECK( architectures[ 0 ].first.offset() != 0 );
    TASK_CHECK( sourcesA[ 0 ].coordinate == 0x1800 );

    return 0;
}

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

const struct task_case task_cases[] =
{
    { "records", "unified_padding_then_unknown",        case_records_unified_padding_then_unknown },
    { "records", "flavor1_two_cpus",                    case_records_flavor1_two_cpus },
    { "records", "legacy_constructor_keeps_raw_records", case_records_legacy_constructor_keeps_raw_records },
    { "records", "invalid_known_counts",                case_records_invalid_known_counts },
    { "records", "truncated_and_overrun",               case_records_truncated_and_overrun },
    { "records", "big_endian",                          case_records_big_endian },
    { "records", "big_endian_file",                     case_records_big_endian_file },
    { "records", "copy_and_stream_release",             case_records_copy_and_stream_release },

    { "contexts", "x86_64",                             case_contexts_x86_64 },
    { "contexts", "i386_unsigned",                      case_contexts_i386_unsigned },
    { "contexts", "unified_inner_x86_64",               case_contexts_unified_inner_x86_64 },
    { "contexts", "arm32",                              case_contexts_arm32 },
    { "contexts", "arm64",                              case_contexts_arm64 },
    { "contexts", "unknown_cpu_and_family",             case_contexts_unknown_cpu_and_family },
    { "contexts", "get_info",                           case_contexts_get_info },

    { "entrypoints", "main_to_va",                      case_entrypoints_main_to_va },
    { "entrypoints", "main_no_segment",                 case_entrypoints_main_no_segment },
    { "entrypoints", "thread_file_and_zero_fill",       case_entrypoints_thread_file_and_zero_fill },
    { "entrypoints", "lc_thread_is_not_a_source",       case_entrypoints_lc_thread_is_not_a_source },
    { "entrypoints", "overlapping_segments",            case_entrypoints_overlapping_segments },
    { "entrypoints", "segment_after_entry",             case_entrypoints_segment_after_entry },
    { "entrypoints", "half_open_boundaries",            case_entrypoints_half_open_boundaries },
    { "entrypoints", "unixthread_undecoded_skipped",    case_entrypoints_unixthread_undecoded_skipped },
    { "entrypoints", "source_order",                    case_entrypoints_source_order },
    { "entrypoints", "macho32_segments",                case_entrypoints_macho32_segments },
    { "entrypoints", "big_endian",                      case_entrypoints_big_endian },
    { "entrypoints", "fat_slices",                      case_entrypoints_fat_slices },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[ 0 ] );
