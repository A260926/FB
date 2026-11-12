// Contract tests for ELFIO::gnu_hash_rebuilder (elfio/elfio_gnu_hash_rebuild.hpp).
//
// The tests build small ELF objects by hand, corrupt their .gnu.hash content,
// rebuild it through the library entry point and then verify the result
// against an independent reference implementation of the GNU hash format
// written below. References (relocations, versym, shndx, group signatures)
// are checked to still denote the same symbols after the reorder.

#include <elfio/elfio.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#define CHECK( cond )                                                       \
    do {                                                                    \
        if ( !( cond ) ) {                                                  \
            std::cerr << "CHECK failed at line " << __LINE__ << ": " #cond \
                      << std::endl;                                         \
            std::exit( 1 );                                                 \
        }                                                                   \
    } while ( 0 )

//------------------------------------------------------------------------------
// Independent endian helpers (the tests must not rely on ELFIO's convertor)
//------------------------------------------------------------------------------
static void put_u16( std::string& s, uint16_t v, bool msb )
{
    char b[2];
    if ( msb ) {
        b[0] = char( v >> 8 );
        b[1] = char( v );
    }
    else {
        b[0] = char( v );
        b[1] = char( v >> 8 );
    }
    s.append( b, 2 );
}

static void put_u32( std::string& s, uint32_t v, bool msb )
{
    char b[4];
    if ( msb ) {
        b[0] = char( v >> 24 );
        b[1] = char( v >> 16 );
        b[2] = char( v >> 8 );
        b[3] = char( v );
    }
    else {
        b[0] = char( v );
        b[1] = char( v >> 8 );
        b[2] = char( v >> 16 );
        b[3] = char( v >> 24 );
    }
    s.append( b, 4 );
}

static void put_u32_at( std::string& s, size_t off, uint32_t v, bool msb )
{
    std::string tmp;
    put_u32( tmp, v, msb );
    s.replace( off, 4, tmp );
}

static uint16_t get_u16( const char* p, bool msb )
{
    const uint8_t* b = (const uint8_t*)p;
    if ( msb )
        return uint16_t( ( uint16_t( b[0] ) << 8 ) | uint16_t( b[1] ) );
    return uint16_t( ( uint16_t( b[1] ) << 8 ) | uint16_t( b[0] ) );
}

static uint32_t get_u32( const char* p, bool msb )
{
    const uint8_t* b = (const uint8_t*)p;
    if ( msb )
        return ( uint32_t( b[0] ) << 24 ) | ( uint32_t( b[1] ) << 16 ) |
               ( uint32_t( b[2] ) << 8 ) | uint32_t( b[3] );
    return ( uint32_t( b[3] ) << 24 ) | ( uint32_t( b[2] ) << 16 ) |
           ( uint32_t( b[1] ) << 8 ) | uint32_t( b[0] );
}

static uint64_t get_u64( const char* p, bool msb )
{
    uint64_t v = 0;
    if ( msb ) {
        for ( int i = 0; i < 8; ++i )
            v = ( v << 8 ) | uint8_t( p[i] );
    }
    else {
        for ( int i = 7; i >= 0; --i )
            v = ( v << 8 ) | uint8_t( p[i] );
    }
    return v;
}

static void put_u64_at( std::string& s, size_t off, uint64_t v, bool msb )
{
    char b[8];
    for ( int i = 0; i < 8; ++i ) {
        if ( msb )
            b[i] = char( v >> ( 56 - 8 * i ) );
        else
            b[i] = char( v >> ( 8 * i ) );
    }
    s.replace( off, 8, b, 8 );
}

//------------------------------------------------------------------------------
// Independent GNU hash reference implementation
//------------------------------------------------------------------------------
static uint32_t ref_gnu_hash( const std::string& s )
{
    uint32_t h = 5381u;
    for ( unsigned char c : s )
        h = h * 33u + c;
    return h;
}

// Stable grouping of the hashed symbols by bucket number.
// hashes[j] is the hash of the symbol at old index symoffset + j.
// Returns, for every new position in the hashed region, the old symbol index.
static std::vector<uint32_t> ref_new_to_old( uint32_t                    nbuckets,
                                             uint32_t                    symoffset,
                                             const std::vector<uint32_t>& hashes )
{
    std::vector<uint32_t> counts( nbuckets, 0 );
    for ( uint32_t h : hashes )
        ++counts[h % nbuckets];
    std::vector<uint32_t> cursor( nbuckets, 0 );
    uint32_t              pos = symoffset;
    for ( uint32_t b = 0; b < nbuckets; ++b ) {
        cursor[b] = pos;
        pos += counts[b];
    }
    std::vector<uint32_t> out( hashes.size() );
    for ( uint32_t j = 0; j < hashes.size(); ++j ) {
        uint32_t b                       = hashes[j] % nbuckets;
        out[cursor[b]++ - symoffset] = symoffset + j;
    }
    return out;
}

// Full expected .gnu.hash section content, zero padded to capacity.
static std::string ref_gnu_hash_table( bool             is64,
                                       bool             msb,
                                       const ELFIO::gnu_hash_rebuild_params& p,
                                       const std::vector<uint32_t>& hashes,
                                       size_t           capacity )
{
    const uint32_t nchains = (uint32_t)hashes.size();
    const uint32_t wbits   = is64 ? 64 : 32;

    std::vector<uint32_t> grouped = ref_new_to_old( p.nbuckets, p.symoffset, hashes );
    std::vector<uint32_t> counts( p.nbuckets, 0 );
    for ( uint32_t h : hashes )
        ++counts[h % p.nbuckets];
    std::vector<uint32_t> start( p.nbuckets, 0 );
    {
        uint32_t pos = p.symoffset;
        for ( uint32_t b = 0; b < p.nbuckets; ++b ) {
            start[b] = pos;
            pos += counts[b];
        }
    }

    std::string out;
    put_u32( out, p.nbuckets, msb );
    put_u32( out, p.symoffset, msb );
    put_u32( out, p.bloom_size, msb );
    put_u32( out, p.bloom_shift, msb );

    std::vector<uint64_t> bloom( p.bloom_size, 0 );
    for ( uint32_t h : hashes ) {
        uint64_t bits = ( 1ull << ( h % wbits ) ) |
                        ( 1ull << ( ( h >> p.bloom_shift ) % wbits ) );
        bloom[( h / wbits ) % p.bloom_size] |= bits;
    }
    for ( uint32_t i = 0; i < p.bloom_size; ++i ) {
        if ( is64 ) {
            char b[8];
            for ( int k = 0; k < 8; ++k )
                b[k] = msb ? char( bloom[i] >> ( 56 - 8 * k ) )
                           : char( bloom[i] >> ( 8 * k ) );
            out.append( b, 8 );
        }
        else {
            put_u32( out, (uint32_t)bloom[i], msb );
        }
    }

    for ( uint32_t b = 0; b < p.nbuckets; ++b )
        put_u32( out, counts[b] ? start[b] : 0, msb );

    for ( uint32_t k = 0; k < nchains; ++k ) {
        uint32_t h    = hashes[grouped[k] - p.symoffset];
        bool     last = ( k + 1 == nchains );
        if ( !last )
            last = ( hashes[grouped[k + 1] - p.symoffset] % p.nbuckets ) !=
                   ( h % p.nbuckets );
        put_u32( out, ( h & ~1u ) | ( last ? 1u : 0u ), msb );
    }

    CHECK( out.size() <= capacity );
    out.append( capacity - out.size(), '\0' );
    return out;
}

static size_t gnu_hash_need( bool                                  is64,
                             const ELFIO::gnu_hash_rebuild_params& p,
                             uint32_t                              nchains )
{
    return 16 + ( is64 ? 8 : 4 ) * (size_t)p.bloom_size +
           4 * (size_t)p.nbuckets + 4 * (size_t)nchains;
}

//------------------------------------------------------------------------------
// Fixture description
//------------------------------------------------------------------------------
struct SymSpec
{
    std::string name;
    uint64_t    value = 0;
    uint64_t    size  = 0;
    uint8_t     bind  = ELFIO::STB_GLOBAL;
    uint8_t     type  = 0; // STT_NOTYPE
    uint8_t     other = 0;
    uint16_t    shndx = ELFIO::SHN_ABS;
};

struct RelocEntrySpec
{
    uint64_t off;
    uint32_t sym; // old symbol index
    uint32_t type;
    int64_t  addend;
};

struct RelocSecSpec
{
    std::string                 name;
    bool                        rela = false;
    std::vector<RelocEntrySpec> entries;
};

struct Fixture
{
    ELFIO::gnu_hash_rebuild_params params;
    size_t                         hash_capacity = 0;
    std::vector<SymSpec>           syms; // index == old symbol index; [0] null
    uint32_t                       sh_info = 0;
    std::vector<RelocSecSpec>      relocs;
    std::vector<uint16_t>          versym; // empty -> no section
    std::vector<uint32_t>          shndx;  // empty -> no section
    bool                           group     = false;
    uint32_t                       group_sig = 0;
    std::vector<uint32_t>          group_members; // filled by the builder
    bool                           sysv_hash_on_target = false;
};

static SymSpec null_sym()
{
    SymSpec s;
    s.bind  = ELFIO::STB_LOCAL;
    s.shndx = 0;
    return s;
}

static std::string build_object( unsigned char cls, unsigned char enc,
                                 Fixture& fx )
{
    const bool msb = enc == ELFIO::ELFDATA2MSB;

    ELFIO::elfio w;
    w.create( cls, enc );
    w.set_type( ELFIO::ET_DYN );

    auto* str = w.sections.add( ".dynstr" );
    str->set_type( ELFIO::SHT_STRTAB );
    ELFIO::string_section_accessor strings( str );

    auto* sym = w.sections.add( ".dynsym" );
    sym->set_type( ELFIO::SHT_DYNSYM );
    sym->set_entry_size( w.get_default_entry_size( ELFIO::SHT_SYMTAB ) );
    sym->set_link( str->get_index() );
    sym->set_info( fx.sh_info );
    ELFIO::symbol_section_accessor symbols( w, sym );
    for ( size_t i = 1; i < fx.syms.size(); ++i ) {
        const SymSpec&    s   = fx.syms[i];
        ELFIO::Elf_Word   idx = symbols.add_symbol(
            strings, s.name.c_str(), (ELFIO::Elf64_Addr)s.value,
            (ELFIO::Elf_Xword)s.size, s.bind, s.type, s.other,
            (ELFIO::Elf_Half)s.shndx );
        CHECK( idx == i );
    }

    auto* gh = w.sections.add( ".gnu.hash" );
    gh->set_type( ELFIO::SHT_GNU_HASH );
    gh->set_link( sym->get_index() );
    gh->set_addr_align( cls == ELFIO::ELFCLASS64 ? 8 : 4 );
    // Corrupt, unusable content on purpose: the rebuild must not trust it.
    gh->set_data( std::string( fx.hash_capacity, '\xA5' ) );

    for ( const auto& rs : fx.relocs ) {
        auto* sec = w.sections.add( rs.name );
        sec->set_type( rs.rela ? ELFIO::SHT_RELA : ELFIO::SHT_REL );
        sec->set_link( sym->get_index() );
        sec->set_entry_size( w.get_default_entry_size(
            rs.rela ? ELFIO::SHT_RELA : ELFIO::SHT_REL ) );
        ELFIO::relocation_section_accessor rel( w, sec );
        for ( const auto& e : rs.entries ) {
            if ( rs.rela )
                rel.add_entry( (ELFIO::Elf64_Addr)e.off,
                               (ELFIO::Elf_Word)e.sym, (unsigned)e.type,
                               (ELFIO::Elf_Sxword)e.addend );
            else
                rel.add_entry( (ELFIO::Elf64_Addr)e.off,
                               (ELFIO::Elf_Word)e.sym, (unsigned)e.type );
        }
    }

    if ( !fx.versym.empty() ) {
        auto* sec = w.sections.add( ".gnu.version" );
        sec->set_type( ELFIO::SHT_GNU_versym );
        sec->set_link( sym->get_index() );
        sec->set_entry_size( 2 );
        std::string data;
        for ( uint16_t v : fx.versym )
            put_u16( data, v, msb );
        sec->set_data( data );
    }

    if ( !fx.shndx.empty() ) {
        auto* sec = w.sections.add( ".dynsym_shndx" );
        sec->set_type( ELFIO::SHT_SYMTAB_SHNDX );
        sec->set_link( sym->get_index() );
        sec->set_entry_size( 4 );
        std::string data;
        for ( uint32_t v : fx.shndx )
            put_u32( data, v, msb );
        sec->set_data( data );
    }

    if ( fx.group ) {
        auto* m1 = w.sections.add( ".text.a" );
        m1->set_type( ELFIO::SHT_PROGBITS );
        m1->set_data( std::string( "X" ) );
        auto* m2 = w.sections.add( ".text.b" );
        m2->set_type( ELFIO::SHT_PROGBITS );
        m2->set_data( std::string( "YY" ) );
        fx.group_members = { m1->get_index(), m2->get_index() };

        auto* sec = w.sections.add( ".group" );
        sec->set_type( ELFIO::SHT_GROUP );
        sec->set_link( sym->get_index() );
        sec->set_info( fx.group_sig );
        sec->set_entry_size( 4 );
        std::string data;
        put_u32( data, ELFIO::GRP_COMDAT, msb );
        put_u32( data, m1->get_index(), msb );
        put_u32( data, m2->get_index(), msb );
        sec->set_data( data );
    }

    if ( fx.sysv_hash_on_target ) {
        auto* h = w.sections.add( ".hash" );
        h->set_type( ELFIO::SHT_HASH );
        h->set_link( sym->get_index() );
        std::string data;
        put_u32( data, 1, msb );
        put_u32( data, 2, msb );
        put_u32( data, 1, msb );
        put_u32( data, 0, msb );
        h->set_data( data );
    }

    // A second, independent symbol table sharing the same string table,
    // with its own relocation and SysV hash sections. None of these may
    // be touched by a rebuild targeting .dynsym.
    {
        auto* sym2 = w.sections.add( ".symtab2" );
        sym2->set_type( ELFIO::SHT_SYMTAB );
        sym2->set_entry_size( w.get_default_entry_size( ELFIO::SHT_SYMTAB ) );
        sym2->set_link( str->get_index() ); // shared string table
        ELFIO::symbol_section_accessor symbols2( w, sym2 );
        symbols2.add_symbol( strings, "other_local", 0x10, 4,
                             ELFIO::STB_LOCAL, 0, 0, 1 );
        symbols2.add_symbol( strings,
                             fx.syms.size() > 1 ? fx.syms.back().name.c_str()
                                                : "other_global",
                             0x20, 4, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0,
                             ELFIO::SHN_ABS );
        sym2->set_info( 2 );

        auto* rel2 = w.sections.add( ".rela.other" );
        rel2->set_type( ELFIO::SHT_RELA );
        rel2->set_link( sym2->get_index() );
        rel2->set_entry_size( w.get_default_entry_size( ELFIO::SHT_RELA ) );
        ELFIO::relocation_section_accessor r2( w, rel2 );
        r2.add_entry( (ELFIO::Elf64_Addr)0x5000, (ELFIO::Elf_Word)2, 7,
                      (ELFIO::Elf_Sxword)-4 );

        auto* h2 = w.sections.add( ".hash.other" );
        h2->set_type( ELFIO::SHT_HASH );
        h2->set_link( sym2->get_index() );
        std::string data;
        put_u32( data, 1, msb );
        put_u32( data, 3, msb );
        put_u32( data, 1, msb );
        put_u32( data, 0, msb );
        put_u32( data, 0, msb );
        put_u32( data, 0, msb );
        h2->set_data( data );
    }

    std::stringstream ss;
    CHECK( w.save( ss ) );
    return ss.str();
}

//------------------------------------------------------------------------------
// Verification of a rebuilt object against the independent reference
//------------------------------------------------------------------------------
static void verify_result( unsigned char cls, unsigned char enc,
                           const Fixture& fx, const std::string& before_bytes,
                           const std::string&                        after_bytes,
                           const ELFIO::gnu_hash_rebuild_permutation& perm )
{
    const bool msb   = enc == ELFIO::ELFDATA2MSB;
    const bool is64  = cls == ELFIO::ELFCLASS64;
    const uint32_t nsyms    = (uint32_t)fx.syms.size();
    const uint32_t symoffset = fx.params.symoffset;

    ELFIO::elfio before, after;
    {
        std::stringstream ss( before_bytes );
        CHECK( before.load( ss ) );
    }
    {
        std::stringstream ss( after_bytes );
        CHECK( after.load( ss ) );
    }

    auto* sym = after.sections[".dynsym"];
    auto* gh  = after.sections[".gnu.hash"];
    CHECK( sym != nullptr && gh != nullptr );

    // Section count, dynsym size and hash section size/address preserved
    CHECK( before.sections.size() == after.sections.size() );
    CHECK( sym->get_size() == before.sections[".dynsym"]->get_size() );
    CHECK( gh->get_size() == fx.hash_capacity );
    CHECK( gh->get_address() ==
           before.sections[".gnu.hash"]->get_address() );

    // Permutation is a permutation and is the identity below symoffset
    CHECK( perm.old_to_new.size() == nsyms );
    CHECK( perm.new_to_old.size() == nsyms );
    for ( uint32_t i = 0; i < nsyms; ++i )
        CHECK( perm.old_to_new[perm.new_to_old[i]] == i );
    for ( uint32_t i = 0; i < symoffset; ++i ) {
        CHECK( perm.new_to_old[i] == i );
        CHECK( perm.old_to_new[i] == i );
    }

    // Permutation matches the independent stable grouping
    std::vector<uint32_t> hashes;
    for ( uint32_t i = symoffset; i < nsyms; ++i )
        hashes.push_back( ref_gnu_hash( fx.syms[i].name ) );
    std::vector<uint32_t> ref_n2o =
        ref_new_to_old( fx.params.nbuckets, symoffset, hashes );
    for ( uint32_t k = 0; k < ref_n2o.size(); ++k )
        CHECK( perm.new_to_old[symoffset + k] == ref_n2o[k] );

    // Hash section bytes match the independent reference table exactly
    std::string expected =
        ref_gnu_hash_table( is64, msb, fx.params, hashes, gh->get_size() );
    CHECK( std::string( gh->get_data(), gh->get_size() ) == expected );

    // Every symbol sits at its new position with all fields intact
    ELFIO::symbol_section_accessor syms( after, sym );
    CHECK( syms.get_symbols_num() == nsyms );
    for ( uint32_t p = 0; p < nsyms; ++p ) {
        const SymSpec& exp = fx.syms[perm.new_to_old[p]];
        std::string    name;
        ELFIO::Elf64_Addr value;
        ELFIO::Elf_Xword  size;
        unsigned char     bind, type, other;
        ELFIO::Elf_Half   shndx;
        CHECK( syms.get_symbol( p, name, value, size, bind, type, shndx,
                                other ) );
        CHECK( name == exp.name );
        CHECK( value == exp.value );
        CHECK( size == exp.size );
        CHECK( bind == exp.bind );
        CHECK( type == exp.type );
        CHECK( other == exp.other );
        CHECK( shndx == exp.shndx );
    }

    // Null symbol, local prefix and sh_info stay legal
    CHECK( sym->get_info() == fx.sh_info );
    for ( uint32_t i = 0; i < fx.sh_info; ++i ) {
        std::string    name;
        ELFIO::Elf64_Addr value;
        ELFIO::Elf_Xword  size;
        unsigned char     bind, type, other;
        ELFIO::Elf_Half   shndx;
        CHECK( syms.get_symbol( i, name, value, size, bind, type, shndx,
                                other ) );
        CHECK( bind == ELFIO::STB_LOCAL );
    }

    // Name lookup through a fresh accessor (uses the rebuilt GNU hash)
    ELFIO::symbol_section_accessor lookup( after, sym );
    for ( uint32_t i = symoffset; i < nsyms; ++i ) {
        if ( fx.syms[i].name.empty() )
            continue;
        ELFIO::Elf64_Addr value = 0;
        ELFIO::Elf_Xword  size  = 0;
        unsigned char     bind = 0, type = 0, other = 0;
        ELFIO::Elf_Half   shndx = 0;
        CHECK( lookup.get_symbol( fx.syms[i].name, value, size, bind, type,
                                  shndx, other ) );
        bool found = false;
        for ( uint32_t k = 0; k < nsyms; ++k )
            if ( fx.syms[k].name == fx.syms[i].name &&
                 fx.syms[k].value == value && fx.syms[k].size == size )
                found = true;
        CHECK( found );
    }
    {
        // Absent names must not be found
        ELFIO::Elf64_Addr value;
        ELFIO::Elf_Xword  size;
        unsigned char     bind, type, other;
        ELFIO::Elf_Half   shndx;
        CHECK( !lookup.get_symbol( "definitely_absent_symbol", value, size,
                                   bind, type, shndx, other ) );
    }

    // Relocations still reference the same symbols; offset/type/addend kept
    for ( const auto& rs : fx.relocs ) {
        auto* sec = after.sections[rs.name];
        CHECK( sec != nullptr );
        ELFIO::relocation_section_accessor rel( after, sec );
        CHECK( rel.get_entries_num() == rs.entries.size() );
        for ( size_t e = 0; e < rs.entries.size(); ++e ) {
            ELFIO::Elf64_Addr off;
            ELFIO::Elf_Word   symidx;
            unsigned          type;
            ELFIO::Elf_Sxword addend;
            CHECK( rel.get_entry( e, off, symidx, type, addend ) );
            const RelocEntrySpec& exp = rs.entries[e];
            CHECK( off == exp.off );
            CHECK( type == exp.type );
            if ( rs.rela )
                CHECK( addend == exp.addend );
            const SymSpec&    referent = fx.syms[exp.sym];
            std::string       name;
            ELFIO::Elf64_Addr value;
            ELFIO::Elf_Xword  size;
            unsigned char     bind, stype, other;
            ELFIO::Elf_Half   shndx;
            CHECK( syms.get_symbol( symidx, name, value, size, bind, stype,
                                    shndx, other ) );
            CHECK( name == referent.name );
            CHECK( value == referent.value );
            CHECK( size == referent.size );
        }
    }

    // Versym halfwords moved raw with the permutation (hidden bits kept)
    if ( !fx.versym.empty() ) {
        auto* sec = after.sections[".gnu.version"];
        CHECK( sec != nullptr );
        CHECK( sec->get_size() == (ELFIO::Elf_Xword)nsyms * 2 );
        const char* d = sec->get_data();
        for ( uint32_t p = 0; p < nsyms; ++p )
            CHECK( get_u16( d + 2 * p, msb ) ==
                   fx.versym[perm.new_to_old[p]] );
    }

    // Shndx words moved raw with the permutation
    if ( !fx.shndx.empty() ) {
        auto* sec = after.sections[".dynsym_shndx"];
        CHECK( sec != nullptr );
        CHECK( sec->get_size() == (ELFIO::Elf_Xword)nsyms * 4 );
        const char* d = sec->get_data();
        for ( uint32_t p = 0; p < nsyms; ++p )
            CHECK( get_u32( d + 4 * p, msb ) ==
                   fx.shndx[perm.new_to_old[p]] );
    }

    // Group signature follows the permutation; members are untouched
    if ( fx.group ) {
        auto* sec = after.sections[".group"];
        CHECK( sec != nullptr );
        CHECK( sec->get_info() == perm.old_to_new[fx.group_sig] );
        std::string expdata;
        put_u32( expdata, ELFIO::GRP_COMDAT, msb );
        for ( uint32_t m : fx.group_members )
            put_u32( expdata, m, msb );
        CHECK( std::string( sec->get_data(), sec->get_size() ) == expdata );
    }

    // The other symbol table, its attachments and the shared string table
    // are byte-for-byte identical
    const char* iso[] = { ".dynstr", ".symtab2", ".rela.other",
                          ".hash.other" };
    for ( const char* n : iso ) {
        auto* sb = before.sections[n];
        auto* sa = after.sections[n];
        CHECK( sb != nullptr && sa != nullptr );
        CHECK( sb->get_type() == sa->get_type() );
        CHECK( sb->get_link() == sa->get_link() );
        CHECK( sb->get_info() == sa->get_info() );
        CHECK( sb->get_size() == sa->get_size() );
        if ( sb->get_size() != 0 )
            CHECK( std::memcmp( sb->get_data(), sa->get_data(),
                                sb->get_size() ) == 0 );
    }
}

//------------------------------------------------------------------------------
// Build, corrupt, rebuild, save, reload and verify one fixture
//------------------------------------------------------------------------------
static double run_fixture( unsigned char cls, unsigned char enc,
                           const Fixture& fx_in, bool expect_reorder )
{
    Fixture     fx     = fx_in; // the builder fills group_members
    std::string before = build_object( cls, enc, fx );

    ELFIO::elfio reader;
    {
        std::stringstream ss( before );
        CHECK( reader.load( ss ) );
    }
    auto* sym = reader.sections[".dynsym"];
    auto* gh  = reader.sections[".gnu.hash"];
    CHECK( sym != nullptr && gh != nullptr );

    ELFIO::gnu_hash_rebuilder        rebuilder( reader );
    ELFIO::gnu_hash_rebuild_permutation perm;
    std::string                      error;
    const auto t0 = std::chrono::steady_clock::now();
    bool       ok = rebuilder.rebuild( sym, gh, fx.params, perm, error );
    const auto t1 = std::chrono::steady_clock::now();
    CHECK( ok );
    CHECK( error.empty() );

    if ( expect_reorder ) {
        bool identity = true;
        for ( size_t i = 0; i < perm.new_to_old.size(); ++i )
            if ( perm.new_to_old[i] != i ) {
                identity = false;
                break;
            }
        CHECK( !identity );
    }

    std::stringstream out;
    CHECK( reader.save( out ) );
    verify_result( cls, enc, fx, before, out.str(), perm );

    return std::chrono::duration<double>( t1 - t0 ).count();
}

//------------------------------------------------------------------------------
// Failure-path helpers: a refused rebuild must leave the object byte identical
//------------------------------------------------------------------------------
static void check_refused( ELFIO::elfio&                        reader,
                           const ELFIO::gnu_hash_rebuild_params& params,
                           const char*                           what )
{
    std::stringstream snap;
    CHECK( reader.save( snap ) );

    auto* sym = reader.sections[".dynsym"];
    auto* gh  = reader.sections[".gnu.hash"];
    CHECK( sym != nullptr && gh != nullptr );

    ELFIO::gnu_hash_rebuilder           rebuilder( reader );
    ELFIO::gnu_hash_rebuild_permutation perm;
    std::string                         error;
    CHECK( !rebuilder.rebuild( sym, gh, params, perm, error ) );
    CHECK( !error.empty() );

    std::stringstream after;
    CHECK( reader.save( after ) );
    CHECK( snap.str() == after.str() );
    std::cout << "    refusal holds: " << what << " (\"" << error << "\")"
              << std::endl;
}

static void expect_failure_bytes( const std::string& bytes,
                                  const ELFIO::gnu_hash_rebuild_params& params,
                                  const char*                           what )
{
    ELFIO::elfio reader;
    std::stringstream ss( bytes );
    CHECK( reader.load( ss ) );
    check_refused( reader, params, what );
}

//------------------------------------------------------------------------------
// Named cases
//------------------------------------------------------------------------------
static void case_cross_bucket_reorder( unsigned char cls, unsigned char enc )
{
    const bool is64 = cls == ELFIO::ELFCLASS64;
    Fixture    fx;
    fx.syms = {
        null_sym(),
        { "local_one", 0x40, 4, ELFIO::STB_LOCAL, 0, 0, 1 },
        { "alpha", 0x1000, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "beta", 0x1008, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "gamma", 0x1010, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "delta", 0x1018, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "epsilon", 0x1020, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "zeta", 0x1028, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "eta", 0x1030, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "theta", 0x1038, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
    };
    fx.sh_info            = 2;
    fx.params.symoffset   = 2;
    fx.params.nbuckets    = 4;
    fx.params.bloom_size  = 2;
    fx.params.bloom_shift = 6;
    // Slack beyond the generated content must be zeroed
    fx.hash_capacity = gnu_hash_need( is64, fx.params, 8 ) + 12;
    run_fixture( cls, enc, fx, true );
}

static void case_multi_relocation_refs( unsigned char cls,
                                        unsigned char enc )
{
    const bool is64 = cls == ELFIO::ELFCLASS64;
    Fixture    fx;
    fx.syms = {
        null_sym(),
        { "fn_a", 0x2000, 16, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "fn_b", 0x2010, 16, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "fn_c", 0x2020, 16, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "data_d", 0x3000, 4, ELFIO::STB_GLOBAL, ELFIO::STT_OBJECT, 0, ELFIO::SHN_ABS },
        { "fn_e", 0x2030, 16, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "fn_f", 0x2040, 16, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
    };
    fx.sh_info            = 1;
    fx.params.symoffset   = 1;
    fx.params.nbuckets    = 8;
    fx.params.bloom_size  = 4;
    fx.params.bloom_shift = 6;
    fx.hash_capacity      = gnu_hash_need( is64, fx.params, 6 ) + 8;

    RelocSecSpec rela;
    rela.name = ".rela.dyn";
    rela.rela = true;
    rela.entries = {
        { 0x1000, 1, 8, 0 },   { 0x1008, 2, 1, -16 }, { 0x1010, 1, 8, 4 },
        { 0x1018, 5, 2, 1000 }, { 0x1020, 0, 0, 0 },   { 0x1028, 6, 7, -8 },
    };
    RelocSecSpec rel;
    rel.name = ".rel.dyn";
    rel.rela = false;
    rel.entries = {
        { 0x2000, 3, 9, 0 },
        { 0x2004, 4, 10, 0 },
        { 0x2008, 3, 9, 0 },
        { 0x200C, 6, 11, 0 },
    };
    fx.relocs = { rela, rel };
    run_fixture( cls, enc, fx, false );
}

static void case_versym_and_shndx( unsigned char cls, unsigned char enc )
{
    const bool is64 = cls == ELFIO::ELFCLASS64;
    Fixture    fx;
    fx.syms = {
        null_sym(),
        { "sym_a", 0x10, 1, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "sym_b", 0x20, 2, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_XINDEX },
        { "sym_c", 0x30, 3, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "sym_d", 0x40, 4, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_XINDEX },
        { "sym_e", 0x50, 5, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
    };
    fx.sh_info            = 1;
    fx.params.symoffset   = 1;
    fx.params.nbuckets    = 4;
    fx.params.bloom_size  = 2;
    fx.params.bloom_shift = 5;
    fx.hash_capacity      = gnu_hash_need( is64, fx.params, 5 ) + 4;
    // Hidden bit patterns (0x8000) must survive the move uninterpreted
    fx.versym = { 0, 1, 2, 0x8002, 3, 0x8001 };
    fx.shndx  = { 0, 0, 7, 0, 9, 0 };
    run_fixture( cls, enc, fx, false );
}

static void case_duplicate_names( unsigned char cls, unsigned char enc )
{
    const bool is64 = cls == ELFIO::ELFCLASS64;
    Fixture    fx;
    fx.syms = {
        null_sym(),
        { "dup", 0x1111, 4, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "solo", 0x3333, 4, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "dup", 0x2222, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "tail", 0x4444, 2, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
    };
    fx.sh_info            = 1;
    fx.params.symoffset   = 1;
    fx.params.nbuckets    = 4;
    fx.params.bloom_size  = 2;
    fx.params.bloom_shift = 6;
    fx.hash_capacity      = gnu_hash_need( is64, fx.params, 4 ) + 20;
    run_fixture( cls, enc, fx, false );
}

static void case_hash_collision_no_merge( unsigned char cls,
                                          unsigned char enc )
{
    const bool is64 = cls == ELFIO::ELFCLASS64;
    // Deterministically find two distinct names with the same 32-bit GNU hash
    std::string c1, c2;
    {
        std::unordered_map<uint32_t, std::string> seen;
        for ( uint32_t i = 0;; ++i ) {
            std::string n = "hx" + std::to_string( i );
            uint32_t    h = ref_gnu_hash( n );
            auto        it = seen.find( h );
            if ( it != seen.end() && it->second != n ) {
                c1 = it->second;
                c2 = n;
                break;
            }
            seen[h] = n;
        }
    }
    Fixture fx;
    fx.syms = {
        null_sym(),
        { c1, 0xAAAA, 4, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "mid", 0x1000, 4, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { c2, 0xBBBB, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "end", 0x2000, 4, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
    };
    fx.sh_info            = 1;
    fx.params.symoffset   = 1;
    fx.params.nbuckets    = 4;
    fx.params.bloom_size  = 2;
    fx.params.bloom_shift = 6;
    fx.hash_capacity      = gnu_hash_need( is64, fx.params, 4 );
    run_fixture( cls, enc, fx, false );
}

static void case_empty_buckets( unsigned char cls, unsigned char enc )
{
    const bool is64 = cls == ELFIO::ELFCLASS64;
    Fixture    fx;
    fx.syms = {
        null_sym(),
        { "one", 0x1, 1, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "two", 0x2, 2, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "three", 0x3, 3, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
    };
    fx.sh_info            = 1;
    fx.params.symoffset   = 1;
    fx.params.nbuckets    = 16; // most buckets stay empty
    fx.params.bloom_size  = 1;
    fx.params.bloom_shift = 5;
    fx.hash_capacity      = gnu_hash_need( is64, fx.params, 3 ); // exact fit
    run_fixture( cls, enc, fx, false );
}

static void case_other_symtab_isolation( unsigned char cls,
                                         unsigned char enc )
{
    const bool is64 = cls == ELFIO::ELFCLASS64;
    Fixture    fx;
    fx.syms = {
        null_sym(),
        { "shared_fn", 0x5000, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "uniq_a", 0x5008, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "uniq_b", 0x5010, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "uniq_c", 0x5018, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
    };
    fx.sh_info            = 1;
    fx.params.symoffset   = 1;
    fx.params.nbuckets    = 4;
    fx.params.bloom_size  = 2;
    fx.params.bloom_shift = 6;
    fx.hash_capacity      = gnu_hash_need( is64, fx.params, 4 ) + 8;
    // The second symbol table reuses "shared_fn" from the same .dynstr;
    // verify_result checks it all stays byte-for-byte identical.
    run_fixture( cls, enc, fx, false );
}

static void case_group_signature( unsigned char cls, unsigned char enc )
{
    const bool is64 = cls == ELFIO::ELFCLASS64;
    Fixture    fx;
    fx.syms = {
        null_sym(),
        { "alpha", 0x1000, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "beta", 0x1008, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "gamma", 0x1010, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "delta", 0x1018, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "epsilon", 0x1020, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
    };
    fx.sh_info            = 1;
    fx.params.symoffset   = 1;
    fx.params.nbuckets    = 4;
    fx.params.bloom_size  = 2;
    fx.params.bloom_shift = 6;
    fx.hash_capacity      = gnu_hash_need( is64, fx.params, 5 ) + 8;
    fx.group              = true;
    fx.group_sig          = 3; // "gamma"
    run_fixture( cls, enc, fx, false );
}

static void case_failure_no_modify( unsigned char cls, unsigned char enc )
{
    const bool is64 = cls == ELFIO::ELFCLASS64;
    const bool msb  = enc == ELFIO::ELFDATA2MSB;

    Fixture fx;
    fx.syms = {
        null_sym(),
        { "loc", 0x40, 4, ELFIO::STB_LOCAL, 0, 0, 1 },
        { "fa", 0x1000, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "fb", 0x1008, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "fc", 0x1010, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
        { "fd", 0x1018, 8, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, ELFIO::SHN_ABS },
    };
    fx.sh_info            = 2;
    fx.params.symoffset   = 2;
    fx.params.nbuckets    = 4;
    fx.params.bloom_size  = 2;
    fx.params.bloom_shift = 6;
    fx.hash_capacity      = gnu_hash_need( is64, fx.params, 4 ) + 16;
    RelocSecSpec rela;
    rela.name = ".rela.dyn";
    rela.rela = true;
    rela.entries = {
        { 0x100, 2, 1, 0 },
        { 0x108, 3, 2, 4 },
        { 0x110, 4, 3, 0xFFFFFFFC },
    };
    RelocSecSpec rel;
    rel.name = ".rel.dyn";
    rel.rela = false;
    rel.entries = { { 0x200, 5, 9, 0 } };
    fx.relocs = { rela, rel };
    fx.versym = { 0, 0, 1, 2, 3, 4 };

    const uint32_t nsyms = (uint32_t)fx.syms.size();
    std::string    base  = build_object( cls, enc, fx );

    // (a) capacity too small for the requested layout
    {
        auto p       = fx.params;
        p.nbuckets   = 0x40000;
        expect_failure_bytes( base, p, "insufficient capacity" );
    }
    // (b) symoffset cuts into the local prefix
    {
        auto p       = fx.params;
        p.symoffset  = 1;
        expect_failure_bytes( base, p, "symoffset cuts local prefix" );
    }
    // (c) invalid layout parameters
    {
        auto p      = fx.params;
        p.nbuckets  = 0;
        expect_failure_bytes( base, p, "zero nbuckets" );
    }
    {
        auto p       = fx.params;
        p.bloom_size = 0;
        expect_failure_bytes( base, p, "zero bloom word count" );
    }
    {
        auto p        = fx.params;
        p.bloom_shift = 32;
        expect_failure_bytes( base, p, "bloom shift out of range" );
    }
    // (d) symbol name not null-terminated
    {
        ELFIO::elfio r;
        {
            std::stringstream ss( base );
            CHECK( r.load( ss ) );
        }
        auto* sym = r.sections[".dynsym"];
        auto* str = r.sections[".dynstr"];
        std::string sd( str->get_data(), str->get_size() );
        CHECK( !sd.empty() );
        sd[sd.size() - 1] = 'Z'; // no NUL at the end of the string table
        str->set_data( sd );
        std::string yd( sym->get_data(), sym->get_size() );
        size_t es = (size_t)sym->get_entry_size();
        put_u32_at( yd, 2 * es, (uint32_t)sd.size() - 1, msb );
        sym->set_data( yd );
        check_refused( r, fx.params, "unterminated symbol name" );
    }
    // (e) relocation references a symbol index out of range
    {
        ELFIO::elfio r;
        {
            std::stringstream ss( base );
            CHECK( r.load( ss ) );
        }
        auto*       relsec = r.sections[".rela.dyn"];
        std::string rd( relsec->get_data(), relsec->get_size() );
        if ( cls == ELFIO::ELFCLASS64 ) {
            uint64_t info = get_u64( rd.data() + 8, msb );
            info = ( uint64_t( nsyms + 40 ) << 32 ) | ( info & 0xFFFFFFFFu );
            put_u64_at( rd, 8, info, msb );
        }
        else {
            uint32_t info = get_u32( rd.data() + 4, msb );
            info = ( ( nsyms + 40 ) << 8 ) | ( info & 0xFFu );
            put_u32_at( rd, 4, info, msb );
        }
        relsec->set_data( rd );
        check_refused( r, fx.params, "relocation symbol index out of range" );
    }
    // (f) dynsym size not a multiple of its entry size
    {
        ELFIO::elfio r;
        {
            std::stringstream ss( base );
            CHECK( r.load( ss ) );
        }
        r.sections[".dynsym"]->append_data( "Z", 1 );
        check_refused( r, fx.params, "incomplete symbol entry" );
    }
    // (g) a SysV hash section is also linked to the target dynsym
    {
        Fixture fx2                = fx;
        fx2.sysv_hash_on_target    = true;
        std::string with_sysv      = build_object( cls, enc, fx2 );
        expect_failure_bytes( with_sysv, fx.params,
                              "stale SysV hash attachment" );
    }
    // (h) versym section length does not match the symbol count
    {
        ELFIO::elfio r;
        {
            std::stringstream ss( base );
            CHECK( r.load( ss ) );
        }
        auto*       vs = r.sections[".gnu.version"];
        std::string vd( vs->get_data(), vs->get_size() );
        vd.resize( vd.size() - 2 );
        vs->set_data( vd );
        check_refused( r, fx.params, "versym length mismatch" );
    }
}

static void case_large_linear_scan( unsigned char cls, unsigned char enc )
{
    const bool     is64   = cls == ELFIO::ELFCLASS64;
    const uint32_t nglob  = 25000;
    Fixture        fx;
    fx.syms.reserve( nglob + 1 );
    fx.syms.push_back( null_sym() );
    for ( uint32_t i = 0; i < nglob; ++i ) {
        fx.syms.push_back( { "g" + std::to_string( i ), 0x1000 + 8 * i, 8,
                             ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0,
                             ELFIO::SHN_ABS } );
    }
    fx.sh_info            = 1;
    fx.params.symoffset   = 1;
    fx.params.nbuckets    = 4096;
    fx.params.bloom_size  = 64;
    fx.params.bloom_shift = 6;
    fx.hash_capacity      = gnu_hash_need( is64, fx.params, nglob ) + 40;

    // Deterministic pseudo-random referents, several per symbol on average
    uint32_t st   = 12345;
    auto     next = [&]() {
        st = st * 1664525u + 1013904223u;
        return st >> 8;
    };
    RelocSecSpec rela1, rela2, rel1;
    rela1.name = ".rela.1";
    rela1.rela = true;
    rela2.name = ".rela.2";
    rela2.rela = true;
    rel1.name  = ".rel.1";
    rel1.rela  = false;
    for ( uint32_t e = 0; e < 30000; ++e ) {
        rela1.entries.push_back(
            { 8ull * e, 1 + next() % nglob, 1 + e % 4, (int64_t)e - 15000 } );
    }
    for ( uint32_t e = 0; e < 30000; ++e ) {
        rela2.entries.push_back(
            { 8ull * e, 1 + next() % nglob, 2 + e % 3, -(int64_t)e } );
    }
    for ( uint32_t e = 0; e < 15000; ++e ) {
        rel1.entries.push_back( { 4ull * e, 1 + next() % nglob, 9, 0 } );
    }
    fx.relocs = { rela1, rela2, rel1 };
    fx.versym.reserve( nglob + 1 );
    for ( uint32_t i = 0; i <= nglob; ++i )
        fx.versym.push_back( (uint16_t)( i % 4 == 0 ? 0x8002 : i % 5 ) );

    // A quadratic "swap then rescan every attachment" implementation would
    // need ~25000 x 75000 entry visits here; the linear one is instant.
    double elapsed = run_fixture( cls, enc, fx, true );
    std::cout << "    large sample rebuilt in " << elapsed << "s (class "
              << int( cls ) << ", endian " << int( enc ) << ")" << std::endl;
    CHECK( elapsed < 10.0 );
}

//------------------------------------------------------------------------------
int main()
{
    const unsigned char classes[] = { ELFIO::ELFCLASS32, ELFIO::ELFCLASS64 };
    const unsigned char encs[]    = { ELFIO::ELFDATA2LSB, ELFIO::ELFDATA2MSB };
    struct CaseDef
    {
        const char* name;
        void ( *fn )( unsigned char, unsigned char );
    };
    const CaseDef cases[] = {
        { "cross_bucket_reorder", case_cross_bucket_reorder },
        { "multi_relocation_refs", case_multi_relocation_refs },
        { "versym_and_shndx", case_versym_and_shndx },
        { "duplicate_names", case_duplicate_names },
        { "hash_collision_no_merge", case_hash_collision_no_merge },
        { "empty_buckets", case_empty_buckets },
        { "other_symtab_isolation", case_other_symtab_isolation },
        { "group_signature", case_group_signature },
        { "failure_no_modify", case_failure_no_modify },
        { "large_linear_scan", case_large_linear_scan },
    };

    for ( const auto& c : cases ) {
        for ( unsigned char cls : classes )
            for ( unsigned char enc : encs )
                c.fn( cls, enc );
        std::cout << "CASE_OK " << c.name << std::endl;
    }
    std::cout << "ALL_CONTRACT_CASES_PASSED" << std::endl;
    return 0;
}
