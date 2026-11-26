/*
 * startup_regression.cpp
 *
 * Task regression for:
 *   - MachO::LoadCommands::Thread::records() raw records + decoded contexts
 *   - CPU dependent register decoding (i386 / x86_64 / x86 unified / ARM / ARM64)
 *   - MachO::File::entryPoints() coordinate translation and segment candidates
 *
 * All images are synthesized byte vectors; nothing is loaded or executed.
 * Test registration uses task_support/check.h and the groups
 * "records", "contexts" and "entrypoints" expected by task_support/run.sh.
 */

#include "../task_support/check.h"

#include <MachO/File.hpp>
#include <MachO/FatFile.hpp>
#include <MachO/LoadCommands/Thread.hpp>
#include <MachO/LoadCommands/EntryPoint.hpp>
#include <MachO/LoadCommands/Segment.hpp>
#include <MachO/LoadCommands/Segment64.hpp>
#include <XS/IO/BinaryDataStream.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using Bytes = std::vector< uint8_t >;
    using MachO::ThreadArchitecture;
    using MachO::ThreadRecord;
    using MachO::ThreadContext;
    using MachO::EntryPointSource;
    using MachO::EntryPointKind;

    /* ------------------------------------------------------------------ */
    /* Byte building                                                      */
    /* ------------------------------------------------------------------ */

    struct Writer
    {
        Bytes  b;
        bool   big = false;

        explicit Writer( bool isBig = false ): big( isBig ) {}

        void u32( uint32_t x )
        {
            for( int i = 0; i < 4; ++i )
            {
                unsigned shift = this->big ? static_cast< unsigned >( 24 - 8 * i )
                                           : static_cast< unsigned >( 8 * i );

                this->b.push_back( static_cast< uint8_t >( ( x >> shift ) & 0xffu ) );
            }
        }

        void u64( uint64_t x )
        {
            for( int i = 0; i < 8; ++i )
            {
                unsigned shift = this->big ? static_cast< unsigned >( 56 - 8 * i )
                                           : static_cast< unsigned >( 8 * i );

                this->b.push_back( static_cast< uint8_t >( ( x >> shift ) & 0xffu ) );
            }
        }

        void raw( const Bytes & x )
        {
            this->b.insert( this->b.end(), x.begin(), x.end() );
        }

        void zeros( size_t n )
        {
            this->b.insert( this->b.end(), n, 0 );
        }
    };

    Bytes payload32( const std::vector< uint32_t > & values, bool big )
    {
        Writer w( big );

        for( uint32_t v: values ) { w.u32( v ); }

        return w.b;
    }

    Bytes payload64( const std::vector< uint64_t > & values, bool big )
    {
        Writer w( big );

        for( uint64_t v: values ) { w.u64( v ); }

        return w.b;
    }

    Bytes threadRecord( uint32_t flavor, uint32_t count, const Bytes & payload, bool big = false )
    {
        Writer w( big );

        w.u32( flavor );
        w.u32( count );
        w.raw( payload );

        return w.b;
    }

    Bytes threadCommand( uint32_t lc, const Bytes & recordBytes, bool big = false )
    {
        Writer w( big );

        w.u32( lc );
        w.u32( static_cast< uint32_t >( 8 + recordBytes.size() ) );
        w.raw( recordBytes );

        return w.b;
    }

    /* Command with an explicitly controlled cmdsize / body. */
    Bytes rawThreadCommand( uint32_t lc, uint32_t cmdSize, const Bytes & body, bool big = false )
    {
        Writer w( big );

        w.u32( lc );
        w.u32( cmdSize );
        w.raw( body );

        return w.b;
    }

    Bytes mainCommand( uint64_t offset, uint64_t stackSize = 0, bool big = false )
    {
        Writer w( big );

        w.u32( 0x80000028u ); /* LC_MAIN */
        w.u32( 24 );
        w.u64( offset );
        w.u64( stackSize );

        return w.b;
    }

    Bytes segment64Command( const std::string & name,
                            uint64_t va, uint64_t vs,
                            uint64_t fo, uint64_t fs,
                            bool big = false )
    {
        Writer w( big );

        w.u32( 0x19 ); /* LC_SEGMENT_64 */
        w.u32( 72 );

        Bytes fixedName( 16, 0 );
        std::memcpy( fixedName.data(), name.data(), name.size() < 16 ? name.size() : 16 );

        w.raw( fixedName );
        w.u64( va );
        w.u64( vs );
        w.u64( fo );
        w.u64( fs );
        w.u32( 7 ); /* maxprot */
        w.u32( 5 ); /* initprot */
        w.u32( 0 ); /* nsects */
        w.u32( 0 ); /* flags */

        return w.b;
    }

    Bytes segment32Command( const std::string & name,
                            uint32_t va, uint32_t vs,
                            uint32_t fo, uint32_t fs,
                            bool big = false )
    {
        Writer w( big );

        w.u32( 0x01 ); /* LC_SEGMENT */
        w.u32( 56 );

        Bytes fixedName( 16, 0 );
        std::memcpy( fixedName.data(), name.data(), name.size() < 16 ? name.size() : 16 );

        w.raw( fixedName );
        w.u32( va );
        w.u32( vs );
        w.u32( fo );
        w.u32( fs );
        w.u32( 7 );
        w.u32( 5 );
        w.u32( 0 );
        w.u32( 0 );

        return w.b;
    }

    struct Image
    {
        bool                big = false;
        bool                m64 = true;
        uint32_t            cpu = 0x01000007u; /* x86_64 */
        std::vector< Bytes > commands;

        Bytes build() const
        {
            Writer h( this->big );

            uint32_t magic;

            /*
             * Always use the logical MH magic; the Writer emits it in the
             * image byte order. The parser reads the first word with the
             * default host order and recognizes the byte swapped form for
             * big endian images.
             */
            if( this->m64 ) { magic = 0xFEEDFACFu; }
            else            { magic = 0xFEEDFACEu; }

            uint32_t sizeOfCommands = 0;

            for( const Bytes & c: this->commands )
            {
                sizeOfCommands += static_cast< uint32_t >( c.size() );
            }

            h.u32( magic );
            h.u32( this->cpu );
            h.u32( 3 ); /* subtype */
            h.u32( 2 ); /* MH_EXECUTE */
            h.u32( static_cast< uint32_t >( this->commands.size() ) );
            h.u32( sizeOfCommands );
            h.u32( 0 ); /* flags */

            if( this->m64 )
            {
                h.u32( 0 ); /* reserved */
            }

            for( const Bytes & c: this->commands )
            {
                h.raw( c );
            }

            return h.b;
        }
    };

    /* ------------------------------------------------------------------ */
    /* Parsing helpers (stream is released before the File returns)       */
    /* ------------------------------------------------------------------ */

    MachO::File parseFile( const Bytes & image )
    {
        XS::IO::BinaryDataStream stream( image );

        return MachO::File( stream );
    }
    /* 0 = no throw, 1 = descriptive std::exception, -1 = empty what(), 2 = other. */
    int parseErrorCode( const Bytes & image )
    {
        try
        {
            XS::IO::BinaryDataStream stream( image );
            MachO::File              file( stream );

            return 0;
        }
        catch( const std::exception & e )
        {
            return ( e.what() != nullptr && e.what()[ 0 ] != '\0' ) ? 1 : -1;
        }
        catch( ... )
        {
            return 2;
        }
    }

    MachO::FatFile parseFat( const Bytes & image )
    {
        XS::IO::BinaryDataStream stream( image );

        return MachO::FatFile( stream );
    }

    bool contains( const std::string & haystack, const std::string & needle )
    {
        return haystack.find( needle ) != std::string::npos;
    }

    /* Known-state payloads. */

    Bytes x86_64State( uint64_t rip, uint64_t rsp, bool big, uint64_t marker = 0 )
    {
        std::vector< uint64_t > v( 21, 0 );

        v[ 7 ]  = rsp;
        v[ 15 ] = marker;
        v[ 16 ] = rip;

        return payload64( v, big );
    }

    Bytes i386State( uint32_t eip, uint32_t esp, bool big )
    {
        std::vector< uint32_t > v( 16, 0 );

        v[ 1 ]  = 0xDEADBEEFu; /* ebx: high bit set, must stay zero extended */
        v[ 7 ]  = esp;
        v[ 10 ] = eip;

        return payload32( v, big );
    }

    Bytes armState( uint32_t pc, uint32_t sp, bool big )
    {
        std::vector< uint32_t > v( 17, 0 );

        for( uint32_t i = 0; i <= 12; ++i ) { v[ i ] = 0x1000u + i; }

        v[ 13 ] = sp;
        v[ 15 ] = pc;
        v[ 16 ] = 0x60000000u; /* cpsr */

        return payload32( v, big );
    }

    Bytes arm64State( uint64_t pc, uint64_t sp, bool big )
    {
        Writer w( big );

        for( uint64_t i = 0; i < 33; ++i )
        {
            w.u64( i == 31 ? sp : ( i == 32 ? pc : 0xA000u + i ) );
        }

        w.u32( 0x60000000u );          /* cpsr */
        w.raw( { 0xAA, 0xBB, 0xCC, 0xDD } ); /* trailing padding */

        return w.b; /* 33*8 + 4 + 4 = 272 = 68*4 */
    }

    Bytes unifiedPayload( uint32_t innerFlavor, uint32_t innerCount, const Bytes & innerState, bool big )
    {
        Writer w( big );

        w.u32( innerFlavor );
        w.u32( innerCount );

        Bytes unionBytes = innerState;

        unionBytes.resize( 168, 0 ); /* fixed 168 byte union, padding retained */
        w.raw( unionBytes );

        return w.b; /* 8 + 168 = 176 = 44*4 */
    }
}

/* ================================================================== */
/* records group                                                      */
/* ================================================================== */

static int records_fields_order_and_bytes( void )
{
    Bytes marker = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60,
                     0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0 };

    Bytes rec0 = threadRecord( 4, 42, x86_64State( 0x100000810ull, 0x7000, false ) );
    Bytes rec1 = threadRecord( 0x1234, 3, marker );
    Bytes rec2 = threadRecord( 0x4242, 0, {} );

    Image img;

    img.cpu = 0x01000007u;

    Bytes allRecords = rec0;

    allRecords.insert( allRecords.end(), rec1.begin(), rec1.end() );
    allRecords.insert( allRecords.end(), rec2.begin(), rec2.end() );
    img.commands.push_back( threadCommand( 0x04, allRecords ) ); /* LC_THREAD */

    MachO::File file = parseFile( img.build() );
    auto threads = file.loadCommands< MachO::LoadCommands::Thread >();

    TASK_CHECK( threads.size() == 1 );

    std::vector< ThreadRecord > records = threads[ 0 ].records();

    TASK_CHECK( records.size() == 3 );

    /* First record: x86_64 flavor 4 / count 42. */
    TASK_CHECK( records[ 0 ].flavor() == 4 );
    TASK_CHECK( records[ 0 ].count() == 42 );
    TASK_CHECK( records[ 0 ].offset() == 8 );
    TASK_CHECK( records[ 0 ].offsetInCommand() == 8 );
    TASK_CHECK( records[ 0 ].payload().size() == 168 );
    TASK_CHECK( records[ 0 ].bytes().size() == 176 );
    TASK_CHECK( records[ 0 ].bytes()[ 0 ] == 0x04 ); /* flavor little endian */
    TASK_CHECK( records[ 0 ].bytes()[ 3 ] == 0x00 );
    TASK_CHECK( records[ 0 ].bytes()[ 4 ] == 42 );    /* count 42 = 0x2A */
    TASK_CHECK( records[ 0 ].bytes()[ 5 ] == 0x00 );
    TASK_CHECK( records[ 0 ].decoded() );

    /* Second record: unknown flavor, raw bytes preserved. */
    TASK_CHECK( records[ 1 ].flavor() == 0x1234 );
    TASK_CHECK( records[ 1 ].count() == 3 );
    TASK_CHECK( records[ 1 ].offset() == 184 ); /* 8 + 176 */
    TASK_CHECK( records[ 1 ].payload().size() == 12 );
    TASK_CHECK( records[ 1 ].bytes().size() == 20 );
    TASK_CHECK( records[ 1 ].payload() == marker );
    TASK_CHECK( records[ 1 ].context().has_value() == false );
    TASK_CHECK( records[ 1 ].decoded() == false );

    /* Third record: unknown zero count is contained and preserved. */
    TASK_CHECK( records[ 2 ].flavor() == 0x4242 );
    TASK_CHECK( records[ 2 ].count() == 0 );
    TASK_CHECK( records[ 2 ].offset() == 204 ); /* 184 + 20 */
    TASK_CHECK( records[ 2 ].payload().empty() );
    TASK_CHECK( records[ 2 ].bytes().size() == 8 );
    TASK_CHECK( records[ 2 ].offset() + records[ 2 ].bytes().size() == 212 );

    return 0;
}

static int records_legacy_constructor_has_records_no_context( void )
{
    Bytes rec0 = threadRecord( 4, 42, x86_64State( 0x1000, 0x2000, false ) );
    Bytes rec1 = threadRecord( 0x9999, 1, { 1, 2, 3, 4 } );
    Bytes body = rec0;

    body.insert( body.end(), rec1.begin(), rec1.end() );

    Bytes cmd = threadCommand( 0x05, body );

    XS::IO::BinaryDataStream stream( cmd );

    stream.seek( 8, XS::IO::BinaryStream::SeekDirection::Begin );

    /* Old 5 argument interface: no CPU, records only. */
    MachO::LoadCommands::Thread legacy( 0x05, static_cast< uint32_t >( cmd.size() ),
                                       MachO::File::Kind::MachO64, stream );

    std::vector< ThreadRecord > a = legacy.records();

    TASK_CHECK( a.size() == 2 );
    TASK_CHECK( a[ 0 ].flavor() == 4 );
    TASK_CHECK( a[ 0 ].context().has_value() == false );
    TASK_CHECK( a[ 0 ].state().has_value() == false );
    TASK_CHECK( a[ 1 ].flavor() == 0x9999 );
    TASK_CHECK( a[ 1 ].payload().size() == 4 );

    /* Copy construction and copy assignment keep owned bytes. */
    MachO::LoadCommands::Thread copied( legacy );

    XS::IO::BinaryDataStream dummyStream( cmd );

    dummyStream.seek( 8, XS::IO::BinaryStream::SeekDirection::Begin );

    MachO::LoadCommands::Thread assigned( 0x05, static_cast< uint32_t >( cmd.size() ),
                                          MachO::File::Kind::MachO64, dummyStream );

    assigned = copied;

    TASK_CHECK( copied.records().size() == 2 );
    TASK_CHECK( assigned.records()[ 0 ].payload() == a[ 0 ].payload() );
    TASK_CHECK( assigned.records()[ 1 ].flavor() == 0x9999 );

    /* CPU aware direct constructor decodes. */
    XS::IO::BinaryDataStream stream2( cmd );

    stream2.seek( 8, XS::IO::BinaryStream::SeekDirection::Begin );

    MachO::LoadCommands::Thread aware( 0x05, static_cast< uint32_t >( cmd.size() ),
                                       MachO::File::Kind::MachO64, 0x01000007u, stream2 );

    TASK_CHECK( aware.records().size() == 2 );
    TASK_CHECK( aware.records()[ 0 ].context().has_value() );
    TASK_CHECK( aware.records()[ 0 ].context()->architecture() == ThreadArchitecture::X86_64 );
    TASK_CHECK( aware.records()[ 1 ].context().has_value() == false );

    return 0;
}

static int records_survive_file_copy_and_stream_release( void )
{
    Image img;

    img.commands.push_back( threadCommand( 0x05, threadRecord( 4, 42, x86_64State( 0xABCDEF, 0x1234, false ) ) ) );

    MachO::File  file  = parseFile( img.build() ); /* stream already released */
    MachO::File  copy  = file;
    MachO::File  moved = std::move( file );

    auto threads = copy.loadCommands< MachO::LoadCommands::Thread >();

    TASK_CHECK( threads.size() == 1 );

    std::vector< ThreadRecord > records = threads[ 0 ].records();

    TASK_CHECK( records.size() == 1 );
    TASK_CHECK( records[ 0 ].context().has_value() );
    TASK_CHECK( records[ 0 ].context()->pc() == 0xABCDEF );
    TASK_CHECK( records[ 0 ].context()->sp() == 0x1234 );

    auto movedThreads = moved.loadCommands< MachO::LoadCommands::Thread >();

    TASK_CHECK( movedThreads.size() == 1 );
    TASK_CHECK( movedThreads[ 0 ].records()[ 0 ].context()->pc() == 0xABCDEF );

    /* Records own their bytes and decoded state, so they outlive the File. */
    std::vector< ThreadRecord > saved;

    {
        MachO::File scoped = parseFile( img.build() );

        saved = scoped.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records();
    }

    TASK_CHECK( saved.size() == 1 );
    TASK_CHECK( saved[ 0 ].flavor() == 4 );
    TASK_CHECK( saved[ 0 ].payload().size() == 168 );
    TASK_CHECK( saved[ 0 ].bytes().size() == 176 );
    TASK_CHECK( saved[ 0 ].context().has_value() );
    TASK_CHECK( saved[ 0 ].context()->pc() == 0xABCDEF );

    return 0;
}

static int records_invalid_ranges_and_lengths_throw( void )
{
    /* Payload runs past the command. */
    {
        Bytes body;
        Writer w;

        w.u32( 1 ); /* flavor */
        w.u32( 4 ); /* count -> 16 payload bytes, none present */
        body = w.b;

        Image img;
        img.commands.push_back( rawThreadCommand( 0x04, 16, body ) );

        TASK_CHECK( parseErrorCode( img.build() ) == 1 );
    }

    /* Trailing bytes too small for another record header. */
    {
        Bytes body( 4, 0 );

        Image img;
        img.commands.push_back( rawThreadCommand( 0x04, 12, body ) );

        TASK_CHECK( parseErrorCode( img.build() ) == 1 );
    }

    /* Count overflow / implausibly large contained span is rejected. */
    {
        Writer w;

        w.u32( 1 );
        w.u32( 0xFFFFFFFFu );

        Image img;
        img.commands.push_back( rawThreadCommand( 0x04, 16, w.b ) );

        TASK_CHECK( parseErrorCode( img.build() ) == 1 );
    }

    /* Known x86_64 flavor 4 with the wrong count is a descriptive error. */
    {
        Bytes payload( 41 * 4, 0 );
        Bytes cmd = threadCommand( 0x05, threadRecord( 4, 41, payload ) );

        Image img;
        img.cpu = 0x01000007u;
        img.commands.push_back( cmd );

        TASK_CHECK( parseErrorCode( img.build() ) == 1 );
    }

    /* Unified outer count must be exactly 44. */
    {
        Bytes payload( 43 * 4, 0 );
        Bytes cmd = threadCommand( 0x05, threadRecord( 7, 43, payload ) );

        Image img;
        img.cpu = 7;
        img.m64 = false;
        img.commands.push_back( cmd );

        TASK_CHECK( parseErrorCode( img.build() ) == 1 );
    }

    /* Unified inner known flavor with wrong inner count throws. */
    {
        Bytes payload( 176, 0 );

        payload[ 0 ] = 4; /* inner flavor 4, little endian */
        payload[ 4 ] = 41; /* wrong inner count */

        Bytes cmd = threadCommand( 0x05, threadRecord( 7, 44, payload ) );

        Image img;
        img.cpu = 0x01000007u;
        img.commands.push_back( cmd );

        TASK_CHECK( parseErrorCode( img.build() ) == 1 );
    }

    /* Command smaller than the load command header is rejected. */
    {
        Image img;
        img.commands.push_back( rawThreadCommand( 0x04, 4, {} ) );

        TASK_CHECK( parseErrorCode( img.build() ) == 1 );
    }

    return 0;
}

/* ================================================================== */
/* contexts group                                                     */
/* ================================================================== */

static int contexts_i386( void )
{
    Image img;

    img.m64 = false;
    img.cpu = 7;
    img.commands.push_back( threadCommand( 0x05, threadRecord( 1, 16, i386State( 0x00401234u, 0x00200000u, false ) ) ) );

    MachO::File file = parseFile( img.build() );
    auto rec = file.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records()[ 0 ];

    TASK_CHECK( rec.context().has_value() );

    const ThreadContext ctx = *rec.context();
    static const char * names[] =
    {
        "eax", "ebx", "ecx", "edx", "edi", "esi", "ebp", "esp",
        "ss", "eflags", "eip", "cs", "ds", "es", "fs", "gs"
    };

    TASK_CHECK( ctx.architecture() == ThreadArchitecture::I386 );
    TASK_CHECK( ctx.architectureName() == std::string( "i386" ) );
    TASK_CHECK( ctx.flavor() == 1 );
    TASK_CHECK( ctx.innerFlavor().has_value() == false );
    TASK_CHECK( ctx.registers().size() == 16 );

    for( size_t i = 0; i < 16; ++i )
    {
        TASK_CHECK( ctx.registers()[ i ].name == names[ i ] );
    }

    TASK_CHECK( ctx.registers()[ 1 ].value == 0x00000000DEADBEEFull ); /* no sign extension */
    TASK_CHECK( ctx.pc() == 0x00401234u );
    TASK_CHECK( ctx.sp() == 0x00200000u );
    TASK_CHECK( ctx.registerValue( "eip" ) == std::optional< uint64_t >( 0x00401234u ) );
    TASK_CHECK( ctx.registerValue( "missing" ).has_value() == false );

    return 0;
}

static int contexts_x86_64( void )
{
    Image img;

    img.cpu = 0x01000007u;
    img.commands.push_back( threadCommand( 0x05,
        threadRecord( 4, 42, x86_64State( 0x100000810ull, 0x7FFF0000ull, false, 0x123456789ABCDEF0ull ) ) ) );

    MachO::File file = parseFile( img.build() );
    const ThreadContext ctx = *file.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records()[ 0 ].context();

    static const char * names[] =
    {
        "rax", "rbx", "rcx", "rdx", "rdi", "rsi", "rbp", "rsp",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "rip", "rflags", "cs", "fs", "gs"
    };

    TASK_CHECK( ctx.architecture() == ThreadArchitecture::X86_64 );
    TASK_CHECK( ctx.architectureName() == std::string( "x86_64" ) );
    TASK_CHECK( ctx.registers().size() == 21 );

    for( size_t i = 0; i < 21; ++i )
    {
        TASK_CHECK( ctx.registers()[ i ].name == names[ i ] );
    }

    TASK_CHECK( ctx.registers()[ 15 ].value == 0x123456789ABCDEF0ull );
    TASK_CHECK( ctx.pc() == 0x100000810ull );
    TASK_CHECK( ctx.sp() == 0x7FFF0000ull );
    TASK_CHECK( ctx.registerValue( "rip" ) == std::optional< uint64_t >( 0x100000810ull ) );

    return 0;
}

static int contexts_unified_and_unknown_coexist( void )
{
    Bytes x64Inner = x86_64State( 0x100000999ull, 0x7010ull, false );
    Bytes unified  = threadRecord( 7, 44, unifiedPayload( 4, 42, x64Inner, false ) );
    Bytes unknown  = threadRecord( 0x7777, 2, { 1, 2, 3, 4, 5, 6, 7, 8 } );

    Image img;

    img.cpu = 0x01000007u;

    Bytes both = unified;

    both.insert( both.end(), unknown.begin(), unknown.end() );
    img.commands.push_back( threadCommand( 0x05, both ) );

    MachO::File file = parseFile( img.build() );
    std::vector< ThreadRecord > recs = file.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records();

    TASK_CHECK( recs.size() == 2 );

    /* Outer identity stays flavor 7 / count 44 / offset 8. */
    TASK_CHECK( recs[ 0 ].flavor() == 7 );
    TASK_CHECK( recs[ 0 ].count() == 44 );
    TASK_CHECK( recs[ 0 ].offset() == 8 );
    TASK_CHECK( recs[ 0 ].context().has_value() );
    TASK_CHECK( recs[ 0 ].context()->flavor() == 7 );
    TASK_CHECK( recs[ 0 ].context()->innerFlavor() == std::optional< uint32_t >( 4 ) );
    TASK_CHECK( recs[ 0 ].context()->architecture() == ThreadArchitecture::X86_64 );
    TASK_CHECK( recs[ 0 ].context()->pc() == 0x100000999ull );
    TASK_CHECK( recs[ 0 ].context()->sp() == 0x7010ull );

    /* Unknown sibling remains raw. */
    TASK_CHECK( recs[ 1 ].flavor() == 0x7777 );
    TASK_CHECK( recs[ 1 ].offset() == 192 ); /* 8 + 184 */
    TASK_CHECK( recs[ 1 ].context().has_value() == false );

    /* Unified with inner i386 state decodes as i386. */
    {
        Bytes u = threadRecord( 7, 44, unifiedPayload( 1, 16, i386State( 0x405000, 0x1000, false ), false ) );

        Image i2;
        i2.cpu = 7;
        i2.m64 = false;
        i2.commands.push_back( threadCommand( 0x05, u ) );

        MachO::File f2 = parseFile( i2.build() );
        const ThreadContext c = *f2.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records()[ 0 ].context();

        TASK_CHECK( c.architecture() == ThreadArchitecture::I386 );
        TASK_CHECK( c.innerFlavor() == std::optional< uint32_t >( 1 ) );
        TASK_CHECK( c.pc() == 0x405000u );
    }

    /* Unknown inner flavor: whole outer record undecoded, no extra record. */
    {
        Bytes u = threadRecord( 7, 44, unifiedPayload( 99, 99, {}, false ) );

        Image i3;
        i3.cpu = 0x01000007u;
        i3.commands.push_back( threadCommand( 0x05, u ) );

        MachO::File f3 = parseFile( i3.build() );
        std::vector< ThreadRecord > rs = f3.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records();

        TASK_CHECK( rs.size() == 1 );
        TASK_CHECK( rs[ 0 ].flavor() == 7 );
        TASK_CHECK( rs[ 0 ].context().has_value() == false );
        TASK_CHECK( rs[ 0 ].payload().size() == 176 );
    }

    return 0;
}

static int contexts_same_flavor_one_two_cpus( void )
{
    /* flavor 1 on an i386 CPU: 16 u32 i386 state, and the image is MachO32. */
    Image x86;

    x86.m64 = false;
    x86.cpu = 7;
    x86.commands.push_back( threadCommand( 0x05, threadRecord( 1, 16, i386State( 0x401000, 0x2000, false ) ) ) );

    MachO::File fx = parseFile( x86.build() );
    const ThreadContext cx = *fx.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records()[ 0 ].context();

    TASK_CHECK( fx.kind() == MachO::File::Kind::MachO32 );
    TASK_CHECK( cx.architecture() == ThreadArchitecture::I386 );
    TASK_CHECK( cx.registers()[ 0 ].name == std::string( "eax" ) );
    TASK_CHECK( cx.pc() == 0x401000u );

    /* Same flavor number 1 on an ARM CPU: 17 u32 ARM state, also MachO32. */
    Image arm;

    arm.m64 = false;
    arm.cpu = 12;
    arm.commands.push_back( threadCommand( 0x05, threadRecord( 1, 17, armState( 0x8000, 0x9000, false ) ) ) );

    MachO::File fa = parseFile( arm.build() );
    const ThreadContext ca = *fa.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records()[ 0 ].context();

    TASK_CHECK( fa.kind() == MachO::File::Kind::MachO32 );
    TASK_CHECK( ca.architecture() == ThreadArchitecture::ARM );
    TASK_CHECK( ca.registers()[ 0 ].name == std::string( "r0" ) );
    TASK_CHECK( ca.pc() == 0x8000u );
    TASK_CHECK( ca.sp() == 0x9000u );

    /* Length validation follows the CPU specific layout. */
    {
        Image badX86;
        badX86.m64 = false;
        badX86.cpu = 7;
        badX86.commands.push_back( threadCommand( 0x05, threadRecord( 1, 17, Bytes( 68, 0 ) ) ) );
        TASK_CHECK( parseErrorCode( badX86.build() ) == 1 );

        Image badArm;
        badArm.m64 = false;
        badArm.cpu = 12;
        badArm.commands.push_back( threadCommand( 0x05, threadRecord( 1, 16, Bytes( 64, 0 ) ) ) );
        TASK_CHECK( parseErrorCode( badArm.build() ) == 1 );
    }

    /* flavor 1 on x86_64 is not one of the supported known combinations. */
    {
        Image weird;
        weird.cpu = 0x01000007u;
        weird.commands.push_back( threadCommand( 0x05, threadRecord( 1, 16, Bytes( 64, 0 ) ) ) );

        MachO::File fw = parseFile( weird.build() );
        TASK_CHECK( fw.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records()[ 0 ].context().has_value() == false );
    }

    return 0;
}

static int contexts_arm_and_arm64_layouts( void )
{
    /* ARM flavor 1 / count 17. */
    {
        Image img;
        img.m64 = false;
        img.cpu = 12;
        img.commands.push_back( threadCommand( 0x05, threadRecord( 1, 17, armState( 0x12345, 0x7FF00, false ) ) ) );

        MachO::File file = parseFile( img.build() );
        const ThreadContext ctx = *file.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records()[ 0 ].context();

        TASK_CHECK( ctx.architecture() == ThreadArchitecture::ARM );
        TASK_CHECK( ctx.registers().size() == 17 );

        for( int i = 0; i <= 12; ++i )
        {
            TASK_CHECK( ctx.registers()[ i ].name == "r" + std::to_string( i ) );
        }

        TASK_CHECK( ctx.registers()[ 13 ].name == std::string( "sp" ) );
        TASK_CHECK( ctx.registers()[ 14 ].name == std::string( "lr" ) );
        TASK_CHECK( ctx.registers()[ 15 ].name == std::string( "pc" ) );
        TASK_CHECK( ctx.registers()[ 16 ].name == std::string( "cpsr" ) );
        TASK_CHECK( ctx.pc() == 0x12345u );
        TASK_CHECK( ctx.sp() == 0x7FF00u );
    }

    /* ARM64 flavor 6 / count 68, with cpsr and trailing padding. */
    {
        Image img;
        img.cpu = 0x0100000Cu;
        img.commands.push_back( threadCommand( 0x05, threadRecord( 6, 68, arm64State( 0x100000800ull, 0x800000ull, false ) ) ) );

        MachO::File file = parseFile( img.build() );
        ThreadRecord rec = file.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records()[ 0 ];
        const ThreadContext ctx = *rec.context();

        TASK_CHECK( ctx.architecture() == ThreadArchitecture::ARM64 );
        TASK_CHECK( ctx.registers().size() == 34 ); /* 33 GPR-ish + cpsr */
        TASK_CHECK( rec.payload().size() == 272 );

        TASK_CHECK( ctx.registers()[ 0 ].name == std::string( "x0" ) );
        TASK_CHECK( ctx.registers()[ 28 ].name == std::string( "x28" ) );
        TASK_CHECK( ctx.registers()[ 29 ].name == std::string( "fp" ) );
        TASK_CHECK( ctx.registers()[ 30 ].name == std::string( "lr" ) );
        TASK_CHECK( ctx.registers()[ 31 ].name == std::string( "sp" ) );
        TASK_CHECK( ctx.registers()[ 32 ].name == std::string( "pc" ) );
        TASK_CHECK( ctx.registers()[ 33 ].name == std::string( "cpsr" ) );

        TASK_CHECK( ctx.sp() == 0x800000ull );
        TASK_CHECK( ctx.pc() == 0x100000800ull );
        TASK_CHECK( ctx.registerValue( "cpsr" ) == std::optional< uint64_t >( 0x60000000u ) );

        /* Padding bytes are raw payload, never a register. */
        TASK_CHECK( rec.payload()[ 268 ] == 0xAA );
        TASK_CHECK( rec.payload()[ 271 ] == 0xDD );

        bool hasPaddingRegister = false;

        for( const auto & r: ctx.registers() )
        {
            if( contains( r.name, "pad" ) ) { hasPaddingRegister = true; }
        }

        TASK_CHECK( hasPaddingRegister == false );
    }

    return 0;
}

static int contexts_big_endian( void )
{
    Bytes state = x86_64State( 0x100000810ull, 0x7FFF0000ull, true, 0x0102030405060708ull );

    Image img;

    img.big = true;
    img.m64 = true;
    img.cpu = 0x01000007u;
    img.commands.push_back( threadCommand( 0x05, threadRecord( 4, 42, state, true ), true ) );

    MachO::File file = parseFile( img.build() );

    TASK_CHECK( file.endianness() == MachO::File::Endianness::BigEndian );

    ThreadRecord rec = file.loadCommands< MachO::LoadCommands::Thread >()[ 0 ].records()[ 0 ];

    TASK_CHECK( rec.flavor() == 4 );
    TASK_CHECK( rec.count() == 42 );
    TASK_CHECK( rec.offset() == 8 );
    TASK_CHECK( rec.context().has_value() );
    TASK_CHECK( rec.context()->architecture() == ThreadArchitecture::X86_64 );
    TASK_CHECK( rec.context()->pc() == 0x100000810ull );
    TASK_CHECK( rec.context()->sp() == 0x7FFF0000ull );
    TASK_CHECK( rec.context()->registers()[ 15 ].value == 0x0102030405060708ull );

    return 0;
}

static int contexts_getinfo_shows_records_and_registers( void )
{
    Image img;

    img.cpu = 0x01000007u;
    img.commands.push_back( threadCommand( 0x04, threadRecord( 4, 42, x86_64State( 0x100000810, 0x7000, false ) ) ) );

    MachO::File file = parseFile( img.build() );
    MachO::LoadCommands::Thread thread = file.loadCommands< MachO::LoadCommands::Thread >()[ 0 ];

    std::string info = thread.getInfo().toString();

    TASK_CHECK( contains( info, "Thread records" ) );
    TASK_CHECK( contains( info, "Record 0" ) );
    TASK_CHECK( contains( info, "rip" ) );
    TASK_CHECK( contains( info, "rsp" ) );
    TASK_CHECK( contains( info, "rax" ) );
    TASK_CHECK( contains( info, "PC" ) );

    /* Whole file info must also build without throwing. */
    std::string fileInfo = file.getInfo().toString();

    TASK_CHECK( contains( fileInfo, "Mach-O file" ) );

    return 0;
}

/* ================================================================== */
/* entrypoints group                                                  */
/* ================================================================== */

static int entrypoints_lc_main_file_offset_to_va( void )
{
    Image img;

    /* Entry command comes first; segment intentionally comes after it. */
    img.commands.push_back( mainCommand( 0x810, 0x4000 ) );
    img.commands.push_back( segment64Command( "__TEXT", 0x100000000ull, 0x1000ull, 0, 0x1000 ) );

    MachO::File file = parseFile( img.build() );
    std::vector< EntryPointSource > eps = file.entryPoints();

    TASK_CHECK( eps.size() == 1 );
    TASK_CHECK( eps[ 0 ].kind == EntryPointKind::Main );
    TASK_CHECK( eps[ 0 ].commandIndex == 0 );
    TASK_CHECK( eps[ 0 ].recordIndex.has_value() == false );
    TASK_CHECK( eps[ 0 ].command == 0x80000028u );
    TASK_CHECK( eps[ 0 ].coordinate == 0x810 );
    TASK_CHECK( eps[ 0 ].candidates.size() == 1 );

    const auto & c = eps[ 0 ].candidates[ 0 ];

    TASK_CHECK( c.segmentIndex == 1 );
    TASK_CHECK( c.segmentName == std::string( "__TEXT" ) );
    TASK_CHECK( c.address == 0x100000810ull );
    TASK_CHECK( c.mappedFileOffset.has_value() );
    TASK_CHECK( *c.mappedFileOffset == 0x810 );
    TASK_CHECK( c.vmAddress == 0x100000000ull );

    /* entryPointSources is an alias with identical content. */
    TASK_CHECK( file.entryPointSources().size() == 1 );

    /* Survives a File copy with the stream long gone. */
    MachO::File copy = file;

    TASK_CHECK( copy.entryPoints()[ 0 ].candidates[ 0 ].address == 0x100000810ull );

    return 0;
}

static int entrypoints_unixthread_pc_to_file( void )
{
    Image img;

    img.commands.push_back( threadCommand( 0x05,
        threadRecord( 4, 42, x86_64State( 0x100000810ull, 0x7000, false ) ) ) );
    img.commands.push_back( segment64Command( "__TEXT", 0x100000000ull, 0x2000ull, 0, 0x1000 ) );

    MachO::File file = parseFile( img.build() );
    std::vector< EntryPointSource > eps = file.entryPoints();

    TASK_CHECK( eps.size() == 1 );
    TASK_CHECK( eps[ 0 ].kind == EntryPointKind::UnixThread );
    TASK_CHECK( eps[ 0 ].commandIndex == 0 );
    TASK_CHECK( eps[ 0 ].recordIndex == std::optional< size_t >( 0 ) );
    TASK_CHECK( eps[ 0 ].coordinate == 0x100000810ull );
    TASK_CHECK( eps[ 0 ].candidates.size() == 1 );
    TASK_CHECK( eps[ 0 ].candidates[ 0 ].segmentIndex == 1 );
    TASK_CHECK( eps[ 0 ].candidates[ 0 ].address == 0x100000810ull );
    TASK_CHECK( eps[ 0 ].candidates[ 0 ].mappedFileOffset == std::optional< uint64_t >( 0x810 ) );

    return 0;
}

static int entrypoints_lc_thread_not_an_entry( void )
{
    Bytes unixRecords = threadRecord( 4, 42, x86_64State( 0x100000100, 0x7000, false ) );
    Bytes unknown     = threadRecord( 999, 2, Bytes( 8, 1 ) );
    Bytes unified     = threadRecord( 7, 44,
                                      unifiedPayload( 4, 42, x86_64State( 0x100000200, 0x7000, false ), false ) );

    unixRecords.insert( unixRecords.end(), unknown.begin(), unknown.end() );
    unixRecords.insert( unixRecords.end(), unified.begin(), unified.end() );

    Image img;

    img.commands.push_back( threadCommand( 0x04, threadRecord( 4, 42, x86_64State( 0x100000300, 0x7000, false ) ) ) );
    img.commands.push_back( threadCommand( 0x05, unixRecords ) );
    img.commands.push_back( segment64Command( "__TEXT", 0x100000000ull, 0x10000ull, 0, 0x10000 ) );

    MachO::File file = parseFile( img.build() );
    std::vector< EntryPointSource > eps = file.entryPoints();

    /* Only records 0 and 2 of the LC_UNIXTHREAD command produce sources. */
    TASK_CHECK( eps.size() == 2 );

    TASK_CHECK( eps[ 0 ].commandIndex == 1 );
    TASK_CHECK( eps[ 0 ].recordIndex == std::optional< size_t >( 0 ) );
    TASK_CHECK( eps[ 0 ].coordinate == 0x100000100ull );

    TASK_CHECK( eps[ 1 ].commandIndex == 1 );
    TASK_CHECK( eps[ 1 ].recordIndex == std::optional< size_t >( 2 ) );
    TASK_CHECK( eps[ 1 ].coordinate == 0x100000200ull );

    for( const auto & e: eps )
    {
        TASK_CHECK( e.commandIndex != 0 ); /* never the LC_THREAD command */
    }

    return 0;
}

static int entrypoints_overlapping_segments( void )
{
    /* LC_MAIN offset covered by two overlapping file ranges. */
    {
        Image img;

        img.commands.push_back( mainCommand( 0x100 ) );
        img.commands.push_back( segment64Command( "__TEXT", 0x1000ull, 0x1000ull, 0, 0x1000 ) );
        img.commands.push_back( segment64Command( "__LINK", 0x2000ull, 0x2000ull, 0, 0x2000 ) );

        MachO::File file = parseFile( img.build() );
        std::vector< EntryPointSource > eps = file.entryPoints();

        TASK_CHECK( eps.size() == 1 );
        TASK_CHECK( eps[ 0 ].candidates.size() == 2 );
        TASK_CHECK( eps[ 0 ].candidates[ 0 ].segmentIndex == 1 );
        TASK_CHECK( eps[ 0 ].candidates[ 1 ].segmentIndex == 2 );
        TASK_CHECK( eps[ 0 ].candidates[ 0 ].address == 0x1100 );
        TASK_CHECK( eps[ 0 ].candidates[ 1 ].address == 0x2100 );
        TASK_CHECK( *eps[ 0 ].candidates[ 0 ].mappedFileOffset == 0x100 );
        TASK_CHECK( *eps[ 0 ].candidates[ 1 ].mappedFileOffset == 0x100 );
    }

    /* Same PC inside two segments at the same virtual address: keep both. */
    {
        Image img;

        img.commands.push_back( threadCommand( 0x05,
            threadRecord( 4, 42, x86_64State( 0x100000500ull, 0x7000, false ) ) ) );
        img.commands.push_back( segment64Command( "__A", 0x100000000ull, 0x1000ull, 0, 0x1000 ) );
        img.commands.push_back( segment64Command( "__B", 0x100000000ull, 0x2000ull, 0x4000, 0x2000 ) );

        MachO::File file = parseFile( img.build() );
        const auto & eps = file.entryPoints();

        TASK_CHECK( eps.size() == 1 );
        TASK_CHECK( eps[ 0 ].candidates.size() == 2 );
        TASK_CHECK( eps[ 0 ].candidates[ 0 ].segmentIndex == 1 );
        TASK_CHECK( eps[ 0 ].candidates[ 1 ].segmentIndex == 2 );
        TASK_CHECK( eps[ 0 ].candidates[ 0 ].address == 0x100000500ull );
        TASK_CHECK( eps[ 0 ].candidates[ 1 ].address == 0x100000500ull );
        TASK_CHECK( *eps[ 0 ].candidates[ 1 ].mappedFileOffset == 0x4500 );
    }

    return 0;
}

static int entrypoints_zero_fill_tail( void )
{
    Image img;

    /* vmsize 0x4000 but only 0x1000 file bytes. */
    img.commands.push_back( threadCommand( 0x05,
        threadRecord( 4, 42, x86_64State( 0x100002000ull, 0x7000, false ) ) ) );
    img.commands.push_back( segment64Command( "__TEXT", 0x100000000ull, 0x4000ull, 0, 0x1000 ) );

    MachO::File file = parseFile( img.build() );
    const auto & eps = file.entryPoints();

    TASK_CHECK( eps.size() == 1 );
    TASK_CHECK( eps[ 0 ].candidates.size() == 1 );
    TASK_CHECK( eps[ 0 ].candidates[ 0 ].address == 0x100002000ull );
    TASK_CHECK( eps[ 0 ].candidates[ 0 ].mappedFileOffset.has_value() == false );

    /* PC exactly at filesize displacement is also zero-fill, still mapped virtually. */
    {
        Image i2;
        i2.commands.push_back( threadCommand( 0x05,
            threadRecord( 4, 42, x86_64State( 0x100001000ull, 0x7000, false ) ) ) );
        i2.commands.push_back( segment64Command( "__TEXT", 0x100000000ull, 0x4000ull, 0x2000, 0x1000 ) );

        MachO::File f2 = parseFile( i2.build() );
        const auto & e2 = f2.entryPoints();

        TASK_CHECK( e2[ 0 ].candidates.size() == 1 );
        TASK_CHECK( e2[ 0 ].candidates[ 0 ].mappedFileOffset.has_value() == false );
    }

    return 0;
}

static int entrypoints_missing_segment_keeps_coordinate( void )
{
    Image img;

    img.commands.push_back( mainCommand( 0xCAFEu ) );
    img.commands.push_back( threadCommand( 0x05,
        threadRecord( 4, 42, x86_64State( 0x000000000BADF00Dull, 0x7000, false ) ) ) );

    MachO::File file = parseFile( img.build() );
    std::vector< EntryPointSource > eps = file.entryPoints();

    TASK_CHECK( eps.size() == 2 );

    TASK_CHECK( eps[ 0 ].kind == EntryPointKind::Main );
    TASK_CHECK( eps[ 0 ].commandIndex == 0 );
    TASK_CHECK( eps[ 0 ].coordinate == 0xCAFEu );
    TASK_CHECK( eps[ 0 ].candidates.empty() );

    TASK_CHECK( eps[ 1 ].kind == EntryPointKind::UnixThread );
    TASK_CHECK( eps[ 1 ].commandIndex == 1 );
    TASK_CHECK( eps[ 1 ].recordIndex == std::optional< size_t >( 0 ) );
    TASK_CHECK( eps[ 1 ].coordinate == 0x0BADF00Dull );
    TASK_CHECK( eps[ 1 ].candidates.empty() );

    return 0;
}

static int entrypoints_half_open_boundaries( void )
{
    /* MAIN: offset == fileoff is inside; offset == fileoff+filesize is out. */
    {
        Image img;

        img.commands.push_back( mainCommand( 0 ) );
        img.commands.push_back( mainCommand( 0x1000 ) );
        img.commands.push_back( segment64Command( "__TEXT", 0x100000000ull, 0x1000ull, 0, 0x1000 ) );

        MachO::File file = parseFile( img.build() );
        auto eps = file.entryPoints();

        TASK_CHECK( eps.size() == 2 );
        TASK_CHECK( eps[ 0 ].coordinate == 0 );
        TASK_CHECK( eps[ 0 ].candidates.size() == 1 );
        TASK_CHECK( eps[ 0 ].candidates[ 0 ].address == 0x100000000ull );

        TASK_CHECK( eps[ 1 ].coordinate == 0x1000 );
        TASK_CHECK( eps[ 1 ].candidates.empty() );
    }

    /* THREAD: PC == vmaddr inside; PC == vmaddr+vmsize outside. */
    {
        Bytes recs = threadRecord( 4, 42, x86_64State( 0x1000ull, 0x7000, false ) );
        Bytes second = threadRecord( 4, 42, x86_64State( 0x2000ull, 0x7000, false ) );

        recs.insert( recs.end(), second.begin(), second.end() );

        Image img;

        img.commands.push_back( threadCommand( 0x05, recs ) );
        img.commands.push_back( segment64Command( "__TEXT", 0x1000ull, 0x1000ull, 0, 0x1000 ) );

        MachO::File file = parseFile( img.build() );
        auto eps = file.entryPoints();

        TASK_CHECK( eps.size() == 2 );
        TASK_CHECK( eps[ 0 ].coordinate == 0x1000 );
        TASK_CHECK( eps[ 0 ].candidates.size() == 1 );
        TASK_CHECK( *eps[ 0 ].candidates[ 0 ].mappedFileOffset == 0 );

        TASK_CHECK( eps[ 1 ].coordinate == 0x2000 );
        TASK_CHECK( eps[ 1 ].candidates.empty() );
    }

    return 0;
}

static int entrypoints_segment32_and_file_base( void )
{
    Image img;

    img.m64 = false;
    img.cpu = 7;
    img.commands.push_back( mainCommand( 0x100 ) );
    img.commands.push_back( segment32Command( "__TEXT", 0x2000u, 0x1000u, 0, 0x1000u ) );

    MachO::File file = parseFile( img.build() );
    auto eps = file.entryPoints();

    TASK_CHECK( eps.size() == 1 );
    TASK_CHECK( eps[ 0 ].candidates.size() == 1 );
    TASK_CHECK( eps[ 0 ].candidates[ 0 ].segmentIndex == 1 );
    TASK_CHECK( eps[ 0 ].candidates[ 0 ].address == 0x2100 );
    TASK_CHECK( *eps[ 0 ].candidates[ 0 ].mappedFileOffset == 0x100 );

    return 0;
}

static Bytes makeFatImage( const Bytes & slice0, uint32_t cpu0,
                           const Bytes & slice1, uint32_t cpu1 )
{
    const uint32_t off0 = 0x1000;
    const uint32_t off1 = 0x4000;
    size_t         total = static_cast< size_t >( off1 ) + slice1.size();

    Bytes fat( total, 0 );
    Writer w( true ); /* fat header and arch entries are big endian */

    w.u32( 0xCAFEBABEu );
    w.u32( 2 );

    w.u32( cpu0 ); w.u32( 3 ); w.u32( off0 ); w.u32( static_cast< uint32_t >( slice0.size() ) ); w.u32( 12 );
    w.u32( cpu1 ); w.u32( 3 ); w.u32( off1 ); w.u32( static_cast< uint32_t >( slice1.size() ) ); w.u32( 12 );

    std::copy( w.b.begin(), w.b.end(), fat.begin() );
    std::copy( slice0.begin(), slice0.end(), fat.begin() + off0 );
    std::copy( slice1.begin(), slice1.end(), fat.begin() + off1 );

    return fat;
}

static int entrypoints_fat_slices_use_own_cpu_and_segments( void )
{
    /* Slice 0: little endian x86_64. */
    Image a;

    a.big = false;
    a.cpu = 0x01000007u;
    a.commands.push_back( threadCommand( 0x05,
        threadRecord( 4, 42, x86_64State( 0x100000810ull, 0x7000, false ) ) ) );
    a.commands.push_back( segment64Command( "__TEXT", 0x100000000ull, 0x2000ull, 0, 0x2000 ) );

    Bytes slice0 = a.build();

    /* Slice 1: big endian ARM64 with its own distinct segment table. */
    Image b;

    b.big = true;
    b.cpu = 0x0100000Cu;
    b.commands.push_back( threadCommand( 0x05,
        threadRecord( 6, 68, arm64State( 0x400001234ull, 0x800000ull, true ), true ), true ) );
    b.commands.push_back( segment64Command( "__TEXT", 0x400000000ull, 0x10000ull, 0x1000, 0x2000, true ) );

    Bytes slice1 = b.build();
    Bytes fat    = makeFatImage( slice0, 0x01000007u, slice1, 0x0100000Cu );

    MachO::FatFile ff    = parseFat( fat ); /* outer stream released */
    auto           archs = ff.architectures();

    TASK_CHECK( archs.size() == 2 );
    TASK_CHECK( archs[ 0 ].second.cpu().type() == 0x01000007u );
    TASK_CHECK( archs[ 1 ].second.cpu().type() == 0x0100000Cu );

    /* Slice 0 mapping. */
    {
        auto eps = archs[ 0 ].second.entryPoints();

        TASK_CHECK( eps.size() == 1 );
        TASK_CHECK( eps[ 0 ].coordinate == 0x100000810ull );
        TASK_CHECK( eps[ 0 ].candidates.size() == 1 );
        TASK_CHECK( eps[ 0 ].candidates[ 0 ].segmentIndex == 1 );
        TASK_CHECK( *eps[ 0 ].candidates[ 0 ].mappedFileOffset == 0x810 );
    }

    /* Slice 1 mapping uses its own CPU decode and its own file base 0x1000. */
    {
        auto threads = archs[ 1 ].second.loadCommands< MachO::LoadCommands::Thread >();

        TASK_CHECK( threads.size() == 1 );
        TASK_CHECK( threads[ 0 ].records()[ 0 ].context().has_value() );
        TASK_CHECK( threads[ 0 ].records()[ 0 ].context()->architecture() == ThreadArchitecture::ARM64 );
        TASK_CHECK( threads[ 0 ].records()[ 0 ].context()->pc() == 0x400001234ull );

        auto eps = archs[ 1 ].second.entryPoints();

        TASK_CHECK( eps.size() == 1 );
        TASK_CHECK( eps[ 0 ].coordinate == 0x400001234ull );
        TASK_CHECK( eps[ 0 ].candidates.size() == 1 );
        TASK_CHECK( eps[ 0 ].candidates[ 0 ].address == 0x400001234ull );
        TASK_CHECK( *eps[ 0 ].candidates[ 0 ].mappedFileOffset == 0x2234 ); /* 0x1000 + 0x1234 */
        TASK_CHECK( eps[ 0 ].candidates[ 0 ].segmentIndex == 1 );
    }

    /* FatFile copy stays readable. */
    MachO::FatFile copy = ff;

    TASK_CHECK( copy.architectures().size() == 2 );
    TASK_CHECK( copy.architectures()[ 0 ].second.entryPoints().size() == 1 );

    return 0;
}

/* ================================================================== */
/* Registration (names match the groups grepped by task_support/run.sh)*/
/* ================================================================== */

const struct task_case task_cases[] =
{
    { "records",     "fields-order-and-bytes",                  records_fields_order_and_bytes },
    { "records",     "legacy-constructor-records-only",         records_legacy_constructor_has_records_no_context },
    { "records",     "copy-and-stream-release",                 records_survive_file_copy_and_stream_release },
    { "records",     "invalid-ranges-and-lengths-throw",        records_invalid_ranges_and_lengths_throw },

    { "contexts",    "i386-registers",                          contexts_i386 },
    { "contexts",    "x86_64-registers",                        contexts_x86_64 },
    { "contexts",    "unified-and-unknown-coexist",             contexts_unified_and_unknown_coexist },
    { "contexts",    "same-flavor-one-two-cpus",                contexts_same_flavor_one_two_cpus },
    { "contexts",    "arm-and-arm64-layouts",                   contexts_arm_and_arm64_layouts },
    { "contexts",    "big-endian-decoding",                     contexts_big_endian },
    { "contexts",    "getinfo-shows-records-and-registers",     contexts_getinfo_shows_records_and_registers },

    { "entrypoints", "lc-main-file-offset-to-va",               entrypoints_lc_main_file_offset_to_va },
    { "entrypoints", "unixthread-pc-to-file-offset",            entrypoints_unixthread_pc_to_file },
    { "entrypoints", "lc-thread-is-not-an-entry",               entrypoints_lc_thread_not_an_entry },
    { "entrypoints", "overlapping-segments",                    entrypoints_overlapping_segments },
    { "entrypoints", "zero-fill-tail",                          entrypoints_zero_fill_tail },
    { "entrypoints", "missing-segment-keeps-coordinate",        entrypoints_missing_segment_keeps_coordinate },
    { "entrypoints", "half-open-boundaries",                    entrypoints_half_open_boundaries },
    { "entrypoints", "segment32-and-file-base",                 entrypoints_segment32_and_file_base },
    { "entrypoints", "fat-slices-own-cpu-and-segments",         entrypoints_fat_slices_use_own_cpu_and_segments },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[ 0 ] );
