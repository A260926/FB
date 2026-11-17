//----------------------------------------------------------------------------//
// libMachO file memory map regression tests.
//
// Three groups are exercised:
//   coordinates - the virtual address view derived from segment commands
//   overlays    - memory-object local views, aliasing, lifetime, protection
//   linkedit    - __LINKEDIT consumption through the existing symbol/string
//                 table entry points, proving symoff/stroff file coordinates
//----------------------------------------------------------------------------//

#include "../task_support/check.h"
#include "macho.h"

#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>

#include <stdlib.h>
#include <string.h>

#ifndef MH_FILESET
#   define MH_FILESET 0xc
#endif

#define TEST_MH_MAGIC     0xfeedfaceu
#define TEST_FAT_MAGIC    0xcafebabeu

#define TEXT_VMADDR       0x100000000ULL
#define LINKEDIT_VMADDR   0x100004000ULL

//----------------------------------------------------------------------------//
// Synthetic Mach-O construction
//----------------------------------------------------------------------------//

typedef struct {
    char name[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
} seg_spec;

typedef struct {
    int present;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} symtab_spec;

static void
write_u32(void *base, size_t offset, uint32_t value)
{ memcpy((uint8_t*)base + offset, &value, sizeof(value)); }

static uint8_t *
build_file(size_t total, const seg_spec *segs, size_t nsegs, const symtab_spec *symtab)
{
    uint8_t *buf = calloc(1, total);
    if (buf == NULL) return NULL;

    struct mach_header_64 *header = (struct mach_header_64*)buf;
    header->magic = MH_MAGIC_64;
    header->cputype = CPU_TYPE_X86_64;
    header->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
    header->filetype = MH_DYLIB;
    header->ncmds = (uint32_t)nsegs + (symtab && symtab->present ? 1 : 0);
    header->flags = 0;
    header->reserved = 0;

    size_t offset = sizeof(struct mach_header_64);

    for (size_t i = 0; i < nsegs; i++) {
        if (offset + sizeof(struct segment_command_64) > total) return buf;
        struct segment_command_64 *segment = (struct segment_command_64*)(buf + offset);
        segment->cmd = LC_SEGMENT_64;
        segment->cmdsize = sizeof(struct segment_command_64);
        memcpy(segment->segname, segs[i].name, sizeof(segment->segname));
        segment->vmaddr = segs[i].vmaddr;
        segment->vmsize = segs[i].vmsize;
        segment->fileoff = segs[i].fileoff;
        segment->filesize = segs[i].filesize;
        segment->maxprot = VM_PROT_READ;
        segment->initprot = VM_PROT_READ;
        segment->nsects = 0;
        segment->flags = 0;
        offset += sizeof(struct segment_command_64);
    }

    if (symtab && symtab->present) {
        if (offset + sizeof(struct symtab_command) > total) return buf;
        struct symtab_command *command = (struct symtab_command*)(buf + offset);
        command->cmd = LC_SYMTAB;
        command->cmdsize = sizeof(struct symtab_command);
        command->symoff = symtab->symoff;
        command->nsyms = symtab->nsyms;
        command->stroff = symtab->stroff;
        command->strsize = symtab->strsize;
        offset += sizeof(struct symtab_command);
    }

    header->sizeofcmds = (uint32_t)(offset - sizeof(struct mach_header_64));
    return buf;
}

static void
put_symbol(uint8_t *symtab, uint32_t index, uint32_t strx, uint8_t type, uint8_t sect, int16_t desc, uint64_t value)
{
    struct nlist_64 *symbol = (struct nlist_64*)(symtab + (size_t)index * sizeof(struct nlist_64));
    symbol->n_un.n_strx = strx;
    symbol->n_type = type;
    symbol->n_sect = sect;
    symbol->n_desc = desc;
    symbol->n_value = value;
}

// Builds a two-segment image whose __LINKEDIT segment carries a symbol table
// and a string table.  sym1/sym2 become the names of the two symbols.
static uint8_t *
build_linkedit_file(size_t *out_size, const char *sym1, const char *sym2,
                    uint64_t text_vmaddr, uint64_t link_vmaddr, uint64_t link_fileoff,
                    int bad_nsyms, int bad_strsize)
{
    seg_spec segs[2] = {
        { "__TEXT",     text_vmaddr, 0x1000, 0,            0x400 },
        { "__LINKEDIT", link_vmaddr, 0x1000, link_fileoff, 0x400 },
    };

    uint32_t nsyms   = bad_nsyms ? 0x10000u : 2u;
    uint32_t strsize = bad_strsize ? 0x1000u : 15u;
    uint32_t stroff  = (uint32_t)(link_fileoff + 0x100);

    symtab_spec symtab = { 1, (uint32_t)link_fileoff, nsyms, stroff, strsize };
    size_t total = (size_t)(link_fileoff + 0x400);

    uint8_t *buf = build_file(total, segs, 2, &symtab);
    if (buf == NULL) return NULL;

    uint8_t *strings = buf + stroff;
    size_t cursor = 0;
    strings[cursor++] = 0;
    memcpy(strings + cursor, sym1, strlen(sym1)); cursor += strlen(sym1);
    strings[cursor++] = 0;
    memcpy(strings + cursor, sym2, strlen(sym2)); cursor += strlen(sym2);
    strings[cursor++] = 0;

    uint32_t strx1 = 1;
    uint32_t strx2 = (uint32_t)(1 + strlen(sym1) + 1);

    put_symbol(buf + link_fileoff, 0, strx1, N_SECT, 1, 0, 0x100001000ULL);
    put_symbol(buf + link_fileoff, 1, strx2, N_SECT, 1, 0, 0x100001010ULL);

    *out_size = total;
    return buf;
}

//----------------------------------------------------------------------------//
// Fixtures
//----------------------------------------------------------------------------//

typedef struct {
    mk_memory_map_file_t map;
    mk_macho_t image;
    mk_segment_t linkedit;
    int map_ready;
    int image_ready;
    int linkedit_ready;
} fixture;

static void
fixture_close(fixture *f)
{
    if (f->linkedit_ready) mk_segment_free(&f->linkedit);
    if (f->image_ready) mk_macho_free(&f->image);
    if (f->map_ready) mk_memory_map_file_free(&f->map);

    f->linkedit_ready = 0;
    f->image_ready = 0;
    f->map_ready = 0;
}

static mk_error_t
fixture_open(fixture *f, uint8_t *bytes, size_t size, mk_vm_slide_t slide)
{
    memset(f, 0, sizeof(*f));

    mk_error_t err = mk_memory_map_file_init(NULL, bytes, size, slide, &f->map);
    if (err != MK_ESUCCESS) return err;
    f->map_ready = 1;

    mk_vm_address_t address = mk_memory_map_file_get_header_address(&f->map);
    err = mk_macho_init_with_slide(NULL, "file-map-regression", slide, address, &f->map, &f->image);
    if (err != MK_ESUCCESS) return err;
    f->image_ready = 1;

    struct load_command *lc = NULL;
    while ((lc = mk_macho_next_command_type(&f->image, lc, LC_SEGMENT_64, NULL))) {
        mk_segment_t candidate;
        if (mk_segment_init_with_mach_load_command(&f->image, (void*)lc, &candidate))
            continue;

        char segname[17] = { 0 };
        mk_segment_copy_name(&candidate, segname);

        if (!strcmp(segname, "__LINKEDIT")) {
            f->linkedit = candidate;
            f->linkedit_ready = 1;
            return MK_ESUCCESS;
        }

        mk_segment_free(&candidate);
    }

    return MK_ENOT_FOUND;
}

//----------------------------------------------------------------------------//
#pragma mark -  coordinates
//----------------------------------------------------------------------------//

static int
case_coordinates_header_address(void)
{
    seg_spec segs[1] = {
        { "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x400 },
    };
    size_t size = 0;
    uint8_t *bytes = build_file(0x400, segs, 1, NULL);
    TASK_CHECK(bytes != NULL);

    const mk_vm_slide_t slide = 0x400000;
    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, 0x400, slide, &map);
    TASK_CHECK(err == MK_ESUCCESS);

    TASK_CHECK(mk_memory_map_file_get_header_address(&map) == TEXT_VMADDR + slide);

    // The header address is directly usable with mk_macho_init_with_slide.
    mk_macho_t image;
    err = mk_macho_init_with_slide(NULL, "file-map-header", slide,
                                   mk_memory_map_file_get_header_address(&map), &map, &image);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_macho_is_64_bit(&image));
    TASK_CHECK(mk_macho_get_filetype(&image) == MH_DYLIB);

    // The first four header bytes round-trip through the map.
    uint32_t magic = 0;
    size_t copied = mk_memory_map_copy_bytes(&map, 0,
                                             mk_memory_map_file_get_header_address(&map),
                                             &magic, sizeof(magic), true, &err);
    TASK_CHECK(copied == sizeof(magic));
    TASK_CHECK(magic == MH_MAGIC_64);

    mk_macho_free(&image);
    mk_memory_map_file_free(&map);
    free(bytes);
    (void)size;
    return 0;
}

static int
case_coordinates_file_and_zero_fill(void)
{
    seg_spec segs[1] = {
        { "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x400 },
    };
    uint8_t *bytes = build_file(0x400, segs, 1, NULL);
    TASK_CHECK(bytes != NULL);

    // Mark the last file-backed bytes so the boundary is visible.
    memset(bytes + 0x3F0, 0xAB, 0x10);

    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, 0x400, 0, &map);
    TASK_CHECK(err == MK_ESUCCESS);

    // A read straddling filesize returns file bytes followed by zeros.
    uint8_t buffer[0x20];
    memset(buffer, 0x5A, sizeof(buffer));
    size_t copied = mk_memory_map_copy_bytes(&map, 0, TEXT_VMADDR + 0x3F0,
                                             buffer, sizeof(buffer), true, &err);
    TASK_CHECK(copied == sizeof(buffer));
    for (size_t i = 0; i < 0x10; i++) TASK_CHECK(buffer[i] == 0xAB);
    for (size_t i = 0x10; i < sizeof(buffer); i++) TASK_CHECK(buffer[i] == 0);

    // Deep in the zero-fill tail.
    uint8_t tail = 0xFF;
    copied = mk_memory_map_copy_bytes(&map, 0, TEXT_VMADDR + 0xFF0, &tail, 1, true, &err);
    TASK_CHECK(copied == 1);
    TASK_CHECK(tail == 0);

    // One byte past vmsize is unmapped.
    TASK_CHECK(!mk_memory_map_has_mapping(&map, 0, TEXT_VMADDR + 0x1000, 1, &err));

    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

static int
case_coordinates_adjacent_crossing(void)
{
    // __TEXT and __DATA are adjacent in the virtual address space but their
    // file ranges are far apart.
    seg_spec segs[2] = {
        { "__TEXT", 0x200000000ULL, 0x400, 0,    0x400 },
        { "__DATA", 0x200000400ULL, 0x100, 0x1000, 0x100 },
    };
    uint8_t *bytes = build_file(0x1100, segs, 2, NULL);
    TASK_CHECK(bytes != NULL);

    memset(bytes + 0x3F0, 0x11, 0x10);
    memset(bytes + 0x1000, 0x22, 0x100);

    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, 0x1100, 0, &map);
    TASK_CHECK(err == MK_ESUCCESS);

    uint8_t buffer[0x20];
    size_t copied = mk_memory_map_copy_bytes(&map, 0, 0x2000003F0,
                                             buffer, sizeof(buffer), true, &err);
    TASK_CHECK(copied == sizeof(buffer));
    for (size_t i = 0; i < 0x10; i++) TASK_CHECK(buffer[i] == 0x11);
    for (size_t i = 0x10; i < sizeof(buffer); i++) TASK_CHECK(buffer[i] == 0x22);

    // The whole crossing maps as one contiguous object.
    mk_memory_object_t object;
    err = mk_memory_map_init_object(&map, 0, 0x2000003F0, 0x20, true, &object);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_target_address(&object) == 0x2000003F0);
    TASK_CHECK(mk_memory_object_target_length(&object) == 0x20);
    TASK_CHECK(*(uint8_t*)mk_memory_object_address(&object) == 0x11);
    mk_memory_map_free_object(&map, &object);

    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

static int
case_coordinates_holes(void)
{
    seg_spec segs[2] = {
        { "__TEXT", 0x300000000ULL, 0x400, 0,    0x400 },
        { "__DATA", 0x300010000ULL, 0x100, 0x800, 0x100 },
    };
    uint8_t *bytes = build_file(0x900, segs, 2, NULL);
    TASK_CHECK(bytes != NULL);
    memset(bytes + 0x100, 0x33, 0x300);
    memset(bytes + 0x800, 0x44, 0x100);

    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, 0x900, 0, &map);
    TASK_CHECK(err == MK_ESUCCESS);

    // A full mapping across the hole fails.
    mk_memory_object_t object;
    err = mk_memory_map_init_object(&map, 0, 0x300000000ULL, 0x20000, true, &object);
    TASK_CHECK(err != MK_ESUCCESS);

    // A short mapping returns only the contiguous prefix, not the hole.
    err = mk_memory_map_init_object(&map, 0, 0x300000000ULL, 0x20000, false, &object);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_target_length(&object) == 0x400);
    TASK_CHECK(mk_memory_object_length(&object) == 0x400);

    // The local view is exactly the prefix; the hole is not allocated.
    mach_vm_address_t region_address = (mach_vm_address_t)mk_memory_object_address(&object);
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_port_t object_name = MACH_PORT_NULL;
    mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
    kern_return_t kr = mach_vm_region(mach_task_self(), &region_address, &region_size,
                                      VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                                      &info_count, &object_name);
    TASK_CHECK(kr == KERN_SUCCESS);
    // The mapping is page rounded, but far smaller than the 64 KiB hole.
    TASK_CHECK(region_size >= 0x400);
    TASK_CHECK(region_size < 0x10000);
    TASK_CHECK((region_address & (vm_page_size - 1)) == 0);
    if (object_name != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), object_name);

    // Reading past the prefix is out of range.
    TASK_CHECK(mk_memory_object_remap_address(&object, 0, 0x300000400ULL, 1, NULL) == UINTPTR_MAX);
    mk_memory_map_free_object(&map, &object);

    // A short copy from the middle of the first segment stops at the hole.
    uint8_t buffer[0x400];
    memset(buffer, 0xEE, sizeof(buffer));
    size_t copied = mk_memory_map_copy_bytes(&map, 0, 0x300000200ULL, buffer, sizeof(buffer), false, &err);
    TASK_CHECK(copied == 0x200);
    TASK_CHECK(buffer[0] == 0x33);
    TASK_CHECK(buffer[0x1FF] == 0x33);
    TASK_CHECK(buffer[0x200] == 0xEE); // untouched past the prefix

    // Addresses inside the hole are unmapped even with short mappings allowed.
    err = mk_memory_map_init_object(&map, 0, 0x300005000ULL, 0x10, false, &object);
    TASK_CHECK(err != MK_ESUCCESS);
    TASK_CHECK(!mk_memory_map_has_mapping(&map, 0, 0x300005000ULL, 0x10, &err));

    // The far segment is reachable on its own.
    err = mk_memory_map_init_object(&map, 0, 0x300010000ULL, 0x100, true, &object);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_length(&object) == 0x100);
    TASK_CHECK(*(uint8_t*)mk_memory_object_address(&object) == 0x44);
    mk_memory_map_free_object(&map, &object);

    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

static int
case_coordinates_pagezero(void)
{
    seg_spec segs[2] = {
        { "__PAGEZERO", 0,               0x100000000ULL, 0, 0 },
        { "__TEXT",     0x100000000ULL,  0x1000,         0, 0x400 },
    };
    uint8_t *bytes = build_file(0x400, segs, 2, NULL);
    TASK_CHECK(bytes != NULL);

    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, 0x400, 0, &map);
    TASK_CHECK(err == MK_ESUCCESS);

    // __PAGEZERO does not provide a mapping.
    TASK_CHECK(!mk_memory_map_has_mapping(&map, 0, 0, 1, &err));
    TASK_CHECK(!mk_memory_map_has_mapping(&map, 0, 0x80000000ULL, 0x10, &err));

    mk_memory_object_t object;
    err = mk_memory_map_init_object(&map, 0, 0, 0x10, true, &object);
    TASK_CHECK(err != MK_ESUCCESS);

    // The header segment still resolves.
    TASK_CHECK(mk_memory_map_file_get_header_address(&map) == 0x100000000ULL);
    err = mk_memory_map_init_object(&map, 0, 0x100000000ULL, 0x10, true, &object);
    TASK_CHECK(err == MK_ESUCCESS);
    mk_memory_map_free_object(&map, &object);

    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

static int
case_coordinates_offset_arithmetic(void)
{
    seg_spec segs[1] = {
        { "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x400 },
    };
    uint8_t *bytes = build_file(0x400, segs, 1, NULL);
    TASK_CHECK(bytes != NULL);
    memset(bytes + 0x100, 0x77, 0x10);

    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, 0x400, 0, &map);
    TASK_CHECK(err == MK_ESUCCESS);

    // offset is added to the input address, exactly as with the other maps.
    mk_memory_object_t object;
    err = mk_memory_map_init_object(&map, 0x10, TEXT_VMADDR + 0x100 - 0x10, 0x10, true, &object);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_target_address(&object) == TEXT_VMADDR + 0x100);
    for (size_t i = 0; i < 0x10; i++)
        TASK_CHECK(((uint8_t*)mk_memory_object_address(&object))[i] == 0x77);
    mk_memory_map_free_object(&map, &object);

    // Offset + address overflow is rejected.
    err = mk_memory_map_init_object(&map, 0x100, UINT64_MAX - 0x10, 0x10, true, &object);
    TASK_CHECK(err == MK_EOVERFLOW);

    // Address + length overflow is rejected.
    err = mk_memory_map_init_object(&map, 0, TEXT_VMADDR, UINT64_MAX, true, &object);
    TASK_CHECK(err == MK_EOVERFLOW);

    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

static int
case_coordinates_rejects_invalid_segments(void)
{
    // filesize greater than vmsize.
    {
        seg_spec segs[1] = { { "__TEXT", TEXT_VMADDR, 0x100, 0, 0x200 } };
        uint8_t *bytes = build_file(0x200, segs, 1, NULL);
        TASK_CHECK(bytes != NULL);
        mk_memory_map_file_t map;
        TASK_CHECK(mk_memory_map_file_init(NULL, bytes, 0x200, 0, &map) == MK_EINVALID_DATA);
        free(bytes);
    }

    // Segment file range extends beyond the input.
    {
        seg_spec segs[1] = { { "__TEXT", TEXT_VMADDR, 0x400, 0, 0x400 } };
        uint8_t *bytes = build_file(0x200, segs, 1, NULL);
        TASK_CHECK(bytes != NULL);
        mk_memory_map_file_t map;
        TASK_CHECK(mk_memory_map_file_init(NULL, bytes, 0x200, 0, &map) == MK_EINVALID_DATA);
        free(bytes);
    }

    // Non-empty virtual segments overlap.
    {
        seg_spec segs[2] = {
            { "__TEXT", TEXT_VMADDR,          0x1000, 0,    0x400 },
            { "__DATA", TEXT_VMADDR + 0x800,  0x1000, 0x400, 0x100 },
        };
        uint8_t *bytes = build_file(0x500, segs, 2, NULL);
        TASK_CHECK(bytes != NULL);
        mk_memory_map_file_t map;
        TASK_CHECK(mk_memory_map_file_init(NULL, bytes, 0x500, 0, &map) == MK_EINVALID_DATA);
        free(bytes);
    }

    return 0;
}

static int
case_coordinates_rejects_unsupported_images(void)
{
    seg_spec segs[1] = { { "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x400 } };

    // Fat magic.
    {
        uint8_t *bytes = build_file(0x400, segs, 1, NULL);
        TASK_CHECK(bytes != NULL);
        write_u32(bytes, 0, TEST_FAT_MAGIC);
        mk_memory_map_file_t map;
        TASK_CHECK(mk_memory_map_file_init(NULL, bytes, 0x400, 0, &map) == MK_EINVALID_DATA);
        free(bytes);
    }

    // 32-bit magic.
    {
        uint8_t *bytes = build_file(0x400, segs, 1, NULL);
        TASK_CHECK(bytes != NULL);
        write_u32(bytes, 0, TEST_MH_MAGIC);
        mk_memory_map_file_t map;
        TASK_CHECK(mk_memory_map_file_init(NULL, bytes, 0x400, 0, &map) == MK_EINVALID_DATA);
        free(bytes);
    }

    // Fileset file type.
    {
        uint8_t *bytes = build_file(0x400, segs, 1, NULL);
        TASK_CHECK(bytes != NULL);
        write_u32(bytes, 12, MH_FILESET);
        mk_memory_map_file_t map;
        TASK_CHECK(mk_memory_map_file_init(NULL, bytes, 0x400, 0, &map) == MK_EINVALID_DATA);
        free(bytes);
    }

    // Negative slide.
    {
        uint8_t *bytes = build_file(0x400, segs, 1, NULL);
        TASK_CHECK(bytes != NULL);
        mk_memory_map_file_t map;
        TASK_CHECK(mk_memory_map_file_init(NULL, bytes, 0x400, -1, &map) == MK_EINVAL);
        free(bytes);
    }

    // Null input.
    {
        mk_memory_map_file_t map;
        TASK_CHECK(mk_memory_map_file_init(NULL, NULL, 0, 0, &map) == MK_EINVAL);
    }

    return 0;
}

//----------------------------------------------------------------------------//
#pragma mark -  overlays
//----------------------------------------------------------------------------//

static int
case_overlays_local_view(void)
{
    seg_spec segs[1] = { { "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x400 } };
    uint8_t *bytes = build_file(0x400, segs, 1, NULL);
    TASK_CHECK(bytes != NULL);
    for (size_t i = 0; i < 0x100; i++)
        bytes[0x100 + i] = (uint8_t)(i ^ 0x5A);

    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, 0x400, 0, &map);
    TASK_CHECK(err == MK_ESUCCESS);

    // The caller's input may be released immediately after init.
    memset(bytes, 0, 0x400);
    free(bytes);

    mk_memory_object_t object;
    err = mk_memory_map_init_object(&map, 0, TEXT_VMADDR + 0x100, 0x100, true, &object);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_target_address(&object) == TEXT_VMADDR + 0x100);
    TASK_CHECK(mk_memory_object_target_length(&object) == 0x100);

    uint8_t *local = (uint8_t*)mk_memory_object_address(&object);
    TASK_CHECK(local != NULL);
    for (size_t i = 0; i < 0x100; i++)
        TASK_CHECK(local[i] == (uint8_t)(i ^ 0x5A));

    mk_memory_map_free_object(&map, &object);
    mk_memory_map_file_free(&map);
    return 0;
}

static int
case_overlays_remap_roundtrip(void)
{
    seg_spec segs[1] = { { "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x400 } };
    uint8_t *bytes = build_file(0x400, segs, 1, NULL);
    TASK_CHECK(bytes != NULL);
    for (size_t i = 0x100; i < 0x400; i++)
        bytes[i] = (uint8_t)(i * 3);

    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, 0x400, 0, &map);
    TASK_CHECK(err == MK_ESUCCESS);

    mk_memory_object_t object;
    err = mk_memory_map_init_object(&map, 0, TEXT_VMADDR, 0x400, true, &object);
    TASK_CHECK(err == MK_ESUCCESS);

    // Target address -> local pointer.
    vm_address_t local = mk_memory_object_remap_address(&object, 0, TEXT_VMADDR + 0x140, 8, &err);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(local != UINTPTR_MAX);
    TASK_CHECK(*(uint8_t*)local == (uint8_t)(0x140 * 3));

    // Local pointer -> target address.
    mk_vm_address_t target = mk_memory_object_unmap_address(&object, 0, local, 8, &err);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(target == TEXT_VMADDR + 0x140);

    // Offset is applied to the target address.
    target = mk_memory_object_unmap_address(&object, 4, local - 4, 8, &err);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(target == TEXT_VMADDR + 0x140);

    // Out-of-range target addresses are rejected.
    TASK_CHECK(mk_memory_object_remap_address(&object, 0, TEXT_VMADDR + 0x400, 1, &err) == UINTPTR_MAX);
    TASK_CHECK(err == MK_EOUT_OF_RANGE);

    mk_memory_map_free_object(&map, &object);
    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

static int
case_overlays_read_only(void)
{
    seg_spec segs[1] = { { "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x400 } };
    uint8_t *bytes = build_file(0x400, segs, 1, NULL);
    TASK_CHECK(bytes != NULL);

    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, 0x400, 0, &map);
    TASK_CHECK(err == MK_ESUCCESS);

    mk_memory_object_t object;
    err = mk_memory_map_init_object(&map, 0, TEXT_VMADDR, 0x100, true, &object);
    TASK_CHECK(err == MK_ESUCCESS);

    vm_address_t local = mk_memory_object_address(&object);
    mach_vm_address_t region_address = (mach_vm_address_t)local;
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_port_t object_name = MACH_PORT_NULL;
    mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;

    kern_return_t kr = mach_vm_region(mach_task_self(), &region_address, &region_size,
                                      VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                                      &info_count, &object_name);
    TASK_CHECK(kr == KERN_SUCCESS);
    TASK_CHECK((info.protection & VM_PROT_READ) != 0);
    TASK_CHECK((info.protection & VM_PROT_WRITE) == 0);

    if (object_name != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), object_name);

    mk_memory_map_free_object(&map, &object);
    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

static int
case_overlays_multiple_objects(void)
{
    seg_spec segs[1] = { { "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x400 } };
    uint8_t *bytes = build_file(0x400, segs, 1, NULL);
    TASK_CHECK(bytes != NULL);
    memset(bytes + 0x100, 0x10, 0x100);
    memset(bytes + 0x200, 0x20, 0x100);

    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, 0x400, 0, &map);
    TASK_CHECK(err == MK_ESUCCESS);

    mk_memory_object_t first;
    mk_memory_object_t second;
    err = mk_memory_map_init_object(&map, 0, TEXT_VMADDR + 0x100, 0x100, true, &first);
    TASK_CHECK(err == MK_ESUCCESS);
    err = mk_memory_map_init_object(&map, 0, TEXT_VMADDR + 0x200, 0x100, true, &second);
    TASK_CHECK(err == MK_ESUCCESS);

    TASK_CHECK(mk_memory_object_address(&first) != mk_memory_object_address(&second));
    TASK_CHECK(*(uint8_t*)mk_memory_object_address(&first) == 0x10);
    TASK_CHECK(*(uint8_t*)mk_memory_object_address(&second) == 0x20);

    // Freeing one object must not invalidate the other.
    mk_memory_map_free_object(&map, &first);
    TASK_CHECK(*(uint8_t*)mk_memory_object_address(&second) == 0x20);
    TASK_CHECK(((uint8_t*)mk_memory_object_address(&second))[0xFF] == 0x20);

    mk_memory_map_free_object(&map, &second);
    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

static int
case_overlays_two_files_independent(void)
{
    seg_spec segs[1] = { { "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x400 } };

    uint8_t *bytes_a = build_file(0x400, segs, 1, NULL);
    uint8_t *bytes_b = build_file(0x400, segs, 1, NULL);
    TASK_CHECK(bytes_a != NULL);
    TASK_CHECK(bytes_b != NULL);

    // Same preferred addresses; A has file bytes where B is zero-filled.
    memset(bytes_a + 0x200, 0xFF, 0x100);
    memset(bytes_b + 0x200, 0x00, 0x100);

    mk_memory_map_file_t map_a;
    mk_memory_map_file_t map_b;
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes_a, 0x400, 0, &map_a) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes_b, 0x400, 0, &map_b) == MK_ESUCCESS);

    free(bytes_a);
    free(bytes_b);

    TASK_CHECK(mk_memory_map_file_get_header_address(&map_a) ==
               mk_memory_map_file_get_header_address(&map_b));

    mk_memory_object_t object_a;
    mk_memory_object_t object_b;
    TASK_CHECK(mk_memory_map_init_object(&map_a, 0, TEXT_VMADDR + 0x200, 0x100, true, &object_a) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_init_object(&map_b, 0, TEXT_VMADDR + 0x200, 0x100, true, &object_b) == MK_ESUCCESS);

    TASK_CHECK(mk_memory_object_address(&object_a) != mk_memory_object_address(&object_b));
    for (size_t i = 0; i < 0x100; i++)
        TASK_CHECK(((uint8_t*)mk_memory_object_address(&object_a))[i] == 0xFF);
    for (size_t i = 0; i < 0x100; i++)
        TASK_CHECK(((uint8_t*)mk_memory_object_address(&object_b))[i] == 0x00);

    // Freeing one map's object does not disturb the other file's view.
    mk_memory_map_free_object(&map_a, &object_a);
    TASK_CHECK(((uint8_t*)mk_memory_object_address(&object_b))[0x10] == 0x00);

    mk_memory_map_free_object(&map_b, &object_b);
    mk_memory_map_file_free(&map_a);
    mk_memory_map_file_free(&map_b);
    return 0;
}

//----------------------------------------------------------------------------//
#pragma mark -  linkedit
//----------------------------------------------------------------------------//

static int
case_linkedit_symbols_and_strings(void)
{
    size_t size = 0;
    uint8_t *bytes = build_linkedit_file(&size, "_main", "_helper",
                                         TEXT_VMADDR, LINKEDIT_VMADDR, 0x1000, 0, 0);
    TASK_CHECK(bytes != NULL);

    fixture f;
    TASK_CHECK(fixture_open(&f, bytes, size, 0x100000) == MK_ESUCCESS);
    free(bytes);

    TASK_CHECK(f.linkedit_ready);

    mk_symbol_table_t symbol_table;
    mk_error_t err = mk_symbol_table_init_with_segment(&f.linkedit, &symbol_table);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_symbol_table_get_symbol_count(&symbol_table) == 2);

    // The symbol table is addressed with the LINKEDIT file coordinates.
    // target = link_vmaddr + slide + (symoff - fileoff)
    mk_vm_range_t symbol_range = mk_symbol_table_get_target_range(&symbol_table);
    TASK_CHECK(symbol_range.location == LINKEDIT_VMADDR + 0x100000);
    TASK_CHECK(symbol_range.length == 2 * sizeof(struct nlist_64));

    mk_string_table_t string_table;
    err = mk_string_table_init_with_segment(&f.linkedit, &string_table);
    TASK_CHECK(err == MK_ESUCCESS);

    mk_vm_range_t string_range = mk_string_table_get_target_range(&string_table);
    TASK_CHECK(string_range.location == LINKEDIT_VMADDR + 0x100000 + 0x100);
    TASK_CHECK(string_range.length == 15);

    const char *expected[2] = { "_main", "_helper" };
    uint32_t index = 0;
    mk_macho_nlist_ptr symbol;
    mk_macho_nlist_ptr previous; previous.any = NULL;
    mk_vm_address_t target = 0;

    while ((symbol = mk_symbol_table_next_mach_symbol(&symbol_table, previous, &index, &target)).any) {
        TASK_CHECK(index < 2);

        mk_symbol_t parsed;
        err = mk_symbol_init(&symbol_table, symbol, &parsed);
        TASK_CHECK(err == MK_ESUCCESS);

        uint32_t strx = mk_symbol_get_strx(&parsed);
        TASK_CHECK(mk_symbol_get_type(&parsed) == N_SECT);
        TASK_CHECK(mk_symbol_get_value(&parsed) == 0x100001000ULL + index * 0x10);

        mk_vm_address_t string_address = 0;
        const char *name = mk_string_table_get_string_at_offset(&string_table, strx, &string_address);
        TASK_CHECK(name != NULL);
        TASK_CHECK(!strcmp(name, expected[index]));
        TASK_CHECK(string_address == string_range.location + strx);

        // The nlist target address comes from the same file coordinates.
        TASK_CHECK(target == symbol_range.location + index * sizeof(struct nlist_64));

        previous = symbol;
    }

    TASK_CHECK(index == 1);
    TASK_CHECK(mk_symbol_table_next_mach_symbol(&symbol_table, previous, &index, &target).any == NULL);

    // Enumerate the strings through the string table entry point.
    const char *string = NULL;
    uint32_t offset = 0;
    uint32_t count = 0;
    uint32_t seen_offsets[4] = { 0 };
    while ((string = mk_string_table_next_string(&string_table, string, &offset, NULL)) != NULL) {
        TASK_CHECK(count < 4);
        seen_offsets[count] = offset;
        count++;
    }
    TASK_CHECK(count == 3);
    TASK_CHECK(seen_offsets[0] == 0);
    TASK_CHECK(seen_offsets[1] == 1);
    TASK_CHECK(seen_offsets[2] == 7);

    mk_string_table_free(&string_table);
    mk_symbol_table_free(&symbol_table);
    fixture_close(&f);
    return 0;
}

static int
case_linkedit_shifted_file_coordinates(void)
{
    // Same virtual layout, but the __LINKEDIT segment starts at a different
    // file offset.  Resolution must follow symoff - fileoff.
    size_t size = 0;
    uint8_t *bytes = build_linkedit_file(&size, "_alpha", "_beta",
                                         0x200000000ULL, 0x200004000ULL, 0x2000, 0, 0);
    TASK_CHECK(bytes != NULL);

    fixture f;
    TASK_CHECK(fixture_open(&f, bytes, size, 0) == MK_ESUCCESS);
    free(bytes);

    mk_symbol_table_t symbol_table;
    TASK_CHECK(mk_symbol_table_init_with_segment(&f.linkedit, &symbol_table) == MK_ESUCCESS);
    TASK_CHECK(mk_symbol_table_get_target_range(&symbol_table).location == 0x200004000ULL);
    TASK_CHECK(mk_symbol_table_get_target_range(&symbol_table).length == 2 * sizeof(struct nlist_64));

    mk_string_table_t string_table;
    TASK_CHECK(mk_string_table_init_with_segment(&f.linkedit, &string_table) == MK_ESUCCESS);
    TASK_CHECK(mk_string_table_get_target_range(&string_table).location == 0x200004100ULL);

    uint32_t index = 0;
    mk_macho_nlist_ptr symbol = mk_symbol_table_next_mach_symbol(&symbol_table, (mk_macho_nlist_ptr)NULL, &index, NULL);
    TASK_CHECK(symbol.any != NULL);

    mk_symbol_t parsed;
    TASK_CHECK(mk_symbol_init(&symbol_table, symbol, &parsed) == MK_ESUCCESS);
    const char *name = mk_string_table_get_string_at_offset(&string_table, mk_symbol_get_strx(&parsed), NULL);
    TASK_CHECK(name != NULL && !strcmp(name, "_alpha"));

    mk_string_table_free(&string_table);
    mk_symbol_table_free(&symbol_table);
    fixture_close(&f);
    return 0;
}

static int
case_linkedit_two_images_independent(void)
{
    size_t size_a = 0;
    size_t size_b = 0;
    uint8_t *bytes_a = build_linkedit_file(&size_a, "_first", "_second",
                                           TEXT_VMADDR, LINKEDIT_VMADDR, 0x1000, 0, 0);
    uint8_t *bytes_b = build_linkedit_file(&size_b, "_third", "_fourth",
                                           TEXT_VMADDR, LINKEDIT_VMADDR, 0x1000, 0, 0);
    TASK_CHECK(bytes_a != NULL);
    TASK_CHECK(bytes_b != NULL);

    fixture fa;
    fixture fb;
    TASK_CHECK(fixture_open(&fa, bytes_a, size_a, 0) == MK_ESUCCESS);
    TASK_CHECK(fixture_open(&fb, bytes_b, size_b, 0) == MK_ESUCCESS);
    free(bytes_a);
    free(bytes_b);

    TASK_CHECK(mk_memory_map_file_get_header_address(&fa.map) ==
               mk_memory_map_file_get_header_address(&fb.map));

    mk_symbol_table_t table_a;
    mk_symbol_table_t table_b;
    TASK_CHECK(mk_symbol_table_init_with_segment(&fa.linkedit, &table_a) == MK_ESUCCESS);
    TASK_CHECK(mk_symbol_table_init_with_segment(&fb.linkedit, &table_b) == MK_ESUCCESS);

    mk_string_table_t strings_a;
    mk_string_table_t strings_b;
    TASK_CHECK(mk_string_table_init_with_segment(&fa.linkedit, &strings_a) == MK_ESUCCESS);
    TASK_CHECK(mk_string_table_init_with_segment(&fb.linkedit, &strings_b) == MK_ESUCCESS);

    uint32_t index = 0;
    mk_macho_nlist_ptr symbol_a = mk_symbol_table_next_mach_symbol(&table_a, (mk_macho_nlist_ptr)NULL, &index, NULL);
    index = 0;
    mk_macho_nlist_ptr symbol_b = mk_symbol_table_next_mach_symbol(&table_b, (mk_macho_nlist_ptr)NULL, &index, NULL);
    TASK_CHECK(symbol_a.any != NULL);
    TASK_CHECK(symbol_b.any != NULL);

    mk_symbol_t parsed_a;
    mk_symbol_t parsed_b;
    TASK_CHECK(mk_symbol_init(&table_a, symbol_a, &parsed_a) == MK_ESUCCESS);
    TASK_CHECK(mk_symbol_init(&table_b, symbol_b, &parsed_b) == MK_ESUCCESS);

    const char *name_a = mk_string_table_get_string_at_offset(&strings_a, mk_symbol_get_strx(&parsed_a), NULL);
    const char *name_b = mk_string_table_get_string_at_offset(&strings_b, mk_symbol_get_strx(&parsed_b), NULL);
    TASK_CHECK(name_a != NULL && !strcmp(name_a, "_first"));
    TASK_CHECK(name_b != NULL && !strcmp(name_b, "_third"));

    mk_string_table_free(&strings_a);
    mk_string_table_free(&strings_b);
    mk_symbol_table_free(&table_a);
    mk_symbol_table_free(&table_b);
    fixture_close(&fa);
    fixture_close(&fb);
    return 0;
}

static int
case_linkedit_rejects_out_of_segment(void)
{
    // nsyms claims more symbol bytes than the LINKEDIT segment can hold.
    {
        size_t size = 0;
        uint8_t *bytes = build_linkedit_file(&size, "_main", "_helper",
                                             TEXT_VMADDR, LINKEDIT_VMADDR, 0x1000, 1, 0);
        TASK_CHECK(bytes != NULL);

        fixture f;
        TASK_CHECK(fixture_open(&f, bytes, size, 0) == MK_ESUCCESS);
        free(bytes);

        mk_symbol_table_t symbol_table;
        TASK_CHECK(mk_symbol_table_init_with_segment(&f.linkedit, &symbol_table) != MK_ESUCCESS);

        fixture_close(&f);
    }

    // strsize places the string table outside the LINKEDIT segment.
    {
        size_t size = 0;
        uint8_t *bytes = build_linkedit_file(&size, "_main", "_helper",
                                             TEXT_VMADDR, LINKEDIT_VMADDR, 0x1000, 0, 1);
        TASK_CHECK(bytes != NULL);

        fixture f;
        TASK_CHECK(fixture_open(&f, bytes, size, 0) == MK_ESUCCESS);
        free(bytes);

        mk_string_table_t string_table;
        TASK_CHECK(mk_string_table_init_with_segment(&f.linkedit, &string_table) != MK_ESUCCESS);

        fixture_close(&f);
    }

    return 0;
}

//----------------------------------------------------------------------------//
#pragma mark -  Registration
//----------------------------------------------------------------------------//

const struct task_case task_cases[] = {
    { "coordinates", "header_address",        case_coordinates_header_address },
    { "coordinates", "file_and_zero_fill",    case_coordinates_file_and_zero_fill },
    { "coordinates", "adjacent_crossing",     case_coordinates_adjacent_crossing },
    { "coordinates", "holes",                 case_coordinates_holes },
    { "coordinates", "pagezero",              case_coordinates_pagezero },
    { "coordinates", "offset_arithmetic",     case_coordinates_offset_arithmetic },
    { "coordinates", "reject_segments",       case_coordinates_rejects_invalid_segments },
    { "coordinates", "reject_unsupported",    case_coordinates_rejects_unsupported_images },

    { "overlays",    "local_view",            case_overlays_local_view },
    { "overlays",    "remap_roundtrip",       case_overlays_remap_roundtrip },
    { "overlays",    "read_only",             case_overlays_read_only },
    { "overlays",    "multiple_objects",      case_overlays_multiple_objects },
    { "overlays",    "two_files_independent", case_overlays_two_files_independent },

    { "linkedit",    "symbols_and_strings",   case_linkedit_symbols_and_strings },
    { "linkedit",    "shifted_coordinates",   case_linkedit_shifted_file_coordinates },
    { "linkedit",    "two_images_independent", case_linkedit_two_images_independent },
    { "linkedit",    "reject_out_of_segment", case_linkedit_rejects_out_of_segment },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
