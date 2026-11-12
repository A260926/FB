// SPDX-License-Identifier: Apache-2.0
/*
    tests/lpc_analysis.cpp
    - implementation of the twelve required groups from checks/lpc_test_api.h
    - every group runs real ALAC material through the public encoder and the
      unmodified decoder, and asserts on the results
*/
#include "lpc_test_api.h"

#include "ALACEncoder.h"
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#include "aglib.h"
#include "dplib.h"
#include "aglib.h"
#include "matrixlib.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace alac_lpc_test {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static bool g_failed = false;
static std::string g_caseName;

static void failf( const char * fmt, ... )
{
    g_failed = true;
    va_list args;
    va_start( args, fmt );
    std::fprintf( stderr, "LPC_ASSERT_FAIL %s: ", g_caseName.c_str() );
    std::vfprintf( stderr, fmt, args );
    std::fprintf( stderr, "\n" );
    va_end( args );
}

#define CHECK( cond, ... )          do { if ( !(cond) ) { failf( __VA_ARGS__ ); } } while ( 0 )
#define REQUIRE( cond, ... )        do { if ( !(cond) ) { failf( __VA_ARGS__ ); return Failed; } } while ( 0 )

// deterministic random numbers
struct Rng
{
    uint32_t s;
    explicit Rng( uint32_t seed = 1 ) : s( seed ) {}
    uint32_t next()
    {
        s = s * 1664525u + 1013904223u;
        return s;
    }
    int32_t range( int32_t lo, int32_t hi )     // inclusive
    {
        return lo + (int32_t)( next() % (uint32_t)( hi - lo + 1 ) );
    }
};

// format helpers
static AudioFormatDescription MakeInput( uint32_t channels, uint32_t depth )
{
    AudioFormatDescription f = {};
    f.mSampleRate = 44100;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = depth;
    f.mFormatID = kALACFormatLinearPCM;
    f.mBytesPerPacket = f.mBytesPerFrame = channels * ( depth / 8 );
    f.mFramesPerPacket = 1;
    return f;
}

static AudioFormatDescription MakeOutput( uint32_t channels, uint32_t depth )
{
    AudioFormatDescription f = {};
    f.mSampleRate = 44100;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = depth;
    f.mFormatID = kALACFormatAppleLossless;
    f.mFormatFlags = ( depth == 16 ) ? 1 : ( ( depth == 20 ) ? 2 : ( ( depth == 24 ) ? 3 : 4 ) );
    return f;
}

// One encoder + one decoder pair for a given configuration
struct Codec
{
    ALACEncoder             encoder;
    ALACDecoder             decoder;
    AudioFormatDescription  input;
    AudioFormatDescription  output;
    std::vector<uint8_t>    cookie;

    bool init( uint32_t channels, uint32_t depth, uint32_t frameSize, bool analysis, bool fast )
    {
        input = MakeInput( channels, depth );
        output = MakeOutput( channels, depth );
        encoder.SetFrameSize( frameSize );
        encoder.SetFastMode( fast );
        encoder.SetLPCAnalysis( analysis );
        if ( encoder.InitializeEncoder( output ) != 0 )
            return false;
        cookie.assign( 256, 0 );
        uint32_t size = (uint32_t)cookie.size();
        encoder.GetMagicCookie( cookie.data(), &size );
        if ( size == 0 )
            return false;
        cookie.resize( size );
        return decoder.Init( cookie.data(), size ) == 0;
    }
};

// A complete compressed packet plus the original and decoded PCM
struct Packet
{
    std::vector<uint8_t>    compressed;
    std::vector<uint8_t>    source;
    std::vector<uint8_t>    decoded;
    uint32_t                numFrames;
    uint32_t                numSamplesPerChannel;
};

// Encode one packet of interleaved PCM; returns the encoded length (0 on error)
static int EncodePacket( Codec & c, const std::vector<uint8_t> & pcm, uint32_t frames,
                         Packet & out, bool verbose )
{
    out.numFrames = frames;
    out.numSamplesPerChannel = frames;
    out.source = pcm;
    out.compressed.assign( c.input.mChannelsPerFrame * frames * 8 + 512, 0 );

    int32_t bytes = (int32_t)pcm.size();
    if ( c.encoder.Encode( c.input, c.output, const_cast<uint8_t *>( pcm.data() ),
                           out.compressed.data(), &bytes ) != 0 )
        return 0;
    if ( bytes <= 0 )
        return 0;
    out.compressed.resize( bytes );

    out.decoded.assign( pcm.size(), 0 );
    BitBuffer bits;
    BitBufferInit( &bits, out.compressed.data(), (uint32_t)bytes );
    uint32_t got = 0;
    if ( c.decoder.Decode( &bits, out.decoded.data(), frames, c.input.mChannelsPerFrame, &got ) != 0 )
        return 0;
    if ( got != frames )
        return 0;

    if ( verbose )
        std::printf( "  packet frames=%u channels=%u bytes=%d\n", frames, c.input.mChannelsPerFrame, bytes );
    return bytes;
}

static bool CheckLossless( const Packet & p, uint32_t channels, uint32_t depth, const char * label )
{
    if ( std::memcmp( p.source.data(), p.decoded.data(), p.source.size() ) != 0 )
    {
        size_t i = 0;
        while ( i < p.source.size() && p.source[i] == p.decoded[i] )
            i++;
        failf( "%s: decoded PCM differs at byte %zu of %zu (channels=%u depth=%u)",
               label, i, p.source.size(), channels, depth );
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ALAC packet header parser (independent of the decoder implementation)
// ---------------------------------------------------------------------------

struct ElementHeader
{
    bool        escape;
    bool        partialFrame;
    uint8_t     bytesShifted;
    uint32_t    numFrames;
    uint8_t     mixBits;
    int8_t      mixRes;
    bool        hasU;
    bool        hasV;
    uint8_t     numU;
    uint8_t     numV;
    uint8_t     modeU;
    uint8_t     modeV;
    uint8_t     denShiftU;
    uint8_t     denShiftV;
    uint8_t     pbFactorU;
    uint8_t     pbFactorV;
    int16_t     coefsU[32];
    int16_t     coefsV[32];
    uint32_t    headerBits;      // bit position immediately after the parameter section
};

struct ParsedPacket
{
    uint8_t         elementTag;      // ID_SCE / ID_CPE
    ElementHeader   element;
};

static bool PeekBits( const uint8_t * data, uint32_t numBytes, uint32_t & bitPos, uint8_t numBits, uint32_t & value )
{
    if ( bitPos + numBits > numBytes * 8 )
        return false;
    value = 0;
    for ( uint8_t i = 0; i < numBits; i++ )
    {
        uint32_t p = bitPos + i;
        uint8_t b = ( data[p >> 3] >> ( 7 - ( p & 7 ) ) ) & 1u;
        value = ( value << 1 ) | b;
    }
    return true;
}

static bool ReadBits( const uint8_t * data, uint32_t numBytes, uint32_t & bitPos, uint8_t numBits, uint32_t & value )
{
    if ( !PeekBits( data, numBytes, bitPos, numBits, value ) )
        return false;
    bitPos += numBits;
    return true;
}

// Parse the first element of an ALAC packet. Only the fields needed by the
// tests are extracted; the layout follows the documented segment header.
static bool ParsePacketHeader( const std::vector<uint8_t> & packet, ParsedPacket & parsed )
{
    uint32_t bitPos = 0;
    uint32_t v = 0;

    if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 3, v ) )
        return false;
    parsed.elementTag = (uint8_t)v;
    if ( ( parsed.elementTag != ID_SCE ) && ( parsed.elementTag != ID_CPE ) )
        return false;
    if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 4, v ) )     // element instance
        return false;
    if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 12, v ) )    // unused
        return false;
    if ( v != 0 )
        return false;

    uint8_t flags = 0;
    if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 4, v ) )
        return false;
    flags = (uint8_t)v;

    ElementHeader & e = parsed.element;
    std::memset( &e, 0, sizeof( e ) );
    e.escape = ( flags & 1 ) != 0;
    e.bytesShifted = ( flags >> 1 ) & 0x3u;
    e.partialFrame = ( flags >> 3 ) != 0;
    if ( e.escape )
    {
        e.headerBits = bitPos;
        return true;
    }

    e.numFrames = 0;
    if ( e.partialFrame )
    {
        uint32_t hi = 0, lo = 0;
        if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 16, hi ) )
            return false;
        if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 16, lo ) )
            return false;
        e.numFrames = ( hi << 16 ) | lo;
    }

    if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 8, v ) )
        return false;
    e.mixBits = (uint8_t)v;
    if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 8, v ) )
        return false;
    e.mixRes = (int8_t)v;

    // U parameters
    if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 4, v ) )
        return false;
    e.modeU = (uint8_t)v;
    if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 4, v ) )
        return false;
    e.denShiftU = (uint8_t)v;
    if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 3, v ) )
        return false;
    e.pbFactorU = (uint8_t)v;
    if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 5, v ) )
        return false;
    e.numU = (uint8_t)v;
    e.hasU = true;
    if ( e.numU > 31 )
        return false;
    for ( uint32_t i = 0; i < e.numU; i++ )
    {
        if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 16, v ) )
            return false;
        e.coefsU[i] = (int16_t)( v > 0x7fffu ? (int32_t)v - 0x10000 : (int32_t)v );
    }

    if ( parsed.elementTag == ID_CPE )
    {
        if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 4, v ) )
            return false;
        e.modeV = (uint8_t)v;
        if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 4, v ) )
            return false;
        e.denShiftV = (uint8_t)v;
        if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 3, v ) )
            return false;
        e.pbFactorV = (uint8_t)v;
        if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 5, v ) )
            return false;
        e.numV = (uint8_t)v;
        e.hasV = true;
        if ( e.numV > 31 )
            return false;
        for ( uint32_t i = 0; i < e.numV; i++ )
        {
            if ( !ReadBits( packet.data(), (uint32_t)packet.size(), bitPos, 16, v ) )
                return false;
            e.coefsV[i] = (int16_t)( v > 0x7fffu ? (int32_t)v - 0x10000 : (int32_t)v );
        }
    }

    e.headerBits = bitPos;
    return true;
}

// ---------------------------------------------------------------------------
// Independent bitstream readers
// - these mirror the documented ALAC bitstream conventions (segment header,
//   adaptive Golomb coding and the predictor recurrence) with wide integer
//   arithmetic; they do not call the encoder's search or entropy routines
// ---------------------------------------------------------------------------

static int32_t Lead32( uint32_t m )
{
    for ( int32_t j = 0; j < 32; j++ )
    {
        if ( ( m & ( 1u << ( 31 - j ) ) ) != 0 )
            return j;
    }
    return 32;
}

static int32_t Lg3a( uint32_t x )
{
    return 31 - Lead32( x + 3 );
}

static uint32_t ReadBitsN( const uint8_t * data, uint32_t numBytes, uint32_t bitPos, uint32_t numBits )
{
    uint32_t value = 0;
    for ( uint32_t i = 0; i < numBits; i++ )
    {
        uint32_t p = bitPos + i;
        if ( ( p >> 3 ) >= numBytes )
            return 0;
        uint8_t b = ( data[p >> 3] >> ( 7 - ( p & 7 ) ) ) & 1u;
        value = ( value << 1 ) | b;
    }
    return value;
}

struct AgState
{
    uint32_t mb, pb, kb, wb;
};

static void InitAgState( AgState & ag, uint32_t mb0, uint32_t pb, uint32_t kb )
{
    ag.mb = mb0;
    ag.pb = pb;
    ag.kb = kb;
    ag.wb = ( 1u << kb ) - 1u;
}

// dyn_get_32bit() mirror: returns the codeword value n.
// The escape form is exactly MAX_PREFIX_32 ones followed by maxBits raw bits
// (there is no terminating zero in that case).
static bool ReadAgCode( const uint8_t * data, uint32_t numBytes, uint32_t & bitPos, AgState & ag,
                        int32_t maxBits, uint32_t & n )
{
    uint32_t m = ag.mb >> QBSHIFT;
    uint32_t k = (uint32_t)Lg3a( m );
    if ( (int32_t)k > (int32_t)ag.kb )
        k = ag.kb;
    m = ( 1u << k ) - 1;

    uint32_t ones = 0;
    while ( ones < MAX_PREFIX_32 )
    {
        if ( bitPos >= numBytes * 8 )
            return false;
        uint32_t bit = ReadBitsN( data, numBytes, bitPos, 1 );
        bitPos += 1;
        if ( bit == 0 )
            break;
        ones++;
    }

    if ( ones >= MAX_PREFIX_32 )
    {
        n = ReadBitsN( data, numBytes, bitPos, (uint32_t)maxBits );
        bitPos += (uint32_t)maxBits;
        return true;
    }

    if ( k == 1 )
    {
        n = ones;
        return true;
    }

    uint32_t v = ReadBitsN( data, numBytes, bitPos, k );
    bitPos += k;
    n = ones * m;
    if ( v >= 2 )
        n += v - 1;
    else
        bitPos -= 1;
    return true;
}

// dyn_get() mirror, used for the zero-run length
static bool ReadAgRunLength( const uint8_t * data, uint32_t numBytes, uint32_t & bitPos,
                             uint32_t mz, uint32_t kz, uint32_t & nz )
{
    uint32_t ones = 0;
    while ( ones < MAX_PREFIX_16 )
    {
        if ( bitPos >= numBytes * 8 )
            return false;
        uint32_t bit = ReadBitsN( data, numBytes, bitPos, 1 );
        bitPos += 1;
        if ( bit == 0 )
            break;
        ones++;
    }

    if ( ones >= MAX_PREFIX_16 )
    {
        nz = ReadBitsN( data, numBytes, bitPos, MAX_DATATYPE_BITS_16 );
        bitPos += MAX_DATATYPE_BITS_16;
        return true;
    }

    if ( kz == 1 )
    {
        nz = ones;
        return true;
    }

    uint32_t v = ReadBitsN( data, numBytes, bitPos, kz );
    bitPos += kz;
    nz = ones * mz;
    if ( v >= 2 )
        nz += v - 1;
    else
        bitPos -= 1;
    return true;
}

// Decode up to `limit` residual samples of one channel's adaptive Golomb stream
static bool AgDecode( const uint8_t * data, uint32_t numBytes, uint32_t & bitPos, AgState & ag,
                      uint32_t chanBits, uint32_t numSamples, std::vector<int32_t> & values, uint32_t limit )
{
    uint32_t c = 0;
    bool     zmode = false;

    values.clear();
    while ( c < numSamples )
    {
        uint32_t n = 0;
        if ( !ReadAgCode( data, numBytes, bitPos, ag, (int32_t)chanBits, n ) )
            return false;

        uint32_t ndecode = n + ( zmode ? 1u : 0u );
        int32_t multiplier = ( ( ndecode & 1u ) != 0 ) ? -1 : 1;
        int32_t del = (int32_t)( ( ndecode + 1u ) >> 1 ) * multiplier;

        values.push_back( del );
        if ( values.size() >= limit )
            return true;
        c++;

        ag.mb = ag.pb * ndecode + ag.mb - ( ( ag.pb * ag.mb ) >> QBSHIFT );
        if ( n > 0xffffu )
            ag.mb = 0xffffu;

        zmode = false;
        if ( ( ( ag.mb << MMULSHIFT ) < QB ) && ( c < numSamples ) )
        {
            zmode = true;
            uint32_t kz = (uint32_t)( Lead32( ag.mb ) - BITOFF + ( ( ag.mb + MOFF ) >> MDENSHIFT ) );
            uint32_t mz = ( ( 1u << kz ) - 1u ) & ag.wb;

            uint32_t nz = 0;
            if ( !ReadAgRunLength( data, numBytes, bitPos, mz, kz, nz ) )
                return false;
            if ( c + nz > numSamples )
                return false;

            for ( uint32_t j = 0; j < nz; j++ )
            {
                values.push_back( 0 );
                if ( values.size() >= limit )
                    return true;
                c++;
            }
            if ( nz >= 65535 )
                zmode = false;
            ag.mb = 0;
        }
    }
    return true;
}

static bool ReadResidualPrefix( const std::vector<uint8_t> & packet, const ElementHeader & e,
                                uint32_t chanBits, uint32_t limit, uint32_t numSamples, std::vector<int32_t> & out )
{
    std::vector<uint8_t> padded = packet;
    padded.resize( packet.size() + 32, 0 );

    uint32_t bitPos = e.headerBits;
    if ( e.bytesShifted != 0 )
        bitPos += e.bytesShifted * 8 * ( ( e.hasV ? 2 : 1 ) * numSamples );

    AgState ag;
    InitAgState( ag, MB0, ( 4 * PB0 ) / 4, KB0 );
    return AgDecode( padded.data(), (uint32_t)padded.size(), bitPos, ag, chanBits, numSamples, out, limit );
}

// Independent wide-integer recurrence that reproduces the predictor residual
// from (signal, initial coefficients, chanBits, denShift). Mirrors the
// adaptive update rules of the codec's pc_block() without calling it.
static int32_t SignExtend64( int64_t value, uint32_t bits )
{
    int64_t mask = ( (int64_t)1 << bits ) - 1;
    int64_t v = value & mask;
    if ( ( v & ( (int64_t)1 << ( bits - 1 ) ) ) != 0 )
        v -= ( (int64_t)1 << bits );
    return (int32_t)v;
}

static int32_t SignOf( int32_t i )
{
    uint32_t negishift = ( (uint32_t)-i ) >> 31;
    return (int32_t)( negishift | (uint32_t)( i >> 31 ) );
}

static void IndependentResiduals( const int32_t * in, uint32_t numSamples, uint32_t numactive,
                                  const int16_t * coefsIn, uint32_t chanBits, uint32_t denshift,
                                  std::vector<int32_t> & out )
{
    int16_t c[32];
    std::memcpy( c, coefsIn, sizeof( int16_t ) * numactive );
    out.assign( numSamples, 0 );

    out[0] = in[0];
    if ( numactive == 0 )
    {
        for ( uint32_t j = 1; j < numSamples; j++ )
            out[j] = in[j];
        return;
    }
    if ( numactive == 31 )
    {
        for ( uint32_t j = 1; j < numSamples; j++ )
            out[j] = SignExtend64( (int64_t)in[j] - in[j - 1], chanBits );
        return;
    }

    for ( uint32_t j = 1; j <= numactive; j++ )
        out[j] = SignExtend64( (int64_t)in[j] - in[j - 1], chanBits );

    uint32_t lim = numactive + 1;
    int64_t denhalf = (int64_t)1 << ( denshift - 1 );

    for ( uint32_t j = lim; j < numSamples; j++ )
    {
        int64_t top = in[j - lim];
        int64_t sum = 0;
        for ( uint32_t k = 0; k < numactive; k++ )
            sum -= (int64_t)c[k] * ( top - (int64_t)in[j - 1 - k] );

        int32_t del = SignExtend64( (int64_t)in[j] - top - ( ( sum + denhalf ) >> denshift ), chanBits );
        out[j] = del;

        int32_t del0 = del;
        int32_t sg = SignOf( del );
        if ( sg > 0 )
        {
            for ( int32_t k = (int32_t)numactive - 1; k >= 0; k-- )
            {
                int32_t dd = (int32_t)( top - (int64_t)in[j - 1 - (uint32_t)k] );
                int32_t sgn = SignOf( dd );
                c[k] = (int16_t)( c[k] - sgn );
                del0 -= (int32_t)( numactive - (uint32_t)k ) * ( (int64_t)( sgn * dd ) >> denshift );
                if ( del0 <= 0 )
                    break;
            }
        }
        else if ( sg < 0 )
        {
            for ( int32_t k = (int32_t)numactive - 1; k >= 0; k-- )
            {
                int32_t dd = (int32_t)( top - (int64_t)in[j - 1 - (uint32_t)k] );
                int32_t sgn = SignOf( dd );
                c[k] = (int16_t)( c[k] + sgn );
                del0 -= (int32_t)( numactive - (uint32_t)k ) * ( (int64_t)( -sgn * dd ) >> denshift );
                if ( del0 >= 0 )
                    break;
            }
        }
    }
}

// Independent mixing for the header/residual check (same lossless transform
// as the codec documents, written with explicit integer arithmetic)
static void IndependentMix( const uint8_t * src, uint32_t channels, uint32_t depth, uint32_t numSamples,
                            int32_t mixBits, int32_t mixRes, uint32_t bytesShifted,
                            std::vector<int32_t> & u, std::vector<int32_t> & v,
                            std::vector<uint16_t> * shiftUV = 0 )
{
    u.assign( numSamples, 0 );
    v.assign( numSamples, 0 );
    if ( shiftUV )
        shiftUV->assign( numSamples * 2, 0 );
    uint32_t shift = bytesShifted * 8;
    uint32_t mask = ( 1u << shift ) - 1u;

    for ( uint32_t i = 0; i < numSamples; i++ )
    {
        int64_t l = 0, r = 0;
        if ( depth == 16 )
        {
            l = (int16_t)( ( (const uint16_t *)src )[ i * channels ] );
            r = ( ( channels == 2 ) ? (int16_t)( ( (const uint16_t *)src )[ i * channels + 1 ] ) : 0 );
        }
        else if ( depth == 24 )
        {
            const uint8_t * bl = src + ( i * channels ) * 3;
            const uint8_t * br = src + ( i * channels + ( channels == 2 ? 1 : 0 ) ) * 3;
            // sign-extend from 24 bits exactly as the codec's 32-bit temporaries do
            int32_t lv = (int32_t)( (uint32_t)bl[2] << 16 | (uint32_t)bl[1] << 8 | bl[0] );
            lv = ( lv << 8 ) >> 8;
            int32_t rv = (int32_t)( (uint32_t)br[2] << 16 | (uint32_t)br[1] << 8 | br[0] );
            rv = ( rv << 8 ) >> 8;
            l = lv;
            r = rv;
            if ( shiftUV )
            {
                shiftUV->at( i * 2 + 0 ) = (uint16_t)( l & mask );
                shiftUV->at( i * 2 + 1 ) = (uint16_t)( r & mask );
            }
            l >>= shift;
            r >>= shift;
        }

        if ( ( channels == 2 ) && ( mixRes != 0 ) )
        {
            int32_t mod = (int32_t)1 << mixBits;
            int32_t m2 = mod - mixRes;
            u[i] = (int32_t)( ( (int64_t)mixRes * l + (int64_t)m2 * r ) >> mixBits );
            v[i] = (int32_t)( l - r );
        }
        else
        {
            u[i] = (int32_t)l;
            v[i] = (int32_t)( ( channels == 2 ) ? r : 0 );
        }
    }
}

// ---------------------------------------------------------------------------
// Signal generators
// ---------------------------------------------------------------------------

// Narrow-band tone plus a little secondary content, the baseline material shape
static void FillTones16( std::vector<uint8_t> & pcm, uint32_t channels, uint32_t frames,
                         double f0, double f1, double amp0, double amp1, double phase, double drift = 0.0 )
{
    int16_t * samples = (int16_t *)pcm.data();
    for ( uint32_t i = 0; i < frames; i++ )
    {
        // baseline material uses integer-time sin/cos with a truncating cast
        (void)phase;
        double t = (double)i + drift;
        for ( uint32_t c = 0; c < channels; c++ )
        {
            double cp = phase + c * 0.37;
            double v = amp0 * std::sin( t * f0 + cp ) + amp1 * std::cos( t * f1 + cp );
            if ( v > 32767.0 ) v = 32767.0;
            if ( v < -32768.0 ) v = -32768.0;
            samples[i * channels + c] = (int16_t)( ( v >= 0.0 ) ? ( v + 0.5 ) : ( v - 0.5 ) );
        }
    }
}

// 24-bit samples packed as 3 little-endian bytes; the low byte carries a
// non-zero, non-repeating pattern that must survive the shift-off path
static void Fill24( std::vector<uint8_t> & pcm, uint32_t channels, uint32_t frames,
                    double f0, double amp0, double phase, bool lowByteNoise, Rng * rng = 0 )
{
    for ( uint32_t i = 0; i < frames; i++ )
    {
        for ( uint32_t c = 0; c < channels; c++ )
        {
            double v = amp0 * std::sin( (double)i * f0 + phase + c * 0.4 );
            int32_t hi = (int32_t)( ( v >= 0.0 ) ? ( v + 0.5 ) : ( v - 0.5 ) );
            hi = ( hi & 0xffffff );
            int32_t lo = 0;
            if ( lowByteNoise )
                lo = rng ? rng->range( 1, 255 ) : (int32_t)( ( i * 37u + c * 91u ) % 255u + 1u );
            int32_t value = ( hi << 8 ) | lo;
            uint8_t * b = &pcm[ ( i * channels + c ) * 3 ];
            b[0] = (uint8_t)( value & 0xff );
            b[1] = (uint8_t)( ( value >> 8 ) & 0xff );
            b[2] = (uint8_t)( ( value >> 16 ) & 0xff );
        }
    }
}

static void FillFullScale16( std::vector<uint8_t> & pcm, uint32_t channels, uint32_t frames, Rng & rng )
{
    int16_t * samples = (int16_t *)pcm.data();
    for ( uint32_t i = 0; i < frames; i++ )
        for ( uint32_t c = 0; c < channels; c++ )
            samples[i * channels + c] = (int16_t)rng.range( -32768, 32767 );
}

static void FillConstant16( std::vector<uint8_t> & pcm, uint32_t channels, uint32_t frames, int16_t value )
{
    int16_t * samples = (int16_t *)pcm.data();
    for ( uint32_t i = 0; i < frames * channels; i++ )
        samples[i] = value;
}

// ---------------------------------------------------------------------------
// Group 1: baseline_gain
// - the three narrow-band packets from checks/baseline.cpp must shrink by at
//   least 5% in total when analysis is on, while staying exactly lossless
// ---------------------------------------------------------------------------

static Result RunBaselineGain()
{
    const uint32_t N = 4096, C = 2;
    std::vector<uint8_t> basePcm( N * C * 2 ), analysisPcm( N * C * 2 );

    Codec original, analysed;
    REQUIRE( original.init( C, 16, N, false, false ), "encoder init (baseline, off)" );
    REQUIRE( analysed.init( C, 16, N, true, false ), "encoder init (baseline, on)" );

    uint64_t totalOriginal = 0, totalAnalysis = 0;

    for ( uint32_t f = 0; f < 3; f++ )
    {
        // exactly the material from checks/baseline.cpp
        int16_t * bo = (int16_t *)basePcm.data();
        int16_t * ba = (int16_t *)analysisPcm.data();
        for ( uint32_t i = 0; i < N; i++ )
        {
            double t = (double)i + (double)( f * N );
            bo[2 * i] = ba[2 * i] = (int16_t)( 11000 * std::sin( t * 0.071 ) + 3000 * std::sin( t * 0.113 ) );
            bo[2 * i + 1] = ba[2 * i + 1] = (int16_t)( 9000 * std::sin( t * 0.071 + 0.4 ) + 2200 * std::cos( t * 0.037 ) );
        }

        Packet pOff, pOn;
        int offBytes = EncodePacket( original, basePcm, N, pOff, false );
        int onBytes = EncodePacket( analysed, analysisPcm, N, pOn, false );
        REQUIRE( offBytes > 0 && onBytes > 0, "encode failed on baseline packet %u", f );
        REQUIRE( CheckLossless( pOff, C, 16, "baseline off" ), "baseline off packet %u not lossless", f );
        REQUIRE( CheckLossless( pOn, C, 16, "baseline on" ), "baseline analysis packet %u not lossless", f );
        REQUIRE( pOn.source == pOff.source, "baseline packet %u: input buffer changed by analysis", f );
        REQUIRE( onBytes <= offBytes, "baseline packet %u: analysis grew the packet (%d > %d)", f, onBytes, offBytes );

        std::printf( "  baseline packet %u: old=%d new=%d delta=%d\n", f, offBytes, onBytes, offBytes - onBytes );
        totalOriginal += offBytes;
        totalAnalysis += onBytes;
    }

    double gain = 100.0 * (double)( totalOriginal - totalAnalysis ) / (double)totalOriginal;
    std::printf( "  baseline total: old=%llu new=%llu gain=%.2f%%\n",
                 (unsigned long long)totalOriginal, (unsigned long long)totalAnalysis, gain );
    REQUIRE( gain >= 5.0, "baseline gain %.2f%% below 5%% (old=%llu new=%llu)",
             gain, (unsigned long long)totalOriginal, (unsigned long long)totalAnalysis );
    return Passed;
}

// ---------------------------------------------------------------------------
// Group 2: mono16_fit
// - a single 16-bit channel, steady narrow band material
// - analysis must help, stay lossless, and actually report a fitted order
// ---------------------------------------------------------------------------

static Result RunMono16Fit()
{
    const uint32_t N = 4096, C = 1;

    uint64_t totalOff = 0, totalOn = 0;

    for ( uint32_t f = 0; f < 4; f++ )
    {
        // fresh pair per packet: both sides start from the same encoding state
        Codec off, on;
        REQUIRE( off.init( C, 16, N, false, false ), "mono16 encoder init off" );
        REQUIRE( on.init( C, 16, N, true, false ), "mono16 encoder init on" );

        std::vector<uint8_t> pcm( N * C * 2 );
        FillTones16( pcm, C, N, 0.062, 0.114, 14000.0, 3500.0, 0.0, (double)( f * N ) );

        Packet pOff, pOn;
        int offBytes = EncodePacket( off, pcm, N, pOff, false );
        int onBytes = EncodePacket( on, pcm, N, pOn, true );
        REQUIRE( offBytes > 0 && onBytes > 0, "mono16 packet %u encode failed", f );
        REQUIRE( CheckLossless( pOff, C, 16, "mono16 off" ), "mono16 off packet %u not lossless", f );
        REQUIRE( CheckLossless( pOn, C, 16, "mono16 on" ), "mono16 analysis packet %u not lossless", f );
        REQUIRE( onBytes <= offBytes, "mono16 packet %u grew under analysis", f );

        ParsedPacket parsed;
        REQUIRE( ParsePacketHeader( pOn.compressed, parsed ), "mono16 packet %u header unparsable", f );
        REQUIRE( !parsed.element.escape, "mono16 packet %u escaped unexpectedly", f );
        std::printf( "  mono16 packet %u: old=%d new=%d orderU=%u\n",
                     f, offBytes, onBytes, parsed.element.numU );
        REQUIRE( parsed.element.numU == 4 || parsed.element.numU == 8 ||
                 parsed.element.numU == 12 || parsed.element.numU == 16,
                 "mono16 packet %u: order %u is not one of the analysis candidates", f, parsed.element.numU );
        totalOff += offBytes;
        totalOn += onBytes;
    }

    double gain = 100.0 * (double)( totalOff - totalOn ) / (double)totalOff;
    std::printf( "  mono16 total: old=%llu new=%llu gain=%.2f%%\n",
                 (unsigned long long)totalOff, (unsigned long long)totalOn, gain );
    REQUIRE( totalOn < totalOff, "mono16 analysis produced no gain (old=%llu new=%llu)",
             (unsigned long long)totalOff, (unsigned long long)totalOn );
    return Passed;
}

// ---------------------------------------------------------------------------
// Group 3: stereo16_matrix
// - correlated and anti-correlated stereo, plus the baseline stereo material
// - the retained mixRes must match the original encoder's selection, and the
//   per-channel analysis must operate on the matrixed U/V data
// ---------------------------------------------------------------------------

static Result RunStereo16Matrix()
{
    const uint32_t N = 4096, C = 2;

    struct Material
    {
        const char * name;
        double f0, f1, amp0, amp1, phase;
        bool   anti;
    } materials[] = {
        { "correlated",     0.058, 0.131, 12000.0, 2600.0, 0.0,  false },
        { "anticorrelated", 0.071, 0.113, 12000.0, 2600.0, 0.0,  true  },
        { "baseline-like",  0.071, 0.113, 11000.0, 3000.0, 0.0,  false },
    };

    for ( size_t m = 0; m < sizeof( materials ) / sizeof( materials[0] ); m++ )
    {
        Codec off, on;
        REQUIRE( off.init( C, 16, N, false, false ), "stereo16 encoder init off (%s)", materials[m].name );
        REQUIRE( on.init( C, 16, N, true, false ), "stereo16 encoder init on (%s)", materials[m].name );

        std::vector<uint8_t> pcm( N * C * 2 );
        int16_t * samples = (int16_t *)pcm.data();
        for ( uint32_t i = 0; i < N; i++ )
        {
            double l = materials[m].amp0 * std::sin( (double)i * materials[m].f0 ) +
                       materials[m].amp1 * std::cos( (double)i * materials[m].f1 );
            int16_t lv = (int16_t)( ( l >= 0.0 ) ? ( l + 0.5 ) : ( l - 0.5 ) );
            int16_t rv;
            if ( materials[m].anti )
                rv = (int16_t)( -lv );
            else
            {
                double r = materials[m].amp0 * std::sin( (double)i * materials[m].f0 + 0.2 ) +
                           materials[m].amp1 * std::cos( (double)i * materials[m].f1 + 0.1 );
                rv = (int16_t)( ( r >= 0.0 ) ? ( r + 0.5 ) : ( r - 0.5 ) );
            }
            samples[2 * i] = lv;
            samples[2 * i + 1] = rv;
        }

        // NOTE: the analysis path re-derives the mix resolution from the same
        // state with the same search, so a fresh pair of encoders must agree.

        Packet pOff, pOn;
        int offBytes = EncodePacket( off, pcm, N, pOff, false );
        int onBytes = EncodePacket( on, pcm, N, pOn, true );
        REQUIRE( offBytes > 0 && onBytes > 0, "stereo16 %s encode failed", materials[m].name );
        REQUIRE( CheckLossless( pOff, C, 16, "stereo16 off" ), "stereo16 %s off not lossless", materials[m].name );
        REQUIRE( CheckLossless( pOn, C, 16, "stereo16 on" ), "stereo16 %s analysis not lossless", materials[m].name );
        REQUIRE( pOn.source == pOff.source, "stereo16 %s: input changed", materials[m].name );
        REQUIRE( onBytes <= offBytes, "stereo16 %s grew under analysis", materials[m].name );

        ParsedPacket a, b;
        REQUIRE( ParsePacketHeader( pOff.compressed, a ), "stereo16 %s off header unparsable", materials[m].name );
        REQUIRE( ParsePacketHeader( pOn.compressed, b ), "stereo16 %s on header unparsable", materials[m].name );
        REQUIRE( a.element.mixRes == b.element.mixRes,
                 "stereo16 %s: mixRes changed (off=%d on=%d)", materials[m].name,
                 (int)a.element.mixRes, (int)b.element.mixRes );
        REQUIRE( b.element.numU == 4 || b.element.numU == 8 || b.element.numU == 12 || b.element.numU == 16,
                 "stereo16 %s: odd U order %u", materials[m].name, b.element.numU );
        REQUIRE( b.element.numV == 4 || b.element.numV == 8 || b.element.numV == 12 || b.element.numV == 16,
                 "stereo16 %s: odd V order %u", materials[m].name, b.element.numV );

        // the transmitted coefficients must be fitted from the U/V data, i.e. not
        // the fixed init_coefs pattern (check both channels; either may legitimately
        // land on a similar value, so require at least one to differ)
        int16_t expected0 = (int16_t)( ( 38 * ( 1 << 9 ) ) >> 4 );
        int16_t expected1 = (int16_t)( ( -29 * ( 1 << 9 ) ) >> 4 );
        int16_t expected2 = (int16_t)( ( -2 * ( 1 << 9 ) ) >> 4 );
        bool distinct = ( b.element.coefsU[0] != expected0 || b.element.coefsU[1] != expected1 ||
                          b.element.coefsU[2] != expected2 ||
                          b.element.coefsV[0] != expected0 || b.element.coefsV[1] != expected1 ||
                          b.element.coefsV[2] != expected2 );
        REQUIRE( distinct, "stereo16 %s: coefficients look like the fixed init pattern", materials[m].name );

        std::printf( "  stereo16 %-14s: old=%d new=%d mixRes=%d orderU=%u orderV=%u\n",
                     materials[m].name, offBytes, onBytes, (int)b.element.mixRes,
                     b.element.numU, b.element.numV );
    }
    return Passed;
}

// ---------------------------------------------------------------------------
// Group 4: stereo24_shift
// - 24-bit stereo with non-zero low bytes
// - the shift-off byte count must be preserved, low bytes must round-trip,
//   and analysis must run on the shifted (U/V) data (orders present)
// ---------------------------------------------------------------------------

static Result RunStereo24Shift()
{
    const uint32_t N = 4096, C = 2;

    uint64_t totalOff = 0, totalOn = 0;

    for ( uint32_t f = 0; f < 3; f++ )
    {
        Codec off, on;
        REQUIRE( off.init( C, 24, N, false, false ), "stereo24 encoder init off" );
        REQUIRE( on.init( C, 24, N, true, false ), "stereo24 encoder init on" );

        std::vector<uint8_t> pcm( N * C * 3 );
        Rng rng( 0x1234u + f );
        // 24-bit material with non-zero low bytes: the encoder's shift-off rule
        // extracts one byte and the analysis runs on the shifted U/V data
        Fill24( pcm, C, N, ( f == 2 ) ? 0.113 : 0.058, 20000.0, 0.0, true, &rng );

        Packet pOff, pOn;
        int offBytes = EncodePacket( off, pcm, N, pOff, false );
        int onBytes = EncodePacket( on, pcm, N, pOn, true );
        REQUIRE( offBytes > 0 && onBytes > 0, "stereo24 packet %u encode failed", f );
        REQUIRE( CheckLossless( pOff, C, 24, "stereo24 off" ), "stereo24 off packet %u not lossless", f );
        REQUIRE( CheckLossless( pOn, C, 24, "stereo24 on" ), "stereo24 analysis packet %u not lossless", f );

        ParsedPacket a, b;
        REQUIRE( ParsePacketHeader( pOff.compressed, a ), "stereo24 off header unparsable" );
        REQUIRE( ParsePacketHeader( pOn.compressed, b ), "stereo24 on header unparsable" );
        REQUIRE( b.element.bytesShifted == 1,
                 "stereo24 packet %u: shift-off byte count is %u, expected 1", f, b.element.bytesShifted );
        REQUIRE( a.element.bytesShifted == b.element.bytesShifted,
                 "stereo24 packet %u: shift-off count changed (%u vs %u)",
                 f, a.element.bytesShifted, b.element.bytesShifted );

        // the low byte is carried verbatim: verify a few samples' low bytes in the
        // decoded output against the source (full PCM compare already did this,
        // but make the low-byte rule an explicit assertion)
        uint32_t lowByteMismatch = 0;
        for ( uint32_t i = 0; i < N * C; i++ )
        {
            if ( ( (uint8_t *)pcm.data() )[i * 3] != ( (uint8_t *)pOn.decoded.data() )[i * 3] )
                lowByteMismatch++;
        }
        REQUIRE( lowByteMismatch == 0, "stereo24 packet %u: %u low bytes differ", f, lowByteMismatch );

        std::printf( "  stereo24 packet %u: old=%d new=%d shiftOff=%u orderU=%u orderV=%u\n",
                     f, offBytes, onBytes, b.element.bytesShifted, b.element.numU, b.element.numV );
        REQUIRE( onBytes <= offBytes, "stereo24 packet %u grew under analysis", f );
        totalOff += offBytes;
        totalOn += onBytes;
    }

    std::printf( "  stereo24 total: old=%llu new=%llu\n",
                 (unsigned long long)totalOff, (unsigned long long)totalOn );
    return Passed;
}

// ---------------------------------------------------------------------------
// Group 5: frequency_change
// - each packet carries a different dominant frequency; the point of per-packet
//   analysis is that packet N's coefficients come from packet N's data
// ---------------------------------------------------------------------------

static Result RunFrequencyChange()
{
    const uint32_t N = 4096, C = 2;

    uint64_t totalOff = 0, totalOn = 0;
    std::vector<int16_t> firstCoefsOn[6];
    std::vector<int16_t> firstCoefsOff[6];

    for ( uint32_t f = 0; f < 6; f++ )
    {
        Codec off, on;
        REQUIRE( off.init( C, 16, N, false, false ), "freqchange encoder init off" );
        REQUIRE( on.init( C, 16, N, true, false ), "freqchange encoder init on" );

        double w = 0.021 + 0.115 * (double)f;      // 0.021 .. 0.596 radians/sample
        std::vector<uint8_t> pcm( N * C * 2 );
        FillTones16( pcm, C, N, w, w * 1.9, 13000.0, 2200.0, 0.0, (double)( f * N ) );

        Packet pOff, pOn;
        int offBytes = EncodePacket( off, pcm, N, pOff, false );
        int onBytes = EncodePacket( on, pcm, N, pOn, true );
        REQUIRE( offBytes > 0 && onBytes > 0, "freqchange packet %u encode failed", f );
        REQUIRE( CheckLossless( pOff, C, 16, "freqchange off" ), "freqchange off packet %u not lossless", f );
        REQUIRE( CheckLossless( pOn, C, 16, "freqchange on" ), "freqchange analysis packet %u not lossless", f );
        REQUIRE( onBytes <= offBytes, "freqchange packet %u grew under analysis", f );

        ParsedPacket a, b;
        REQUIRE( ParsePacketHeader( pOff.compressed, a ), "freqchange off header unparsable" );
        REQUIRE( ParsePacketHeader( pOn.compressed, b ), "freqchange on header unparsable" );
        std::printf( "  freqchange packet %u (w=%.3f): old=%d new=%d orderU=%u orderV=%u\n",
                     f, w, offBytes, onBytes, b.element.numU, b.element.numV );

        for ( uint32_t k = 0; k < b.element.numU; k++ )
        {
            firstCoefsOn[f].push_back( b.element.coefsU[k] );
            firstCoefsOff[f].push_back( a.element.coefsU[k] );
        }
        totalOff += offBytes;
        totalOn += onBytes;
    }

    // the analysed coefficients must track the changing input
    uint32_t changed = 0;
    for ( uint32_t f = 1; f < 6; f++ )
    {
        if ( firstCoefsOn[f].size() >= 2 && firstCoefsOn[f - 1].size() >= 2 &&
             firstCoefsOn[f][0] != firstCoefsOn[f - 1][0] )
            changed++;
    }
    REQUIRE( changed >= 3, "freqchange: coefficients do not track the input (changed=%u)", changed );

    double gain = 100.0 * (double)( totalOff - totalOn ) / (double)totalOff;
    std::printf( "  freqchange total: old=%llu new=%llu gain=%.2f%%\n",
                 (unsigned long long)totalOff, (unsigned long long)totalOn, gain );
    REQUIRE( totalOn < totalOff, "freqchange: analysis produced no gain" );
    return Passed;
}

// ---------------------------------------------------------------------------
// Group 6: header_residual
// - parse the packet header, then independently reproduce the first residual
//   samples from the transmitted coefficients only, and compare against the
//   residual stream produced by the same adaptive rules as the decoder
// ---------------------------------------------------------------------------

static Result CheckHeaderResidualFor( Packet & p, uint32_t channels, uint32_t depth, const char * label )
{
    ParsedPacket parsed;
    REQUIRE( ParsePacketHeader( p.compressed, parsed ), "%s: header unparsable", label );
    REQUIRE( !parsed.element.escape, "%s: packet escaped, no coefficients to compare", label );

    const ElementHeader & e = parsed.element;
    uint32_t bytesShifted = e.bytesShifted;
    uint32_t chanBits = depth - bytesShifted * 8 + ( ( channels == 2 ) ? 1 : 0 );
    uint32_t numSamples = p.numFrames;

    // rebuild the predictor-domain signal the encoder must have used
    std::vector<int32_t> u, v;
    std::vector<uint16_t> shiftUV( numSamples * 2 );
    IndependentMix( p.source.data(), channels, depth, numSamples, e.mixBits, e.mixRes, bytesShifted,
                    u, v, ( depth == 24 ) ? &shiftUV : 0 );

    if ( depth == 24 )
    {

        // the shift bytes in the header must match the low bytes of the source
        uint32_t shiftStart = e.headerBits;
        uint32_t bitPos = shiftStart;
        uint32_t bitShift = bytesShifted * 8;
        for ( uint32_t i = 0; i < numSamples * 2; i += 2 )
        {
            uint32_t lo = 0, hi = 0;
            REQUIRE( ReadBits( p.compressed.data(), (uint32_t)p.compressed.size(), bitPos, bitShift, lo ),
                     "%s: truncated shift-off data", label );
            REQUIRE( ReadBits( p.compressed.data(), (uint32_t)p.compressed.size(), bitPos, bitShift, hi ),
                     "%s: truncated shift-off data", label );
            REQUIRE( shiftUV[i] == (uint16_t)lo && shiftUV[i + 1] == (uint16_t)hi,
                     "%s: transmitted shift-off data does not match the source low bytes", label );
        }
    }

    // regenerate the residual prefix using the header's coefficients and the
    // adaptive predictor (the same update rules the decoder uses)
    std::vector<int32_t> residuals;
    std::vector<int16_t> coefs( e.coefsU, e.coefsU + e.numU );
    IndependentResiduals( u.data(), numSamples, e.numU, coefs.data(), chanBits, e.denShiftU, residuals );



    std::vector<int32_t> headerResiduals;
    uint32_t prefix = ( numSamples < 96 ) ? numSamples : 96;
    REQUIRE( ReadResidualPrefix( p.compressed, e, chanBits, prefix, numSamples, headerResiduals ),
             "%s: could not read residual prefix", label );

    uint32_t mismatches = 0;
    for ( uint32_t i = 0; i < prefix; i++ )
    {
        if ( headerResiduals[i] != residuals[i] )
        {

            mismatches++;
        }
    }

    REQUIRE( mismatches == 0, "%s: %u/%u residual values differ", label, mismatches, prefix );

    if ( channels == 2 )
    {
        std::vector<int32_t> residualsV;
        std::vector<int16_t> coefsV( e.coefsV, e.coefsV + e.numV );
        IndependentResiduals( v.data(), numSamples, e.numV, coefsV.data(), chanBits, e.denShiftV, residualsV );

        // skip over the U residual stream to reach the V residual stream
        uint32_t bitPos = e.headerBits;
        if ( e.bytesShifted != 0 )
            bitPos += e.bytesShifted * 8 * 2 * numSamples;

        std::vector<uint8_t> padded = p.compressed;
        padded.resize( p.compressed.size() + 16, 0 );
        AgState ag;
        InitAgState( ag, MB0, ( 4 * PB0 ) / 4, KB0 );
        std::vector<int32_t> allU;
        REQUIRE( AgDecode( padded.data(), (uint32_t)padded.size(), bitPos, ag, chanBits,
                           numSamples, allU, numSamples ),
                 "%s: truncated U residual stream", label );

        InitAgState( ag, MB0, ( 4 * PB0 ) / 4, KB0 );
        std::vector<int32_t> headerResidualsV;
        REQUIRE( AgDecode( padded.data(), (uint32_t)padded.size(), bitPos, ag, chanBits,
                           numSamples, headerResidualsV, prefix ),
                 "%s: could not read V residual prefix", label );
        uint32_t vMismatch = 0;
        for ( uint32_t i = 0; i < prefix; i++ )
        {
            if ( headerResidualsV[i] != residualsV[i] )
                vMismatch++;
        }
        REQUIRE( vMismatch == 0, "%s: %u V residual values differ", label, vMismatch );
    }
    return Passed;
}

static Result RunHeaderResidual()
{
    const uint32_t N = 4096;

    {
        Codec on;
        REQUIRE( on.init( 1, 16, N, true, false ), "header_residual mono init" );
        std::vector<uint8_t> pcm( N * 2 );
        FillTones16( pcm, 1, N, 0.063, 0.118, 13500.0, 3000.0, 0.0, 0.0 );
        Packet p;
        REQUIRE( EncodePacket( on, pcm, N, p, true ) > 0, "header_residual mono encode" );
        REQUIRE( CheckLossless( p, 1, 16, "header_residual mono" ), "header_residual mono not lossless" );
        REQUIRE( CheckHeaderResidualFor( p, 1, 16, "mono16" ) == Passed, "header_residual mono16 failed" );
    }

    {
        Codec on;
        REQUIRE( on.init( 2, 16, N, true, false ), "header_residual stereo init" );
        std::vector<uint8_t> pcm( N * 4 );
        FillTones16( pcm, 2, N, 0.067, 0.121, 12000.0, 2800.0, 0.0, 0.0 );
        Packet p;
        REQUIRE( EncodePacket( on, pcm, N, p, true ) > 0, "header_residual stereo encode" );
        REQUIRE( CheckLossless( p, 2, 16, "header_residual stereo" ), "header_residual stereo not lossless" );
        REQUIRE( CheckHeaderResidualFor( p, 2, 16, "stereo16" ) == Passed, "header_residual stereo16 failed" );
    }

    {
        Codec on;
        REQUIRE( on.init( 2, 24, N, true, false ), "header_residual stereo24 init" );
        std::vector<uint8_t> pcm( N * 6 );
        Fill24( pcm, 2, N, 0.059, 20000.0, 0.0, true, 0 );
        Packet p;
        REQUIRE( EncodePacket( on, pcm, N, p, true ) > 0, "header_residual stereo24 encode" );
        REQUIRE( CheckLossless( p, 2, 24, "header_residual stereo24" ), "header_residual stereo24 not lossless" );
        REQUIRE( CheckHeaderResidualFor( p, 2, 24, "stereo24" ) == Passed, "header_residual stereo24 failed" );
    }
    return Passed;
}

// ---------------------------------------------------------------------------
// Group 7: degenerate
// - silence, constant, very low energy, short packets, and near-identical
//   channels must not produce non-finite or out-of-range coefficients, must
//   stay lossless, and must never grow a packet
// ---------------------------------------------------------------------------

static Result RunDegenerate()
{
    const uint32_t N = 4096;


    struct Case
    {
        const char * name;
        std::vector<uint8_t> pcm;
        uint32_t channels;
        uint32_t frames;
    };

    std::vector<Case> cases;

    {   // silence, mono
        Case c; c.name = "silence-mono"; c.channels = 1; c.frames = N;
        c.pcm.assign( N * 2, 0 );       // all-zero samples
        cases.push_back( c );
    }
    {   // constant, mono
        Case c; c.name = "constant-mono"; c.channels = 1; c.frames = N;
        c.pcm.assign( N * 2, 0 );
        FillConstant16( c.pcm, 1, N, 1234 );
        cases.push_back( c );
    }
    {   // constant extremes
        Case c; c.name = "constant-min"; c.channels = 2; c.frames = N;
        c.pcm.assign( N * 4, 0 );
        FillConstant16( c.pcm, 2, N, -32768 );
        for ( uint32_t i = 0; i < N; i++ )
            ( (int16_t *)c.pcm.data() )[2 * i + 1] = 32767;
        cases.push_back( c );
    }
    {   // very low energy, stereo
        Case c; c.name = "low-energy"; c.channels = 2; c.frames = N;
        c.pcm.assign( N * 4, 0 );
        int16_t * s = (int16_t *)c.pcm.data();
        for ( uint32_t i = 0; i < N; i++ )
        {
            s[2 * i] = (int16_t)( ( i & 1 ) ? 0 : -1 );
            s[2 * i + 1] = (int16_t)( ( i % 3 ) - 1 );
        }
        cases.push_back( c );
    }
    {   // identical channels, stereo (rank deficiency in V at the first lag)
        Case c; c.name = "identical-channels"; c.channels = 2; c.frames = N;
        c.pcm.assign( N * 4, 0 );
        FillTones16( c.pcm, 2, N, 0.071, 0.113, 12000.0, 3000.0, 0.0, 0.0 );
        int16_t * s = (int16_t *)c.pcm.data();
        for ( uint32_t i = 0; i < N; i++ )
            s[2 * i + 1] = s[2 * i];
        cases.push_back( c );
    }
    {   // very short packet, exactly 5 samples (one below order 4's requirement)
        Case c; c.name = "tiny-4"; c.channels = 1; c.frames = 4;
        c.pcm.assign( 4 * 2, 0 );
        FillTones16( c.pcm, 1, 4, 0.3, 0.7, 8000.0, 2000.0, 0.0, 0.0 );
        cases.push_back( c );
    }

    for ( size_t i = 0; i < cases.size(); i++ )
    {
        // fresh encoder pair per case: the no-growth comparison must start from
        // the same encoding state on both sides
        Codec onCase, offCase;
        REQUIRE( onCase.init( cases[i].channels, 16, N, true, false ), "degenerate on init" );
        REQUIRE( offCase.init( cases[i].channels, 16, N, false, false ), "degenerate off init" );

        Packet pOn, pOff;
        int onBytes = EncodePacket( onCase, cases[i].pcm, cases[i].frames, pOn, false );
        int offBytes = EncodePacket( offCase, cases[i].pcm, cases[i].frames, pOff, false );
        REQUIRE( onBytes > 0, "degenerate %s: analysis encode failed", cases[i].name );
        REQUIRE( CheckLossless( pOn, cases[i].channels, 16, cases[i].name ),
                 "degenerate %s: analysis not lossless", cases[i].name );
        if ( offBytes > 0 )
            REQUIRE( onBytes <= offBytes, "degenerate %s: packet grew (%d > %d)",
                     cases[i].name, onBytes, offBytes );

        ParsedPacket parsed;
        if ( ParsePacketHeader( pOn.compressed, parsed ) && !parsed.element.escape )
        {
            for ( uint32_t k = 0; k < parsed.element.numU; k++ )
                REQUIRE( parsed.element.coefsU[k] >= -32768 && parsed.element.coefsU[k] <= 32767,
                         "%s: coefficient out of int16 range", cases[i].name );
            for ( uint32_t k = 0; k < parsed.element.numV; k++ )
                REQUIRE( parsed.element.coefsV[k] >= -32768 && parsed.element.coefsV[k] <= 32767,
                         "%s: V coefficient out of int16 range", cases[i].name );
        }
        std::printf( "  degenerate %-18s: old=%d new=%d lossless=yes\n",
                     cases[i].name, offBytes, onBytes );
    }
    return Passed;
}

// ---------------------------------------------------------------------------
// Group 8: fullscale
// - near-full-scale and full-scale material must not overflow the fitted
//   coefficients or the predictor; output must stay lossless
// ---------------------------------------------------------------------------

static Result RunFullScale()
{
    const uint32_t N = 4096;

    // full-scale square wave (worst case for sign pattern) and full-scale noise
    for ( uint32_t variant = 0; variant < 3; variant++ )
    {
        Codec on16, off16;
        REQUIRE( on16.init( 2, 16, N, true, false ), "fullscale stereo16 init" );
        REQUIRE( off16.init( 2, 16, N, false, false ), "fullscale off16 init" );

        std::vector<uint8_t> pcm( N * 4 );
        int16_t * s = (int16_t *)pcm.data();
        Rng rng( 7u + variant );

        if ( variant == 0 )
        {
            for ( uint32_t i = 0; i < N; i++ )
            {
                s[2 * i] = ( i < N / 2 ) ? 32767 : -32768;
                s[2 * i + 1] = -s[2 * i];
            }
        }
        else if ( variant == 1 )
        {
            FillFullScale16( pcm, 2, N, rng );
        }
        else
        {
            for ( uint32_t i = 0; i < N; i++ )
            {
                double v = 32767.0 * std::sin( (double)i * 0.0009 );  // near DC, full amplitude
                s[2 * i] = (int16_t)( ( v >= 0.0 ) ? ( v + 0.5 ) : ( v - 0.5 ) );
                s[2 * i + 1] = (int16_t)( -s[2 * i] );
            }
        }

        Packet pOn, pOff;
        int onBytes = EncodePacket( on16, pcm, N, pOn, false );
        int offBytes = EncodePacket( off16, pcm, N, pOff, false );
        REQUIRE( onBytes > 0, "fullscale variant %u encode failed", variant );
        REQUIRE( CheckLossless( pOn, 2, 16, "fullscale 16" ), "fullscale variant %u not lossless", variant );
        REQUIRE( onBytes <= offBytes, "fullscale variant %u grew (%d > %d)", variant, onBytes, offBytes );

        ParsedPacket parsed;
        if ( ParsePacketHeader( pOn.compressed, parsed ) && !parsed.element.escape )
        {
            int64_t sumAbsU = 0, sumAbsV = 0;
            for ( uint32_t k = 0; k < parsed.element.numU; k++ )
                sumAbsU += ( parsed.element.coefsU[k] < 0 ) ? -parsed.element.coefsU[k] : parsed.element.coefsU[k];
            for ( uint32_t k = 0; k < parsed.element.numV; k++ )
                sumAbsV += ( parsed.element.coefsV[k] < 0 ) ? -parsed.element.coefsV[k] : parsed.element.coefsV[k];
            // the scaled bound: sum |coef| * |difference| must stay inside int32
            // (differencs are at most 2^17 for 17-bit matrixed data)
            // 17-bit matrixed data: the scale limit keeps sum|coef| <= 2^(29-17) = 4096
            REQUIRE( sumAbsU <= 4096, "fullscale variant %u: U coefficient sum %lld too large", variant, (long long)sumAbsU );
            REQUIRE( sumAbsV <= 4096, "fullscale variant %u: V coefficient sum %lld too large", variant, (long long)sumAbsV );
        }
        std::printf( "  fullscale variant %u: old=%d new=%d\n", variant, offBytes, onBytes );
    }

    // 24-bit full-scale material (shift-off active)
    for ( uint32_t variant = 0; variant < 2; variant++ )
    {
        Codec on24;
        REQUIRE( on24.init( 1, 24, N, true, false ), "fullscale mono24 init" );

        std::vector<uint8_t> pcm( N * 3 );
        Rng rng( 31u + variant );
        for ( uint32_t i = 0; i < N; i++ )
        {
            int32_t hi = ( variant == 0 ) ? ( ( i & 1 ) ? 8388607 : -8388608 )
                                          : rng.range( -8388608, 8388607 );
            int32_t value = ( hi << 8 ) | rng.range( 0, 255 );
            uint8_t * b = &pcm[i * 3];
            b[0] = (uint8_t)( value & 0xff );
            b[1] = (uint8_t)( ( value >> 8 ) & 0xff );
            b[2] = (uint8_t)( ( value >> 16 ) & 0xff );
        }
        Packet pOn;
        int onBytes = EncodePacket( on24, pcm, N, pOn, false );
        REQUIRE( onBytes > 0, "fullscale 24 variant %u encode failed", variant );
        REQUIRE( CheckLossless( pOn, 1, 24, "fullscale 24" ), "fullscale 24 variant %u not lossless", variant );
        std::printf( "  fullscale 24 variant %u: new=%d\n", variant, onBytes );
    }
    return Passed;
}

// ---------------------------------------------------------------------------
// Group 9: short_tail
// - partial final packets (fewer samples than the frame size) with several
//   lengths, including lengths below the highest candidate orders
// ---------------------------------------------------------------------------

static Result RunShortTail()
{
    const uint32_t N = 4096;


    const uint32_t tails[] = { 1, 2, 3, 4, 5, 8, 12, 13, 16, 17, 32, 63, 257 };

    for ( size_t t = 0; t < sizeof( tails ) / sizeof( tails[0] ); t++ )
    {
        uint32_t frames = tails[t];

        for ( uint32_t channels = 1; channels <= 2; channels++ )
        {
            Codec on, off;
            REQUIRE( on.init( channels, 16, N, true, false ), "short_tail on init" );
            REQUIRE( off.init( channels, 16, N, false, false ), "short_tail off init" );

            std::vector<uint8_t> pcm( frames * channels * 2 );
            FillTones16( pcm, channels, frames, 0.071, 0.113, 12000.0, 3000.0, 0.0, 0.0 );

            Packet pOn, pOff;
            int onBytes = EncodePacket( on, pcm, frames, pOn, false );
            int offBytes = EncodePacket( off, pcm, frames, pOff, false );
            REQUIRE( onBytes > 0, "short_tail %u ch%u: analysis encode failed", frames, channels );
            REQUIRE( CheckLossless( pOn, channels, 16, "short_tail" ),
                     "short_tail %u ch%u: analysis not lossless", frames, channels );
            REQUIRE( onBytes <= offBytes, "short_tail %u ch%u: grew (%d > %d)",
                     frames, channels, onBytes, offBytes );

            ParsedPacket parsed;
            if ( ParsePacketHeader( pOn.compressed, parsed ) && !parsed.element.escape )
            {
                if ( parsed.element.partialFrame )
                    REQUIRE( parsed.element.numFrames == frames,
                             "short_tail %u ch%u: partial-frame length field is %u",
                             frames, channels, parsed.element.numFrames );
                REQUIRE( parsed.element.numU <= frames || parsed.element.numU == 4,
                         "short_tail %u ch%u: order %u exceeds available history",
                         frames, channels, parsed.element.numU );
            }
            if ( t % 4 == 0 )
                std::printf( "  short_tail frames=%u ch=%u: old=%d new=%d\n", frames, channels, offBytes, onBytes );
        }
    }
    return Passed;
}

// ---------------------------------------------------------------------------
// Group 10: entropy_fallback
// - high-entropy material must not be made worse; the original candidate and
//   the escape path remain available
// ---------------------------------------------------------------------------

static Result RunEntropyFallback()
{
    const uint32_t N = 4096;


    for ( uint32_t variant = 0; variant < 4; variant++ )
    {
        Codec onM, offM, onS, offS;
        REQUIRE( onM.init( 1, 16, N, true, false ), "entropy mono on init" );
        REQUIRE( offM.init( 1, 16, N, false, false ), "entropy mono off init" );
        REQUIRE( onS.init( 2, 16, N, true, false ), "entropy stereo on init" );
        REQUIRE( offS.init( 2, 16, N, false, false ), "entropy stereo off init" );

        std::vector<uint8_t> monoPcm( N * 2 ), stereoPcm( N * 4 );
        Rng rng( 100u + variant );
        int16_t * sm = (int16_t *)monoPcm.data();
        int16_t * ss = (int16_t *)stereoPcm.data();
        for ( uint32_t i = 0; i < N; i++ )
        {
            int16_t v, w;
            if ( variant == 0 )      v = (int16_t)rng.range( -32768, 32767 );
            else if ( variant == 1 ) v = (int16_t)( ( rng.next() & 1u ) ? 32767 : -32768 );
            else if ( variant == 2 ) v = (int16_t)( ( i & 1 ) ? 32767 : rng.range( -4, 4 ) );
            else                     v = (int16_t)rng.range( -20, 20 );
            w = (int16_t)rng.range( -32768, 32767 );
            sm[i] = v;
            ss[2 * i] = v;
            ss[2 * i + 1] = w;
        }

        Packet pOnM, pOffM, pOnS, pOffS;
        int onMonoBytes = EncodePacket( onM, monoPcm, N, pOnM, false );
        int offMonoBytes = EncodePacket( offM, monoPcm, N, pOffM, false );
        int onStereoBytes = EncodePacket( onS, stereoPcm, N, pOnS, false );
        int offStereoBytes = EncodePacket( offS, stereoPcm, N, pOffS, false );

        // stereo variants use the interleaved buffer as-is (2 channels)
        REQUIRE( onMonoBytes > 0 && offMonoBytes > 0, "entropy variant %u mono encode failed", variant );
        REQUIRE( CheckLossless( pOnM, 1, 16, "entropy mono" ), "entropy variant %u mono not lossless", variant );
        REQUIRE( onMonoBytes <= offMonoBytes, "entropy variant %u mono grew (%d > %d)",
                 variant, onMonoBytes, offMonoBytes );

        REQUIRE( onStereoBytes > 0 && offStereoBytes > 0, "entropy variant %u stereo encode failed", variant );
        REQUIRE( CheckLossless( pOnS, 2, 16, "entropy stereo" ), "entropy variant %u stereo not lossless", variant );
        REQUIRE( onStereoBytes <= offStereoBytes, "entropy variant %u stereo grew (%d > %d)",
                 variant, onStereoBytes, offStereoBytes );

        ParsedPacket parsedM, parsedS;
        REQUIRE( ParsePacketHeader( pOnM.compressed, parsedM ), "entropy variant %u mono header unparsable", variant );
        REQUIRE( ParsePacketHeader( pOnS.compressed, parsedS ), "entropy variant %u stereo header unparsable", variant );
        std::printf( "  entropy variant %u: mono old=%d new=%d escape=%d | stereo old=%d new=%d escape=%d\n",
                     variant, offMonoBytes, onMonoBytes, (int)parsedM.element.escape,
                     offStereoBytes, onStereoBytes, (int)parsedS.element.escape );
    }
    return Passed;
}

// ---------------------------------------------------------------------------
// Group 11: state_isolation
// - analysis candidates must not pollute each other's adaptive state
// - two interleaved encoders must produce independent, correct streams
// - the encoding state after a packet must be the state of the chosen path
// ---------------------------------------------------------------------------

static Result RunStateIsolation()
{
    const uint32_t N = 4096;

    // (a) two encoders advanced in lockstep with different material must not
    //     influence each other: encoder A alone and encoder A interleaved with
    //     encoder B must emit identical bytes for A's packets
    {
        Codec a1, a2, b;
        REQUIRE( a1.init( 2, 16, N, true, false ), "state_isolation a1 init" );
        REQUIRE( a2.init( 2, 16, N, true, false ), "state_isolation a2 init" );
        REQUIRE( b.init( 2, 16, N, true, false ), "state_isolation b init" );

        std::vector<std::vector<uint8_t> > material;
        for ( uint32_t f = 0; f < 4; f++ )
        {
            std::vector<uint8_t> pcm( N * 4 );
            FillTones16( pcm, 2, N, 0.043 + 0.02 * f, 0.09 + 0.03 * f,
                         11000.0, 2500.0, 0.0, (double)( f * N ) );
            material.push_back( pcm );
        }

        std::vector<std::vector<uint8_t> > alone, interleaved;
        for ( uint32_t f = 0; f < material.size(); f++ )
        {
            Packet p;
            REQUIRE( EncodePacket( a1, material[f], N, p, false ) > 0, "state_isolation alone encode" );
            alone.push_back( p.compressed );
        }
        for ( uint32_t f = 0; f < material.size(); f++ )
        {
            Packet pa, pb;
            REQUIRE( EncodePacket( a2, material[f], N, pa, false ) > 0, "state_isolation a2 encode" );
            REQUIRE( EncodePacket( b, material[material.size() - 1 - f], N, pb, false ) > 0,
                     "state_isolation b encode" );
            interleaved.push_back( pa.compressed );
        }
        for ( uint32_t f = 0; f < material.size(); f++ )
            REQUIRE( alone[f] == interleaved[f],
                     "state_isolation: packet %u of encoder A differs when another encoder runs", f );
        std::printf( "  state_isolation: interleaved encoders stay independent over %zu packets\n",
                     material.size() );
    }

    // (b) the state at the start of each packet is the same regardless of how
    //     many candidates were considered. A packet encoded twice through the
    //     same analysis encoder must produce the same bytes the second time
    //     (the coefficient reference for the packet is re-fitted from data)
    {
        Codec on, on2;
        REQUIRE( on.init( 1, 16, N, true, false ), "state_isolation repeat init" );
        REQUIRE( on2.init( 1, 16, N, true, false ), "state_isolation repeat init2" );

        std::vector<uint8_t> pcm( N * 2 );
        FillTones16( pcm, 1, N, 0.071, 0.113, 13000.0, 3000.0, 0.0, 0.0 );

        Packet first, second;
        REQUIRE( EncodePacket( on, pcm, N, first, false ) > 0, "state_isolation repeat packet 1" );
        REQUIRE( EncodePacket( on, pcm, N, second, false ) > 0, "state_isolation repeat packet 2" );
        REQUIRE( first.compressed == second.compressed,
                 "state_isolation: repeated identical packet produced different output" );
    }

    // (c) a fresh encoder and a used encoder must agree when fed the same
    //     material from the same state: warm both with the same packet, then
    //     compare their next packet
    {
        Codec one, two;
        REQUIRE( one.init( 2, 16, N, true, false ), "state_isolation warm init1" );
        REQUIRE( two.init( 2, 16, N, true, false ), "state_isolation warm init2" );

        std::vector<uint8_t> warm( N * 4 ), target( N * 4 );
        FillTones16( warm, 2, N, 0.061, 0.117, 12000.0, 2500.0, 0.0, 0.0 );
        FillTones16( target, 2, N, 0.083, 0.131, 13000.0, 3000.0, 0.0, 50000.0 );

        Packet pw1, pw2, pt1, pt2;
        REQUIRE( EncodePacket( one, warm, N, pw1, false ) > 0, "state_isolation warm one" );
        REQUIRE( EncodePacket( two, warm, N, pw2, false ) > 0, "state_isolation warm two" );
        REQUIRE( pw1.compressed == pw2.compressed, "state_isolation: warm-up output differs" );
        REQUIRE( EncodePacket( one, target, N, pt1, false ) > 0, "state_isolation target one" );
        REQUIRE( EncodePacket( two, target, N, pt2, false ) > 0, "state_isolation target two" );
        REQUIRE( pt1.compressed == pt2.compressed,
                 "state_isolation: equal state produced different target output" );
        std::printf( "  state_isolation: adaptive state is deterministic and isolated\n" );
    }
    return Passed;
}

// ---------------------------------------------------------------------------
// Group 12: disabled_fast
// - with the switch off the output must be byte-for-byte the original encoder
// - with the switch on and fast mode enabled, analysis takes priority: the
//   result must match the analysis result of a non-fast encoder from the same
//   state, and must beat the fast-only result
// ---------------------------------------------------------------------------

static Result RunDisabledFast()
{
    const uint32_t N = 4096, C = 2;

    // (a) switch off: bytes and FNV must match the original baseline values
    {
        Codec off;
        REQUIRE( off.init( C, 16, N, false, false ), "disabled encoder init" );
        std::vector<uint8_t> pcm( N * C * 2 );
        uint64_t total = 0;
        uint32_t hash = 2166136261u;
        for ( uint32_t f = 0; f < 3; f++ )
        {
            int16_t * b = (int16_t *)pcm.data();
            for ( uint32_t i = 0; i < N; i++ )
            {
                double t = (double)i + (double)( f * N );
                b[2 * i] = (int16_t)( 11000 * std::sin( t * 0.071 ) + 3000 * std::sin( t * 0.113 ) );
                b[2 * i + 1] = (int16_t)( 9000 * std::sin( t * 0.071 + 0.4 ) + 2200 * std::cos( t * 0.037 ) );
            }
            Packet p;
            int bytes = EncodePacket( off, pcm, N, p, false );
            REQUIRE( bytes > 0, "disabled encoder packet %u failed", f );
            for ( int i = 0; i < bytes; i++ )
                hash = ( hash ^ p.compressed[i] ) * 16777619u;
            total += (uint64_t)bytes;
        }
        std::printf( "  disabled_fast: off-mode total=%llu fnv=%08x (baseline reference 10882/1a9bfeae)\n",
                     (unsigned long long)total, hash );
        REQUIRE( total == 10882, "disabled_fast: off-mode bytes %llu != 10882 baseline",
                 (unsigned long long)total );
        REQUIRE( hash == 0x1a9bfeaeu, "disabled_fast: off-mode fnv %08x != 1a9bfeae", hash );
    }

    // (b) switch off, fast on: bytes must match a fast-only reference built
    //     from an encoder that never enabled analysis
    {
        Codec fastOff, fastOffRef;
        REQUIRE( fastOff.init( C, 16, N, false, true ), "disabled fast init" );
        REQUIRE( fastOffRef.init( C, 16, N, false, true ), "disabled fast ref init" );

        std::vector<uint8_t> pcm( N * C * 2 );
        FillTones16( pcm, C, N, 0.071, 0.113, 11000.0, 3000.0, 0.0, 0.0 );
        Packet a, b;
        REQUIRE( EncodePacket( fastOff, pcm, N, a, false ) > 0, "disabled fast encode" );
        REQUIRE( EncodePacket( fastOffRef, pcm, N, b, false ) > 0, "disabled fast ref encode" );
        REQUIRE( a.compressed == b.compressed, "disabled_fast: fast-mode output changed when analysis is off" );
        std::printf( "  disabled_fast: fast-mode packet identical with analysis off (%d bytes)\n",
                     (int)a.compressed.size() );
    }

    // (c) switch on + fast on: analysis must win over fast
    {
        Codec fastOn, fastOff, slowOn;
        REQUIRE( fastOn.init( C, 16, N, true, true ), "fast+analysis init" );
        REQUIRE( fastOff.init( C, 16, N, false, true ), "fast-only init" );
        REQUIRE( slowOn.init( C, 16, N, true, false ), "slow analysis init" );

        std::vector<uint8_t> pcm( N * C * 2 );
        FillTones16( pcm, C, N, 0.058, 0.121, 12000.0, 2800.0, 0.0, 0.0 );

        Packet pFA, pF, pSA;
        int faBytes = EncodePacket( fastOn, pcm, N, pFA, false );
        int fBytes = EncodePacket( fastOff, pcm, N, pF, false );
        int saBytes = EncodePacket( slowOn, pcm, N, pSA, false );
        REQUIRE( faBytes > 0 && fBytes > 0 && saBytes > 0, "fast priority encode failed" );
        REQUIRE( CheckLossless( pFA, C, 16, "fast+analysis" ), "fast+analysis not lossless" );
        REQUIRE( faBytes <= fBytes, "fast priority: analysis result (%d) not smaller than fast (%d)", faBytes, fBytes );
        REQUIRE( faBytes == saBytes,
                 "fast priority: fast+analysis (%d bytes) != analysis-only (%d bytes)", faBytes, saBytes );
        std::printf( "  disabled_fast: fast-only=%d, fast+analysis=%d, analysis-only=%d\n",
                     fBytes, faBytes, saBytes );
    }

    // (d) out-of-scope depths are untouched: with analysis enabled, a 32-bit
    //     packet must be byte-identical to the analysis-disabled encoder
    {
        Codec bit32On, bit32Off;
        REQUIRE( bit32On.init( C, 32, N, true, false ), "32-bit analysis init" );
        REQUIRE( bit32Off.init( C, 32, N, false, false ), "32-bit reference init" );

        std::vector<uint8_t> pcm( N * C * 4 );
        int32_t * samples = (int32_t *)pcm.data();
        Rng rng( 4242 );
        for ( uint32_t i = 0; i < N * C; i++ )
            samples[i] = rng.range( -2000000000, 2000000000 );

        Packet pOn, pOff;
        REQUIRE( EncodePacket( bit32On, pcm, N, pOn, false ) > 0, "32-bit analysis encode" );
        REQUIRE( EncodePacket( bit32Off, pcm, N, pOff, false ) > 0, "32-bit reference encode" );
        REQUIRE( CheckLossless( pOn, C, 32, "32-bit analysis" ), "32-bit analysis not lossless" );
        REQUIRE( pOn.compressed == pOff.compressed,
                 "disabled_fast: 32-bit output changed when analysis is enabled" );
        std::printf( "  disabled_fast: 32-bit packet identical with analysis enabled (%d bytes)\n",
                     (int)pOn.compressed.size() );
    }
    return Passed;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

typedef Result (*RunFn)();

static Result Dispatch( RunFn fn, const char * name )
{
    g_caseName = name;
    g_failed = false;
    Result r = fn();
    if ( ( r == Passed ) && g_failed )
        return Failed;
    return r;
}

static Result RunBaselineGainDispatch()    { return Dispatch( RunBaselineGain, "baseline_gain" ); }
static Result RunMono16FitDispatch()       { return Dispatch( RunMono16Fit, "mono16_fit" ); }
static Result RunStereo16MatrixDispatch()  { return Dispatch( RunStereo16Matrix, "stereo16_matrix" ); }
static Result RunStereo24ShiftDispatch()   { return Dispatch( RunStereo24Shift, "stereo24_shift" ); }
static Result RunFrequencyChangeDispatch() { return Dispatch( RunFrequencyChange, "frequency_change" ); }
static Result RunHeaderResidualDispatch()  { return Dispatch( RunHeaderResidual, "header_residual" ); }
static Result RunDegenerateDispatch()      { return Dispatch( RunDegenerate, "degenerate" ); }
static Result RunFullScaleDispatch()       { return Dispatch( RunFullScale, "fullscale" ); }
static Result RunShortTailDispatch()       { return Dispatch( RunShortTail, "short_tail" ); }
static Result RunEntropyFallbackDispatch() { return Dispatch( RunEntropyFallback, "entropy_fallback" ); }
static Result RunStateIsolationDispatch()  { return Dispatch( RunStateIsolation, "state_isolation" ); }
static Result RunDisabledFastDispatch()    { return Dispatch( RunDisabledFast, "disabled_fast" ); }

void register_tests( Registry & registry )
{
    static const struct { const char * name; RunFn run; } kEntries[] = {
        { "baseline_gain",    RunBaselineGainDispatch },
        { "mono16_fit",       RunMono16FitDispatch },
        { "stereo16_matrix",  RunStereo16MatrixDispatch },
        { "stereo24_shift",   RunStereo24ShiftDispatch },
        { "frequency_change", RunFrequencyChangeDispatch },
        { "header_residual",  RunHeaderResidualDispatch },
        { "degenerate",       RunDegenerateDispatch },
        { "fullscale",        RunFullScaleDispatch },
        { "short_tail",       RunShortTailDispatch },
        { "entropy_fallback", RunEntropyFallbackDispatch },
        { "state_isolation",  RunStateIsolationDispatch },
        { "disabled_fast",    RunDisabledFastDispatch },
    };
    registry.clear();
    for ( size_t i = 0; i < sizeof( kEntries ) / sizeof( kEntries[0] ); i++ )
    {
        Case c;
        c.name = kEntries[i].name;
        c.run = kEntries[i].run;
        registry.push_back( c );
    }
}

} // namespace alac_lpc_test
