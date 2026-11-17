#ifndef HEADER_PARSER_MACH_O_REFERENCE_PARSER_H
#define HEADER_PARSER_MACH_O_REFERENCE_PARSER_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../HeaderData.h"
#include "../Globals.h"
#include "../utils/common_fileio.h"

#include "MachOFileHeader.h"
#include "MachOHeaderOffsets.h"

/**
 * Static resolution of the indirect symbol references of a thin Mach-O image:
 *
 *   S_NON_LAZY_SYMBOL_POINTERS / S_LAZY_SYMBOL_POINTERS / S_SYMBOL_STUBS slot
 *   -> indirect symbol table entry
 *   -> nlist symbol
 *   -> library ordinal -> load class dylib command path
 *
 * The result is stored in hd->macho_references / hd->macho_references_size /
 * hd->macho_references_status. The array and all strings are owned by hd and
 * released by freeHeaderData.
 *
 * Only the static structure of the image is analyzed: no dependency file is
 * opened and nothing is loaded, executed or modified.
 *
 * Works for thin 32/64 bit images of both endians. Pointer slot widths follow
 * the image bitness, stub widths are taken from the section's reserved2 field,
 * reserved1 is the start index into the indirect symbol table. All table
 * offsets (symoff, stroff, indirectsymoff, section offsets) are relative to
 * the image start (gp->file.start_offset); the tables may be located far away
 * from the load command area.
 */

#define MACHO_REF_INDIRECT_SYMBOL_LOCAL (0x80000000u)
#define MACHO_REF_INDIRECT_SYMBOL_ABS (0x40000000u)

#define MACHO_REF_ORDINAL_SELF (0x0u)
#define MACHO_REF_ORDINAL_DYNAMIC_LOOKUP (0xfeu)
#define MACHO_REF_ORDINAL_EXECUTABLE (0xffu)

typedef struct MachORefSectionInfo
{
    uint32_t segment_index;
    uint32_t section_index;
    char segname[MACH_O_SEG_NAME_LN];
    char sectname[MACH_O_SEG_NAME_LN];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t type;
    uint32_t reserved1;
    uint32_t reserved2;
} MachORefSectionInfo;

typedef struct MachORefContext
{
    FILE* fp;
    uint64_t file_size;
    uint64_t start; /* image start within the file */
    uint8_t be;
    uint8_t is64;

    uint32_t segment_count;
    uint32_t section_count;

    MachORefSectionInfo* secs;
    size_t secs_size;
    size_t secs_capacity;

    char** dylibs; /* load class dylib paths in load command order; entries may be NULL */
    size_t dylibs_size;
    size_t dylibs_capacity;

    uint8_t have_symtab;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;

    uint8_t have_dysymtab;
    uint32_t indirectsymoff;
    uint32_t nindirectsyms;

    uint8_t failed; /* allocation failure */
} MachORefContext;

static uint16_t MachORef_readU16(const uint8_t* p, uint8_t be);
static uint32_t MachORef_readU32(const uint8_t* p, uint8_t be);
static uint64_t MachORef_readU64(const uint8_t* p, uint8_t be);
static uint8_t MachORef_readAt(FILE* fp, uint64_t abs_off, uint8_t* buf, size_t ln, uint64_t file_size);
static char* MachORef_copyString(const char* s, size_t max_ln);
static void MachORef_addSection(MachORefContext* ctx, const MachORefSectionInfo* sec);
static void MachORef_addDylib(MachORefContext* ctx, char* path);
static void MachORef_collectSegment(MachORefContext* ctx, uint64_t cmd_off, uint32_t cmdsize);
static void MachORef_collectSymtab(MachORefContext* ctx, uint64_t cmd_off);
static void MachORef_collectDysymtab(MachORefContext* ctx, uint64_t cmd_off);
static void MachORef_collectDylib(MachORefContext* ctx, uint64_t cmd_off, uint32_t cmdsize);
static char* MachORef_stringAt(const uint8_t* strtab, uint64_t strtab_size, uint32_t strx);
static void MachORef_resolveSymbol(MachORefContext* ctx, uint32_t idx, const uint8_t* strtab, uint64_t strtab_size, MachOReferenceData* ref);
static uint8_t MachORef_appendRef(PHeaderData hd, const MachOReferenceData* ref, size_t* capacity);
static void MachORef_discardRefs(PHeaderData hd);
static void MachORef_freeContext(MachORefContext* ctx);
static void MachO_buildReferences(PHeaderData hd, PGlobalParams gp);



uint16_t MachORef_readU16(const uint8_t* p, uint8_t be)
{
    if ( be )
        return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint32_t MachORef_readU32(const uint8_t* p, uint8_t be)
{
    if ( be )
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t MachORef_readU64(const uint8_t* p, uint8_t be)
{
    uint64_t hi;
    uint64_t lo;
    if ( be )
    {
        hi = MachORef_readU32(p, be);
        lo = MachORef_readU32(p + 4, be);
    }
    else
    {
        lo = MachORef_readU32(p, be);
        hi = MachORef_readU32(p + 4, be);
    }
    return (hi << 32) | lo;
}

/**
 * Read ln bytes at the absolute file offset abs_off.
 * Returns 0 when the range is not fully inside the file or reading failed.
 */
uint8_t MachORef_readAt(FILE* fp, uint64_t abs_off, uint8_t* buf, size_t ln, uint64_t file_size)
{
    if ( abs_off > file_size || (uint64_t)ln > file_size - abs_off )
        return 0;
    return readFile(fp, (size_t)abs_off, ln, buf) == ln;
}

/**
 * Copy at most max_ln chars of s into a fresh NUL terminated string.
 */
char* MachORef_copyString(const char* s, size_t max_ln)
{
    size_t ln = 0;
    char* out;

    while ( ln < max_ln && s[ln] != 0 )
        ln++;

    out = (char*) malloc(ln + 1);
    if ( out == NULL )
        return NULL;

    memcpy(out, s, ln);
    out[ln] = 0;

    return out;
}

void MachORef_addSection(MachORefContext* ctx, const MachORefSectionInfo* sec)
{
    MachORefSectionInfo* tmp;

    if ( ctx->secs_size >= ctx->secs_capacity )
    {
        size_t ncap = (ctx->secs_capacity == 0) ? 4 : ctx->secs_capacity * 2;
        tmp = (MachORefSectionInfo*) realloc(ctx->secs, ncap * sizeof(MachORefSectionInfo));
        if ( tmp == NULL )
        {
            ctx->failed = 1;
            return;
        }
        ctx->secs = tmp;
        ctx->secs_capacity = ncap;
    }
    ctx->secs[ctx->secs_size++] = *sec;
}

void MachORef_addDylib(MachORefContext* ctx, char* path)
{
    char** tmp;

    if ( ctx->dylibs_size >= ctx->dylibs_capacity )
    {
        size_t ncap = (ctx->dylibs_capacity == 0) ? 4 : ctx->dylibs_capacity * 2;
        tmp = (char**) realloc(ctx->dylibs, ncap * sizeof(char*));
        if ( tmp == NULL )
        {
            ctx->failed = 1;
            free(path);
            return;
        }
        ctx->dylibs = tmp;
        ctx->dylibs_capacity = ncap;
    }
    ctx->dylibs[ctx->dylibs_size++] = path;
}

/**
 * Collect the indirect reference sections of one LC_SEGMENT/LC_SEGMENT_64 command.
 * The physical section numbering counts all sections, also non indirect ones.
 */
void MachORef_collectSegment(MachORefContext* ctx, uint64_t cmd_off, uint32_t cmdsize)
{
    uint8_t* buf;
    uint32_t nsects;
    uint32_t k;
    uint32_t seg_hdr = (ctx->is64) ? SIZE_OF_MACHO_O_SEGMENT_HEADER_64 : SIZE_OF_MACHO_O_SEGMENT_HEADER_32;
    uint32_t sect_size = (ctx->is64) ? SIZE_OF_MACHO_O_SECTION_HEADER_64 : SIZE_OF_MACHO_O_SECTION_HEADER_32;
    const MachO_Section_Offsets* offs = (ctx->is64) ? &MachOsectionOffsets64 : &MachOsectionOffsets32;

    ctx->segment_count++;
    if ( cmdsize < seg_hdr )
        return;

    buf = (uint8_t*) malloc(cmdsize);
    if ( buf == NULL )
    {
        ctx->failed = 1;
        return;
    }
    if ( !MachORef_readAt(ctx->fp, cmd_off, buf, cmdsize, ctx->file_size) )
    {
        free(buf);
        return;
    }

    nsects = MachORef_readU32(buf + ((ctx->is64) ? SegmentCommandOffsets64.nsects : SegmentCommandOffsets32.nsects), ctx->be);
    if ( (uint64_t)nsects > (uint64_t)(cmdsize - seg_hdr) / sect_size )
        nsects = (cmdsize - seg_hdr) / sect_size;

    for ( k = 0; k < nsects; k++ )
    {
        const uint8_t* s = buf + seg_hdr + (size_t)k * sect_size;
        MachORefSectionInfo sec;
        uint32_t flags = MachORef_readU32(s + offs->flags, ctx->be);
        uint32_t type = flags & SECTION_TYPE;

        sec.section_index = ctx->section_count++;
        sec.segment_index = ctx->segment_count - 1;

        if ( type != S_NON_LAZY_SYMBOL_POINTERS && type != S_LAZY_SYMBOL_POINTERS && type != S_SYMBOL_STUBS )
            continue;

        memcpy(sec.segname, s + offs->segname, MACH_O_SEG_NAME_LN);
        memcpy(sec.sectname, s + offs->sectname, MACH_O_SEG_NAME_LN);
        if ( ctx->is64 )
        {
            sec.addr = MachORef_readU64(s + offs->addr, ctx->be);
            sec.size = MachORef_readU64(s + offs->size, ctx->be);
        }
        else
        {
            sec.addr = MachORef_readU32(s + offs->addr, ctx->be);
            sec.size = MachORef_readU32(s + offs->size, ctx->be);
        }
        sec.offset = MachORef_readU32(s + offs->offset, ctx->be);
        sec.reserved1 = MachORef_readU32(s + offs->reserved1, ctx->be);
        sec.reserved2 = MachORef_readU32(s + offs->reserved2, ctx->be);
        sec.type = type;

        MachORef_addSection(ctx, &sec);
    }

    free(buf);
}

void MachORef_collectSymtab(MachORefContext* ctx, uint64_t cmd_off)
{
    uint8_t raw[SIZE_OF_MACHO_O_SYMTAB_COMMAND];

    if ( ctx->have_symtab )
        return;
    if ( !MachORef_readAt(ctx->fp, cmd_off, raw, SIZE_OF_MACHO_O_SYMTAB_COMMAND, ctx->file_size) )
        return;

    ctx->symoff = MachORef_readU32(raw + SymtabCommandOffsets.symoff, ctx->be);
    ctx->nsyms = MachORef_readU32(raw + SymtabCommandOffsets.nsyms, ctx->be);
    ctx->stroff = MachORef_readU32(raw + SymtabCommandOffsets.stroff, ctx->be);
    ctx->strsize = MachORef_readU32(raw + SymtabCommandOffsets.strsize, ctx->be);
    ctx->have_symtab = 1;
}

void MachORef_collectDysymtab(MachORefContext* ctx, uint64_t cmd_off)
{
    uint8_t raw[SIZE_OF_MACHO_O_DY_SYMTAB_COMMAND];

    if ( ctx->have_dysymtab )
        return;
    if ( !MachORef_readAt(ctx->fp, cmd_off, raw, SIZE_OF_MACHO_O_DY_SYMTAB_COMMAND, ctx->file_size) )
        return;

    ctx->indirectsymoff = MachORef_readU32(raw + DySymtabCommandOffsets.indirectsymoff, ctx->be);
    ctx->nindirectsyms = MachORef_readU32(raw + DySymtabCommandOffsets.nindirectsyms, ctx->be);
    ctx->have_dysymtab = 1;
}

/**
 * Append the path of one load class dylib command (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB,
 * LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB, LC_LAZY_LOAD_DYLIB) to the ordinal list.
 * The command counts for the ordinal mapping even when its name cannot be read;
 * in that case a NULL path is stored and references to it stay unresolved.
 */
void MachORef_collectDylib(MachORefContext* ctx, uint64_t cmd_off, uint32_t cmdsize)
{
    uint8_t raw[SIZE_OF_MACHO_O_DYLIB_COMMAND];
    uint32_t name_off;
    char* path = NULL;

    if ( MachORef_readAt(ctx->fp, cmd_off, raw, SIZE_OF_MACHO_O_DYLIB_COMMAND, ctx->file_size) )
    {
        name_off = MachORef_readU32(raw + DylibCommandOffsets.dylib + DylibOffsets.name, ctx->be);
        if ( name_off < cmdsize )
        {
            uint32_t max_ln = cmdsize - name_off;
            uint8_t* tmp = (uint8_t*) malloc(max_ln);
            if ( tmp != NULL )
            {
                if ( MachORef_readAt(ctx->fp, cmd_off + name_off, tmp, max_ln, ctx->file_size) )
                    path = MachORef_copyString((const char*) tmp, max_ln);
                free(tmp);
            }
        }
    }

    MachORef_addDylib(ctx, path);
}

/**
 * Look up a NUL terminated string in the string table.
 * Returns an owned copy or NULL when strx is out of range or no NUL is found.
 */
char* MachORef_stringAt(const uint8_t* strtab, uint64_t strtab_size, uint32_t strx)
{
    uint64_t i;

    if ( strtab == NULL || (uint64_t)strx >= strtab_size )
        return NULL;

    for ( i = strx; i < strtab_size; i++ )
    {
        if ( strtab[i] == 0 )
            return MachORef_copyString((const char*) strtab + strx, (size_t)(i - strx));
    }

    return NULL;
}

/**
 * Resolve one plain indirect symbol table index to its nlist symbol and library.
 * On any failure the slot keeps its raw numbers and stays MACHO_REFERENCE_UNRESOLVED.
 */
void MachORef_resolveSymbol(MachORefContext* ctx, uint32_t idx, const uint8_t* strtab, uint64_t strtab_size, MachOReferenceData* ref)
{
    uint8_t raw[16];
    uint64_t esize = (ctx->is64) ? 16 : 12;
    uint32_t strx;
    uint8_t ordinal;
    char* name = NULL;
    char* path = NULL;

    ref->kind = MACHO_REFERENCE_UNRESOLVED;
    ref->symbol_index = idx;

    if ( !ctx->have_symtab )
        return;
    if ( (uint64_t)idx >= (uint64_t)ctx->nsyms )
        return;
    if ( !MachORef_readAt(ctx->fp, ctx->start + ctx->symoff + (uint64_t)idx * esize, raw, (size_t)esize, ctx->file_size) )
        return;

    strx = MachORef_readU32(raw, ctx->be);
    ref->symbol_type = raw[4];
    ref->symbol_sect = raw[5];
    ref->symbol_desc = MachORef_readU16(raw + 6, ctx->be);
    ref->symbol_value = (ctx->is64) ? MachORef_readU64(raw + 8, ctx->be) : MachORef_readU32(raw + 8, ctx->be);
    ordinal = (uint8_t)((ref->symbol_desc >> 8) & 0xff);
    ref->library_ordinal = ordinal;

    name = MachORef_stringAt(strtab, strtab_size, strx);
    if ( name == NULL )
        return;

    if ( ordinal != MACHO_REF_ORDINAL_SELF && ordinal != MACHO_REF_ORDINAL_DYNAMIC_LOOKUP && ordinal != MACHO_REF_ORDINAL_EXECUTABLE )
    {
        const char* src = NULL;
        if ( (size_t)ordinal <= ctx->dylibs_size )
            src = ctx->dylibs[ordinal - 1];
        if ( src == NULL )
        {
            free(name);
            return;
        }
        path = MachORef_copyString(src, strlen(src));
        if ( path == NULL )
        {
            free(name);
            ctx->failed = 1;
            return;
        }
    }

    ref->symbol_name = name;
    ref->library_path = path;
    ref->kind = MACHO_REFERENCE_SYMBOL;
}

uint8_t MachORef_appendRef(PHeaderData hd, const MachOReferenceData* ref, size_t* capacity)
{
    if ( hd->macho_references_size >= *capacity )
    {
        size_t ncap = (*capacity == 0) ? 8 : (*capacity) * 2;
        MachOReferenceData* tmp = (MachOReferenceData*) realloc(hd->macho_references, ncap * sizeof(MachOReferenceData));
        if ( tmp == NULL )
            return 0;
        hd->macho_references = tmp;
        *capacity = ncap;
    }
    hd->macho_references[hd->macho_references_size++] = *ref;
    return 1;
}

/**
 * Drop all built references. An incomplete table must not be mistaken for a
 * complete result, so structural errors end with an empty array.
 */
void MachORef_discardRefs(PHeaderData hd)
{
    size_t i;

    for ( i = 0; i < hd->macho_references_size; i++ )
    {
        free(hd->macho_references[i].segment_name);
        free(hd->macho_references[i].section_name);
        free(hd->macho_references[i].symbol_name);
        free(hd->macho_references[i].library_path);
    }
    free(hd->macho_references);
    hd->macho_references = NULL;
    hd->macho_references_size = 0;
}

void MachORef_freeContext(MachORefContext* ctx)
{
    size_t i;

    free(ctx->secs);
    for ( i = 0; i < ctx->dylibs_size; i++ )
        free(ctx->dylibs[i]);
    free(ctx->dylibs);
}

void MachO_buildReferences(PHeaderData hd, PGlobalParams gp)
{
    MachORefContext ctx;
    uint8_t header[SIZE_OF_MACHO_O_HEADER_64];
    uint32_t ncmds;
    uint32_t i;
    uint64_t cmd_off;
    uint64_t needed;
    uint32_t* indirect = NULL;
    uint8_t* strtab = NULL;
    uint64_t strtab_size = 0;
    size_t refs_capacity = 0;
    size_t si;
    uint8_t status = MACHO_REFERENCES_NONE;

    hd->macho_references = NULL;
    hd->macho_references_size = 0;
    hd->macho_references_status = MACHO_REFERENCES_NONE;

    if ( hd->headertype != HEADER_TYPE_MACH_O || gp == NULL || gp->file.handle == NULL )
        return;

    memset(&ctx, 0, sizeof(ctx));
    ctx.fp = gp->file.handle;
    ctx.file_size = gp->file.size;
    ctx.start = gp->file.start_offset;
    ctx.be = (hd->endian == ENDIAN_BIG);
    ctx.is64 = (hd->h_bitness == 64);

    if ( !MachORef_readAt(ctx.fp, ctx.start, header, SIZE_OF_MACHO_O_HEADER_64, ctx.file_size) )
        return;
    ncmds = MachORef_readU32(header + MachHeaderOffsets.ncmds, ctx.be);

    cmd_off = ctx.start + ((ctx.is64) ? SIZE_OF_MACHO_O_HEADER_64 : SIZE_OF_MACHO_O_HEADER);
    for ( i = 0; i < ncmds; i++ )
    {
        uint8_t lc_raw[SIZE_OF_MACHO_O_LOAD_COMMAND];
        uint32_t cmd;
        uint32_t cmdsize;

        if ( !MachORef_readAt(ctx.fp, cmd_off, lc_raw, SIZE_OF_MACHO_O_LOAD_COMMAND, ctx.file_size) )
            break;
        cmd = MachORef_readU32(lc_raw + LoadCommandOffsets.cmd, ctx.be);
        cmdsize = MachORef_readU32(lc_raw + LoadCommandOffsets.cmdsize, ctx.be);
        if ( cmdsize < SIZE_OF_MACHO_O_LOAD_COMMAND || cmd_off + cmdsize > ctx.file_size )
            break;

        switch ( cmd )
        {
            case LC_SEGMENT:
            case LC_SEGMENT_64:
                MachORef_collectSegment(&ctx, cmd_off, cmdsize);
                break;
            case LC_SYMTAB:
                MachORef_collectSymtab(&ctx, cmd_off);
                break;
            case LC_DYSYMTAB:
                MachORef_collectDysymtab(&ctx, cmd_off);
                break;
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB:
            case LC_LAZY_LOAD_DYLIB:
                MachORef_collectDylib(&ctx, cmd_off, cmdsize);
                break;
            default:
                break;
        }
        cmd_off += cmdsize;
    }

    if ( ctx.failed )
        goto error;

    if ( ctx.secs_size == 0 )
        goto done; /* no indirect reference sections: empty array, status NONE */

    if ( !ctx.have_dysymtab )
        goto error;

    /* validate the sections and compute the needed indirect table range */
    needed = 0;
    for ( si = 0; si < ctx.secs_size; si++ )
    {
        MachORefSectionInfo* sec = &ctx.secs[si];
        uint64_t width = (sec->type == S_SYMBOL_STUBS) ? sec->reserved2 : ((ctx.is64) ? 8 : 4);
        uint64_t end_index;

        if ( width == 0 || (sec->size % width) != 0 )
            goto error;
        end_index = (uint64_t)sec->reserved1 + sec->size / width;
        if ( end_index > needed )
            needed = end_index;
    }

    if ( needed > (uint64_t)ctx.nindirectsyms )
        goto error;
    if ( ctx.start + ctx.indirectsymoff + needed * 4 > ctx.file_size )
        goto error;

    /* read the indirect symbol table */
    if ( needed > 0 )
    {
        uint8_t* raw = (uint8_t*) malloc((size_t)(needed * 4));
        uint64_t j;

        if ( raw == NULL )
            goto error;
        if ( !MachORef_readAt(ctx.fp, ctx.start + ctx.indirectsymoff, raw, (size_t)(needed * 4), ctx.file_size) )
        {
            free(raw);
            goto error;
        }
        indirect = (uint32_t*) malloc((size_t)(needed * sizeof(uint32_t)));
        if ( indirect == NULL )
        {
            free(raw);
            goto error;
        }
        for ( j = 0; j < needed; j++ )
            indirect[j] = MachORef_readU32(raw + j * 4, ctx.be);
        free(raw);
    }

    /* read the string table if possible; truncation only affects single entries */
    if ( ctx.have_symtab && ctx.strsize > 0 && ctx.start + ctx.stroff < ctx.file_size )
    {
        uint64_t avail = ctx.file_size - (ctx.start + ctx.stroff);
        if ( avail > ctx.strsize )
            avail = ctx.strsize;
        strtab = (uint8_t*) malloc((size_t)avail);
        if ( strtab != NULL )
        {
            if ( MachORef_readAt(ctx.fp, ctx.start + ctx.stroff, strtab, (size_t)avail, ctx.file_size) )
                strtab_size = avail;
            else
            {
                free(strtab);
                strtab = NULL;
            }
        }
    }

    /* build the references in physical section order, then slot order */
    for ( si = 0; si < ctx.secs_size; si++ )
    {
        MachORefSectionInfo* sec = &ctx.secs[si];
        uint64_t width = (sec->type == S_SYMBOL_STUBS) ? sec->reserved2 : ((ctx.is64) ? 8 : 4);
        uint64_t nslots = sec->size / width;
        uint64_t slot;

        for ( slot = 0; slot < nslots; slot++ )
        {
            MachOReferenceData ref;
            uint32_t v;

            memset(&ref, 0, sizeof(ref));
            ref.segment_index = sec->segment_index;
            ref.section_index = sec->section_index;
            ref.section_type = sec->type;
            ref.segment_name = MachORef_copyString(sec->segname, MACH_O_SEG_NAME_LN);
            ref.section_name = MachORef_copyString(sec->sectname, MACH_O_SEG_NAME_LN);
            if ( ref.segment_name == NULL || ref.section_name == NULL )
            {
                free(ref.segment_name);
                free(ref.section_name);
                goto error;
            }
            ref.slot_index = slot;
            ref.slot_address = sec->addr + slot * width;
            ref.slot_image_offset = (uint64_t)sec->offset + slot * width;
            ref.slot_file_offset = ctx.start + ref.slot_image_offset;
            ref.indirect_index = (uint64_t)sec->reserved1 + slot;
            v = indirect[sec->reserved1 + slot];
            ref.indirect_value = v;

            if ( (v & MACHO_REF_INDIRECT_SYMBOL_LOCAL) && (v & MACHO_REF_INDIRECT_SYMBOL_ABS) )
                ref.kind = MACHO_REFERENCE_LOCAL_ABS;
            else if ( v & MACHO_REF_INDIRECT_SYMBOL_LOCAL )
                ref.kind = MACHO_REFERENCE_LOCAL;
            else if ( v & MACHO_REF_INDIRECT_SYMBOL_ABS )
                ref.kind = MACHO_REFERENCE_ABS;
            else
                MachORef_resolveSymbol(&ctx, v, strtab, strtab_size, &ref);

            if ( ctx.failed || !MachORef_appendRef(hd, &ref, &refs_capacity) )
            {
                free(ref.segment_name);
                free(ref.section_name);
                free(ref.symbol_name);
                free(ref.library_path);
                goto error;
            }
        }
    }

    status = MACHO_REFERENCES_OK;
    goto done;

error:
    MachORef_discardRefs(hd);
    status = MACHO_REFERENCES_ERROR;

done:
    free(indirect);
    free(strtab);
    MachORef_freeContext(&ctx);
    hd->macho_references_status = status;
}

#endif
