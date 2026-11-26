#ifndef HEADER_PARSER_MACHO_REFERENCES_H
#define HEADER_PARSER_MACHO_REFERENCES_H

/*
 * Mach-O indirect symbol reference resolution.
 *
 * Connects S_NON_LAZY_SYMBOL_POINTERS / S_LAZY_SYMBOL_POINTERS pointer slots
 * and S_SYMBOL_STUBS stubs to their indirect symbol table entry, nlist symbol
 * and two-level namespace library.
 *
 * The resolver performs its own second, order independent scan of the load
 * commands and uses positioned file reads, so symbol/indirect/string tables
 * may live far away from the load command area.  Every returned string is
 * copied into malloc storage owned by HeaderData.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../HeaderData.h"
#include "../utils/common_fileio.h"
#include "MachOFileHeader.h"

#ifndef MR_INDIRECT_SYMBOL_LOCAL
#define MR_INDIRECT_SYMBOL_LOCAL 0x80000000u
#endif
#ifndef MR_INDIRECT_SYMBOL_ABS
#define MR_INDIRECT_SYMBOL_ABS   0x40000000u
#endif

/* Loading class dylib commands that occupy library ordinals. */
#ifndef LC_LOAD_WEAK_DYLIB
#define LC_LOAD_WEAK_DYLIB       0x80000018u
#endif
#ifndef LC_REEXPORT_DYLIB
#define LC_REEXPORT_DYLIB        0x8000001fu
#endif
#ifndef LC_LOAD_UPWARD_DYLIB
#define LC_LOAD_UPWARD_DYLIB     0x80000023u
#endif

typedef struct MR_Reader
{
    FILE* fp;
    uint64_t base;      /* whole file byte offset of the image start */
    uint64_t file_size; /* whole file size in bytes */
    int big_endian;
} MR_Reader;

typedef struct MR_Dylib
{
    uint32_t cmd;
    char* path; /* may be NULL if the command name is not readable */
} MR_Dylib;

typedef struct MR_Section
{
    uint32_t segment_index;
    uint32_t section_index;
    uint32_t section_in_segment;
    char segname[17];
    char sectname[17];
    uint8_t type;
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t reserved1;
    uint32_t reserved2;
} MR_Section;

static int MR_readBytes(const MR_Reader* r, uint64_t image_offset, void* out, size_t n)
{
    uint64_t abs_offset;

    if ( n == 0 )
        return 1;
    if ( image_offset > UINT64_MAX - (uint64_t)n )
        return 0;
    if ( r->base > UINT64_MAX - image_offset )
        return 0;

    abs_offset = r->base + image_offset;
    if ( abs_offset > r->file_size || (uint64_t)n > r->file_size - abs_offset )
        return 0;

    return readFile(r->fp, (size_t)abs_offset, n, (uint8_t*)out) == n;
}

static int MR_rangeReadable(const MR_Reader* r, uint64_t image_offset, uint64_t n)
{
    unsigned char scratch[1];

    if ( n == 0 )
        return 1;
    if ( image_offset > UINT64_MAX - n )
        return 0;

    /* MR_readBytes already performs the complete bounds check. */
    return MR_readBytes(r, image_offset, scratch, 1) &&
           MR_readBytes(r, image_offset + n - 1, scratch, 1);
}

static uint32_t MR_getU32(const unsigned char* p, int big_endian)
{
    if ( big_endian )
    {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
    }
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t MR_getU16(const unsigned char* p, int big_endian)
{
    if ( big_endian )
        return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint64_t MR_getU64(const unsigned char* p, int big_endian)
{
    if ( big_endian )
    {
        return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
               ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
               ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
               ((uint64_t)p[6] << 8)  |  (uint64_t)p[7];
    }
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static int MR_readU32(const MR_Reader* r, uint64_t off, uint32_t* value)
{
    unsigned char b[4];
    if ( !MR_readBytes(r, off, b, sizeof(b)) )
        return 0;
    *value = MR_getU32(b, r->big_endian);
    return 1;
}

/* Copy a fixed 16 byte Mach-O name into a NUL terminated owned string. */
static char* MR_copyFixedName(const unsigned char* p)
{
    size_t n = strnlen((const char*)p, MACH_O_SEG_NAME_LN);
    char* s = (char*)malloc(n + 1);
    if ( !s )
        return NULL;
    memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

/*
 * Read a NUL terminated string starting at image offset off.
 * At most max_len bytes belong to the enclosing table/command and the read
 * is additionally bounded by the file size.  Returns an owned copy or NULL
 * when the bounds are violated or no NUL terminator exists.
 */
static char* MR_readCString(const MR_Reader* r, uint64_t off, uint64_t max_len)
{
    uint64_t limit = max_len;
    uint64_t n = 0;
    unsigned char c;
    char* s;

    if ( off >= r->file_size )
        return NULL;
    if ( limit > r->file_size - off )
        limit = r->file_size - off;

    while ( n < limit )
    {
        if ( !MR_readBytes(r, off + n, &c, 1) )
            return NULL;
        if ( c == '\0' )
        {
            s = (char*)malloc((size_t)n + 1);
            if ( !s )
                return NULL;
            if ( n && !MR_readBytes(r, off, s, (size_t)n) )
            {
                free(s);
                return NULL;
            }
            s[n] = '\0';
            return s;
        }
        ++n;
    }

    /* The enclosing range ended without a NUL terminator. */
    return NULL;
}

static int MR_isLoadingDylibCommand(uint32_t cmd)
{
    return cmd == LC_LOAD_DYLIB ||
           cmd == LC_LOAD_WEAK_DYLIB ||
           cmd == LC_REEXPORT_DYLIB ||
           cmd == LC_LOAD_UPWARD_DYLIB;
}

static void MR_freeDylibs(MR_Dylib* dylibs, size_t n)
{
    size_t i;
    if ( !dylibs )
        return;
    for ( i = 0; i < n; ++i )
        free(dylibs[i].path);
    free(dylibs);
}

/*
 * Scan all load commands once and collect the supported sections, the
 * symtab/dysymtab locations and the loading dylib commands in load order.
 * Returns 0 on success and a MachOReferencesError code on a fatal scan
 * problem.  Absence of supported sections is not an error.
 */
static uint8_t MR_collect(const MR_Reader* r,
                          uint8_t bitness,
                          MR_Section** sections_out,
                          size_t* sections_count_out,
                          MR_Dylib** dylibs_out,
                          size_t* dylibs_count_out,
                          uint32_t* symoff, uint32_t* nsyms,
                          uint32_t* stroff, uint32_t* strsize,
                          int* have_symtab,
                          uint32_t* indirectoff, uint32_t* nindirect,
                          int* have_dysymtab)
{
    uint32_t magic_ncmds = 0;
    uint32_t ncmds = 0;
    uint64_t off;
    uint64_t header_size = (bitness == 64) ? SIZE_OF_MACHO_O_HEADER_64
                                           : SIZE_OF_MACHO_O_HEADER;
    uint32_t segment_index = 0;
    uint32_t global_section_index = 0;
    MR_Section* sections = NULL;
    size_t sections_count = 0;
    MR_Dylib* dylibs = NULL;
    size_t dylibs_count = 0;
    size_t dylibs_capacity = 0;
    uint8_t err = MACHO_REFERENCES_OK;
    uint32_t i;

    *sections_out = NULL;
    *sections_count_out = 0;
    *dylibs_out = NULL;
    *dylibs_count_out = 0;
    *have_symtab = 0;
    *have_dysymtab = 0;

    if ( !MR_readU32(r, 16, &magic_ncmds) )
        return MACHO_REFERENCES_ERR_READ;
    ncmds = magic_ncmds;

    off = header_size;
    for ( i = 0; i < ncmds; ++i )
    {
        unsigned char lc_bytes[8];
        uint32_t cmd, cmdsize;

        if ( !MR_readBytes(r, off, lc_bytes, sizeof(lc_bytes)) )
            break; /* command area ends; original parser reports this too */

        cmd = MR_getU32(lc_bytes, r->big_endian);
        cmdsize = MR_getU32(lc_bytes + 4, r->big_endian);
        if ( cmdsize < 8 )
            break;
        if ( off > UINT64_MAX - cmdsize )
            break;

        if ( cmd == LC_SEGMENT || cmd == LC_SEGMENT_64 )
        {
            int is64 = (cmd == LC_SEGMENT_64);
            uint64_t seg_header_size = is64 ? SIZE_OF_MACHO_O_SEGMENT_HEADER_64
                                            : SIZE_OF_MACHO_O_SEGMENT_HEADER_32;
            uint64_t sect_size = is64 ? SIZE_OF_MACHO_O_SECTION_HEADER_64
                                      : SIZE_OF_MACHO_O_SECTION_HEADER_32;
            uint32_t nsects_offset = is64 ? 64 : 48;
            uint32_t nsects = 0;
            uint32_t j;
            uint32_t section_in_segment = 0;

            if ( MR_readU32(r, off + nsects_offset, &nsects) )
            {
                ++segment_index;
                for ( j = 0; j < nsects; ++j )
                {
                    uint64_t sec_off = off + seg_header_size + (uint64_t)j * sect_size;
                    unsigned char sb[80];
                    MR_Section sec;
                    uint32_t flags;

                    ++global_section_index;
                    ++section_in_segment;

                    if ( sect_size > sizeof(sb) ||
                         !MR_readBytes(r, sec_off, sb, (size_t)sect_size) )
                    {
                        err = MACHO_REFERENCES_ERR_READ;
                        goto fail;
                    }

                    memset(&sec, 0, sizeof(sec));
                    sec.segment_index = segment_index;
                    sec.section_index = global_section_index;
                    sec.section_in_segment = section_in_segment;
                    memcpy(sec.sectname, sb, MACH_O_SEG_NAME_LN);
                    sec.sectname[16] = '\0';
                    memcpy(sec.segname, sb + MACH_O_SEG_NAME_LN, MACH_O_SEG_NAME_LN);
                    sec.segname[16] = '\0';

                    if ( is64 )
                    {
                        sec.addr = MR_getU64(sb + 32, r->big_endian);
                        sec.size = MR_getU64(sb + 40, r->big_endian);
                        sec.offset = MR_getU32(sb + 48, r->big_endian);
                        flags = MR_getU32(sb + 64, r->big_endian);
                        sec.reserved1 = MR_getU32(sb + 68, r->big_endian);
                        sec.reserved2 = MR_getU32(sb + 72, r->big_endian);
                    }
                    else
                    {
                        sec.addr = MR_getU32(sb + 32, r->big_endian);
                        sec.size = MR_getU32(sb + 36, r->big_endian);
                        sec.offset = MR_getU32(sb + 40, r->big_endian);
                        flags = MR_getU32(sb + 56, r->big_endian);
                        sec.reserved1 = MR_getU32(sb + 60, r->big_endian);
                        sec.reserved2 = MR_getU32(sb + 64, r->big_endian);
                    }
                    sec.type = (uint8_t)(flags & 0xffu);

                    if ( sec.type == S_NON_LAZY_SYMBOL_POINTERS ||
                         sec.type == S_LAZY_SYMBOL_POINTERS ||
                         sec.type == S_SYMBOL_STUBS )
                    {
                        MR_Section* grown = (MR_Section*)realloc(
                            sections, (sections_count + 1) * sizeof(MR_Section));
                        if ( !grown )
                        {
                            err = MACHO_REFERENCES_ERR_READ;
                            goto fail;
                        }
                        sections = grown;
                        sections[sections_count++] = sec;
                    }
                }
            }
        }
        else if ( cmd == LC_SYMTAB && cmdsize >= SIZE_OF_MACHO_O_SYMTAB_COMMAND )
        {
            uint32_t v[4];
            if ( MR_readU32(r, off + 8, &v[0]) &&
                 MR_readU32(r, off + 12, &v[1]) &&
                 MR_readU32(r, off + 16, &v[2]) &&
                 MR_readU32(r, off + 20, &v[3]) )
            {
                *symoff = v[0];
                *nsyms = v[1];
                *stroff = v[2];
                *strsize = v[3];
                *have_symtab = 1;
            }
        }
        else if ( cmd == LC_DYSYMTAB && cmdsize >= SIZE_OF_MACHO_O_DY_SYMTAB_COMMAND )
        {
            if ( MR_readU32(r, off + 0x38, indirectoff) &&
                 MR_readU32(r, off + 0x3c, nindirect) )
            {
                *have_dysymtab = 1;
            }
        }
        else if ( MR_isLoadingDylibCommand(cmd) &&
                  cmdsize >= SIZE_OF_MACHO_O_DYLIB_COMMAND )
        {
            uint32_t name_offset = 0;
            MR_Dylib* grown;
            char* path;

            (void)MR_readU32(r, off + 8, &name_offset);
            if ( name_offset < 8 || name_offset > cmdsize )
                name_offset = SIZE_OF_MACHO_O_DYLIB_COMMAND;

            path = MR_readCString(r, off + name_offset,
                                  (uint64_t)cmdsize - name_offset);

            if ( dylibs_count == dylibs_capacity )
            {
                size_t new_capacity = dylibs_capacity ? dylibs_capacity * 2 : 4;
                grown = (MR_Dylib*)realloc(dylibs, new_capacity * sizeof(MR_Dylib));
                if ( !grown )
                {
                    free(path);
                    err = MACHO_REFERENCES_ERR_READ;
                    goto fail;
                }
                dylibs = grown;
                dylibs_capacity = new_capacity;
            }
            dylibs[dylibs_count].cmd = cmd;
            dylibs[dylibs_count].path = path; /* counted even if path is NULL */
            ++dylibs_count;
        }

        off += cmdsize;
    }

    *sections_out = sections;
    *sections_count_out = sections_count;
    *dylibs_out = dylibs;
    *dylibs_count_out = dylibs_count;
    return MACHO_REFERENCES_OK;

fail:
    MR_freeDylibs(dylibs, dylibs_count);
    free(sections);
    return err;
}

/*
 * Resolve all indirect symbol relations into hd->macho_references.
 * Never closes fp and never lets file lifetime escape: all strings are
 * copied.  fp must remain open for the duration of this call.
 */
static void MachO_resolveReferences(PHeaderData hd,
                                    FILE* fp,
                                    size_t start_offset,
                                    size_t whole_file_size)
{
    MR_Reader r;
    MR_Section* sections = NULL;
    size_t sections_count = 0;
    MR_Dylib* dylibs = NULL;
    size_t dylibs_count = 0;
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    uint32_t indirectoff = 0, nindirect = 0;
    int have_symtab = 0, have_dysymtab = 0;
    uint8_t err;
    size_t s;
    uint64_t total_slots = 0;
    size_t out_index = 0;

    hd->macho_references = NULL;
    hd->macho_references_size = 0;
    hd->macho_references_error = MACHO_REFERENCES_OK;

    if ( !hd || !fp || (hd->h_bitness != 32 && hd->h_bitness != 64) )
        return;

    r.fp = fp;
    r.base = (uint64_t)start_offset;
    r.file_size = (uint64_t)whole_file_size;
    r.big_endian = (hd->endian == ENDIAN_BIG);

    err = MR_collect(&r, hd->h_bitness,
                     &sections, &sections_count,
                     &dylibs, &dylibs_count,
                     &symoff, &nsyms, &stroff, &strsize, &have_symtab,
                     &indirectoff, &nindirect, &have_dysymtab);
    if ( err != MACHO_REFERENCES_OK )
    {
        hd->macho_references_error = err;
        return;
    }

    if ( sections_count == 0 )
    {
        MR_freeDylibs(dylibs, dylibs_count);
        free(sections);
        return; /* empty array */
    }

    if ( !have_dysymtab )
    {
        hd->macho_references_error = MACHO_REFERENCES_ERR_MISSING_DYSYMTAB;
        goto cleanup;
    }

    /* Geometry and table coverage validation.  A defect invalidates the
     * whole image result rather than returning a partial relation table. */
    for ( s = 0; s < sections_count; ++s )
    {
        MR_Section* sec = &sections[s];
        uint64_t slot_width;
        uint64_t slots;
        uint64_t indirect_end;

        if ( sec->type == S_SYMBOL_STUBS )
        {
            if ( sec->reserved2 == 0 )
            {
                hd->macho_references_error = MACHO_REFERENCES_ERR_STUB_WIDTH;
                goto cleanup;
            }
            slot_width = sec->reserved2;
        }
        else
        {
            slot_width = (hd->h_bitness == 64) ? 8 : 4;
        }

        if ( slot_width == 0 || (sec->size % slot_width) != 0 )
        {
            hd->macho_references_error = MACHO_REFERENCES_ERR_SLOT_ALIGNMENT;
            goto cleanup;
        }
        slots = sec->size / slot_width;

        if ( sec->reserved1 > UINT64_MAX - slots )
        {
            hd->macho_references_error = MACHO_REFERENCES_ERR_INDIRECT_TABLE;
            goto cleanup;
        }
        indirect_end = (uint64_t)sec->reserved1 + slots;
        if ( indirect_end > nindirect )
        {
            hd->macho_references_error = MACHO_REFERENCES_ERR_INDIRECT_TABLE;
            goto cleanup;
        }
        if ( slots != 0 )
        {
            uint64_t table_bytes = slots * 4;
            uint64_t table_start = (uint64_t)indirectoff +
                                   (uint64_t)sec->reserved1 * 4;
            if ( !MR_rangeReadable(&r, table_start, table_bytes) )
            {
                hd->macho_references_error = MACHO_REFERENCES_ERR_INDIRECT_TABLE;
                goto cleanup;
            }
        }

        if ( total_slots > UINT64_MAX - slots )
        {
            hd->macho_references_error = MACHO_REFERENCES_ERR_INDIRECT_TABLE;
            goto cleanup;
        }
        total_slots += slots;
    }

    if ( total_slots == 0 )
    {
        MR_freeDylibs(dylibs, dylibs_count);
        free(sections);
        return;
    }
    if ( total_slots > (uint64_t)SIZE_MAX / sizeof(MachOReferenceData) )
    {
        hd->macho_references_error = MACHO_REFERENCES_ERR_INDIRECT_TABLE;
        goto cleanup;
    }

    hd->macho_references = (MachOReferenceData*)calloc((size_t)total_slots,
                                                       sizeof(MachOReferenceData));
    if ( !hd->macho_references )
    {
        hd->macho_references_error = MACHO_REFERENCES_ERR_READ;
        goto cleanup;
    }

    for ( s = 0; s < sections_count; ++s )
    {
        MR_Section* sec = &sections[s];
        uint64_t slot_width = (sec->type == S_SYMBOL_STUBS)
                                  ? sec->reserved2
                                  : ((hd->h_bitness == 64) ? 8 : 4);
        uint64_t slots = sec->size / slot_width;
        uint64_t j;

        for ( j = 0; j < slots; ++j )
        {
            MachOReferenceData* ref = &hd->macho_references[out_index++];
            uint32_t indirect_index = sec->reserved1 + (uint32_t)j;
            uint32_t indirect_value = 0;
            uint32_t special;

            ref->library_ordinal = -1;
            ref->segment_index = sec->segment_index;
            ref->section_index = sec->section_index;
            ref->section_index_in_segment = sec->section_in_segment;
            ref->slot_index = (uint32_t)j;
            ref->section_type = sec->type;
            ref->segment_name = MR_copyFixedName((const unsigned char*)sec->segname);
            ref->section_name = MR_copyFixedName((const unsigned char*)sec->sectname);

            ref->address = sec->addr + j * slot_width;
            ref->image_offset = (uint64_t)sec->offset + j * slot_width;
            ref->file_offset = r.base + ref->image_offset;

            ref->indirect_index = indirect_index;
            if ( !MR_readU32(&r, (uint64_t)indirectoff +
                                      (uint64_t)indirect_index * 4,
                              &indirect_value) )
            {
                /* Coverage was validated above; keep the slot regardless. */
                ref->status = MACHO_REFERENCE_UNRESOLVED;
                continue;
            }
            ref->indirect_value = indirect_value;

            special = indirect_value & (MR_INDIRECT_SYMBOL_LOCAL | MR_INDIRECT_SYMBOL_ABS);
            if ( special == MR_INDIRECT_SYMBOL_LOCAL )
            {
                ref->status = MACHO_REFERENCE_LOCAL;
                continue;
            }
            if ( special == MR_INDIRECT_SYMBOL_ABS )
            {
                ref->status = MACHO_REFERENCE_ABS;
                continue;
            }
            if ( special == (MR_INDIRECT_SYMBOL_LOCAL | MR_INDIRECT_SYMBOL_ABS) )
            {
                ref->status = MACHO_REFERENCE_LOCAL_ABS;
                continue;
            }

            /* Ordinary nlist index. */
            ref->nlist_index = indirect_value;
            ref->status = MACHO_REFERENCE_RESOLVED;

            {
                uint64_t nlist_size = (hd->h_bitness == 64) ? 16 : 12;
                uint64_t nlist_off;
                unsigned char nb[16];
                uint32_t strx = 0;

                if ( !have_symtab || indirect_value >= nsyms )
                {
                    ref->status = MACHO_REFERENCE_UNRESOLVED;
                    continue;
                }
                nlist_off = (uint64_t)symoff +
                            (uint64_t)indirect_value * nlist_size;
                if ( !MR_rangeReadable(&r, nlist_off, nlist_size) ||
                     !MR_readBytes(&r, nlist_off, nb, (size_t)nlist_size) )
                {
                    ref->status = MACHO_REFERENCE_UNRESOLVED;
                    continue;
                }

                strx = MR_getU32(nb, r.big_endian);
                ref->n_type = nb[4];
                ref->n_sect = nb[5];
                ref->n_desc = MR_getU16(nb + 6, r.big_endian);
                if ( hd->h_bitness == 64 )
                    ref->n_value = MR_getU64(nb + 8, r.big_endian);
                else
                    ref->n_value = MR_getU32(nb + 8, r.big_endian);

                if ( strx < strsize )
                {
                    ref->symbol_name = MR_readCString(
                        &r, (uint64_t)stroff + strx,
                        (uint64_t)strsize - strx);
                }
                if ( !ref->symbol_name )
                    ref->status = MACHO_REFERENCE_UNRESOLVED;

                /* Two-level namespace library ordinal lives in n_desc[15:8]. */
                {
                    uint32_t ordinal = ((uint32_t)ref->n_desc >> 8) & 0xffu;
                    ref->library_ordinal = (int32_t)ordinal;

                    if ( ordinal == MACHO_LIBRARY_ORDINAL_SELF_VALUE )
                    {
                        ref->library_kind = MACHO_LIBRARY_ORDINAL_SELF;
                    }
                    else if ( ordinal == MACHO_LIBRARY_ORDINAL_DYNAMIC_LOOKUP_VALUE )
                    {
                        ref->library_kind = MACHO_LIBRARY_ORDINAL_DYNAMIC_LOOKUP;
                    }
                    else if ( ordinal == MACHO_LIBRARY_ORDINAL_EXECUTABLE_VALUE )
                    {
                        ref->library_kind = MACHO_LIBRARY_ORDINAL_EXECUTABLE;
                    }
                    else if ( ordinal >= 1 && ordinal <= 0xfd )
                    {
                        if ( ordinal <= dylibs_count )
                        {
                            ref->library_kind = MACHO_LIBRARY_ORDINAL_NORMAL;
                            if ( dylibs[ordinal - 1].path )
                            {
                                ref->library_path = (char*)malloc(
                                    strlen(dylibs[ordinal - 1].path) + 1);
                                if ( ref->library_path )
                                    strcpy(ref->library_path,
                                           dylibs[ordinal - 1].path);
                                else
                                    ref->status = MACHO_REFERENCE_UNRESOLVED;
                            }
                            else
                            {
                                ref->status = MACHO_REFERENCE_UNRESOLVED;
                            }
                        }
                        else
                        {
                            ref->library_kind = MACHO_LIBRARY_ORDINAL_INVALID;
                            ref->status = MACHO_REFERENCE_UNRESOLVED;
                        }
                    }
                    else
                    {
                        /* Unreachable: u8 values are all covered above. */
                        ref->library_kind = MACHO_LIBRARY_ORDINAL_INVALID;
                        ref->status = MACHO_REFERENCE_UNRESOLVED;
                    }
                }
            }
        }
    }

    hd->macho_references_size = (size_t)total_slots;

    MR_freeDylibs(dylibs, dylibs_count);
    free(sections);
    return;

cleanup:
    MR_freeDylibs(dylibs, dylibs_count);
    free(sections);
    /* freeInnerHeaderData is the single owner free path. */
    free(hd->macho_references);
    hd->macho_references = NULL;
    hd->macho_references_size = 0;
}

static const char* MachO_referenceStatusName(uint8_t status)
{
    switch ( status )
    {
        case MACHO_REFERENCE_RESOLVED:     return "resolved";
        case MACHO_REFERENCE_UNRESOLVED:   return "unresolved";
        case MACHO_REFERENCE_LOCAL:        return "local";
        case MACHO_REFERENCE_ABS:          return "abs";
        case MACHO_REFERENCE_LOCAL_ABS:    return "local+abs";
        default:                           return "unknown";
    }
}

static const char* MachO_referenceSectionTypeName(uint8_t type)
{
    switch ( type )
    {
        case S_NON_LAZY_SYMBOL_POINTERS: return "S_NON_LAZY_SYMBOL_POINTERS";
        case S_LAZY_SYMBOL_POINTERS:     return "S_LAZY_SYMBOL_POINTERS";
        case S_SYMBOL_STUBS:             return "S_SYMBOL_STUBS";
        default:                         return "other";
    }
}

/* Extended, human readable output of exactly the same relations. */
static void MachO_printReferences(PHeaderData hd)
{
    size_t i;

    if ( !hd )
        return;

    if ( hd->macho_references_error != MACHO_REFERENCES_OK )
    {
        printf("MachOIndirectReferences: error=%u (incomplete table, %zu entr%s)\n",
               (unsigned)hd->macho_references_error,
               hd->macho_references_size,
               hd->macho_references_size == 1 ? "y" : "ies");
        return;
    }

    printf("MachOIndirectReferences (%zu):\n", hd->macho_references_size);
    for ( i = 0; i < hd->macho_references_size; ++i )
    {
        MachOReferenceData* ref = &hd->macho_references[i];

        printf(" - %s segment #%u \"%s\" section #%u (#%u in segment) \"%s\" slot #%u\n",
               MachO_referenceSectionTypeName(ref->section_type),
               ref->segment_index,
               ref->segment_name ? ref->segment_name : "",
               ref->section_index,
               ref->section_index_in_segment,
               ref->section_name ? ref->section_name : "",
               ref->slot_index);
        printf("   - address=0x%llx image_offset=%llu file_offset=%llu\n",
               (unsigned long long)ref->address,
               (unsigned long long)ref->image_offset,
               (unsigned long long)ref->file_offset);
        printf("   - indirect index=%u raw=0x%08x status=%s\n",
               ref->indirect_index, ref->indirect_value,
               MachO_referenceStatusName(ref->status));

        if ( ref->status == MACHO_REFERENCE_RESOLVED ||
             ref->status == MACHO_REFERENCE_UNRESOLVED )
        {
            printf("   - nlist=%u name=\"%s\" type=0x%02x sect=%u desc=0x%04x value=0x%llx\n",
                   ref->nlist_index,
                   ref->symbol_name ? ref->symbol_name : "",
                   (unsigned)ref->n_type,
                   (unsigned)ref->n_sect,
                   (unsigned)ref->n_desc,
                   (unsigned long long)ref->n_value);
            if ( ref->library_ordinal >= 0 )
            {
                printf("   - library ordinal=%d kind=%u path=\"%s\"\n",
                       ref->library_ordinal,
                       (unsigned)ref->library_kind,
                       ref->library_path ? ref->library_path : "");
            }
        }
    }
}

#endif /* HEADER_PARSER_MACHO_REFERENCES_H */
