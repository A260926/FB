//----------------------------------------------------------------------------//
//|
//|  file_map_regression.c
//|
//|  Regression tests for the read-only file image memory map
//|  (mk_memory_map_file_t).  The tests build small synthetic little-endian
//|  thin 64-bit Mach-O images in memory; no files are downloaded and no
//|  image is modified or executed.
//|
//|  Groups:
//|    coordinates - virtual/file coordinate translation, holes, adjacency
//|    overlays    - file-byte/zero overlays and object/map independence
//|    linkedit    - symoff/stroff wiring through the stock __LINKEDIT APIs
//|
//----------------------------------------------------------------------------//

#include "task_support/check.h"

#include "macho.h"

#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//----------------------------------------------------------------------------//
#pragma mark -  Fixture Helpers
//----------------------------------------------------------------------------//

struct seg_spec {
    const char *name;
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
};

// Writes a little-endian thin 64-bit Mach-O header followed by one
// LC_SEGMENT_64 per spec.  The buffer is filled with a deterministic pattern
// first so file backed bytes are recognizable.
static uint8_t*
make_macho64_image(size_t size, uint8_t fill, const struct seg_spec *segs, unsigned nseg)
{
    uint8_t *bytes = malloc(size);
    if (!bytes) return NULL;

    for (size_t i = 0; i < size; ++i)
        bytes[i] = (uint8_t)(fill + (uint8_t)((i * 31u) & 0x1f));

    struct mach_header_64 *h = (struct mach_header_64*)bytes;
    memset(h, 0, sizeof(*h));
    h->magic = MH_MAGIC_64;
    h->cputype = CPU_TYPE_X86_64;
    h->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
    h->filetype = MH_DYLIB;
    h->ncmds = nseg;
    h->sizeofcmds = nseg * (uint32_t)sizeof(struct segment_command_64);

    for (unsigned i = 0; i < nseg; ++i) {
        struct segment_command_64 *s =
            (struct segment_command_64*)(bytes + sizeof(struct mach_header_64) + i * sizeof(*s));
        memset(s, 0, sizeof(*s));
        s->cmd = LC_SEGMENT_64;
        s->cmdsize = sizeof(*s);
        if (segs[i].name)
            strncpy(s->segname, segs[i].name, sizeof(s->segname));
        s->vmaddr = segs[i].vmaddr;
        s->vmsize = segs[i].vmsize;
        s->fileoff = segs[i].fileoff;
        s->filesize = segs[i].filesize;
        s->maxprot = VM_PROT_READ | VM_PROT_WRITE;
        s->initprot = VM_PROT_READ;
    }

    return bytes;
}

// Standard coordinate fixture:
//
//   __PAGEZERO [0x00000, 0x10000)            no mapping
//   __TEXT     [0x10000, 0x12000) file [0x0000, 0x1000)  (half zero fill)
//   __ADJACENT [0x12000, 0x13000) file [0x2000, 0x3000)  (virtual-adjacent)
//              hole     [0x13000, 0x40000)
//   __FAR      [0x40000, 0x41000) file [0x3000, 0x4000)
#define COORD_SIZE      0x4000
#define COORD_TEXT      0x10000ULL
#define COORD_TEXT_VM   0x2000ULL
#define COORD_TEXT_FILE 0x1000ULL
#define COORD_ADJ       0x12000ULL
#define COORD_FAR       0x40000ULL

static uint8_t*
make_coordinates_image(void)
{
    const struct seg_spec segs[] = {
        { SEG_PAGEZERO, 0x00000, 0x10000, 0x0000, 0x0000 },
        { SEG_TEXT,     0x10000, 0x02000, 0x0000, 0x1000 },
        { "__ADJ",      0x12000, 0x01000, 0x2000, 0x1000 },
        { "__FAR",      0x40000, 0x01000, 0x3000, 0x1000 },
    };
    return make_macho64_image(COORD_SIZE, 0x40, segs, sizeof(segs)/sizeof(segs[0]));
}

//----------------------------------------------------------------------------//
#pragma mark -  coordinates
//----------------------------------------------------------------------------//

// Header address and basic file-coordinate mapping plus remap/unmap.
static int
coordinates_header(void)
{
    size_t size = COORD_SIZE;
    uint8_t *bytes = make_coordinates_image();
    TASK_CHECK(bytes != NULL);

    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, size, 0, &map);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_file_get_header_address(&map) == COORD_TEXT);

    mk_memory_object_t obj;
    err = mk_memory_map_init_object(&map, 0, COORD_TEXT, 0x800, true, &obj);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_length(&obj) == 0x800);
    TASK_CHECK(mk_memory_object_target_address(&obj) == COORD_TEXT);
    TASK_CHECK(mk_memory_object_address(&obj) != 0);
    TASK_CHECK(memcmp((void*)mk_memory_object_address(&obj), bytes + 0, 0x800) == 0);

    // remap target -> local, then unmap local -> target must round trip.
    mk_error_t map_err = MK_ESUCCESS;
    vm_address_t local = mk_memory_object_remap_address(&obj, 0, COORD_TEXT + 0x100, 4, &map_err);
    TASK_CHECK(map_err == MK_ESUCCESS);
    TASK_CHECK(local != UINTPTR_MAX);
    TASK_CHECK(local == mk_memory_object_address(&obj) + 0x100);
    TASK_CHECK(memcmp((void*)local, bytes + 0x100, 4) == 0);

    mk_vm_address_t back = mk_memory_object_unmap_address(&obj, 0, local, 4, &map_err);
    TASK_CHECK(map_err == MK_ESUCCESS);
    TASK_CHECK(back == COORD_TEXT + 0x100);

    mk_memory_map_free_object(&map, &obj);
    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

// The tail where filesize < vmsize reads as zero fill and remains contiguous.
static int
coordinates_zero_fill(void)
{
    size_t size = COORD_SIZE;
    uint8_t *bytes = make_coordinates_image();
    TASK_CHECK(bytes != NULL);

    mk_memory_map_file_t map;
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes, size, 0, &map) == MK_ESUCCESS);

    mk_memory_object_t obj;
    TASK_CHECK(mk_memory_map_init_object(&map, 0, COORD_TEXT, COORD_TEXT_VM, true, &obj) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_length(&obj) == COORD_TEXT_VM);

    const uint8_t *local = (const uint8_t*)mk_memory_object_address(&obj);
    TASK_CHECK(memcmp(local, bytes, COORD_TEXT_FILE) == 0);

    bool all_zero = true;
    for (uint64_t i = COORD_TEXT_FILE; i < COORD_TEXT_VM; ++i) {
        if (local[i] != 0) { all_zero = false; break; }
    }
    TASK_CHECK(all_zero);
    TASK_CHECK(local[COORD_TEXT_VM - 1] == 0);

    // The zero region participates in remap/unmap round trips.
    vm_address_t z = mk_memory_object_remap_address(&obj, 0, COORD_TEXT + 0x1800, 8, NULL);
    TASK_CHECK(z != UINTPTR_MAX);
    TASK_CHECK(z == mk_memory_object_address(&obj) + 0x1800);
    TASK_CHECK(((const uint8_t*)z)[0] == 0 && ((const uint8_t*)z)[7] == 0);
    mk_vm_address_t zback = mk_memory_object_unmap_address(&obj, 0, z, 8, NULL);
    TASK_CHECK(zback == COORD_TEXT + 0x1800);

    mk_memory_map_free_object(&map, &obj);
    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

// One read may cross adjacent virtual segments even though their file
// positions are not contiguous.
static int
coordinates_adjacent_crossing(void)
{
    size_t size = COORD_SIZE;
    uint8_t *bytes = make_coordinates_image();
    TASK_CHECK(bytes != NULL);

    mk_memory_map_file_t map;
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes, size, 0, &map) == MK_ESUCCESS);

    // [0x10F00, 0x12F00): 0x100 file tail + 0x1000 zero fill + 0xF00 __ADJ.
    const mk_vm_address_t start = COORD_TEXT + 0xF00;
    const mk_vm_size_t length = 0x2000;

    uint8_t *expected = calloc(1, (size_t)length);
    TASK_CHECK(expected != NULL);
    memcpy(expected + 0x0000, bytes + 0x0F00, 0x100);
    // expected[0x100 .. 0x1100) stays zero.
    memcpy(expected + 0x1100, bytes + 0x2000, 0xF00);

    mk_memory_object_t obj;
    mk_error_t err = mk_memory_map_init_object(&map, 0, start, length, true, &obj);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_length(&obj) == length);
    TASK_CHECK(memcmp((void*)mk_memory_object_address(&obj), expected, (size_t)length) == 0);

    // A pointer into the second segment unmaps back to the right target.
    vm_address_t p = mk_memory_object_remap_address(&obj, 0, COORD_ADJ + 0x10, 1, NULL);
    TASK_CHECK(p == mk_memory_object_address(&obj) + 0x1110);
    mk_vm_address_t t = mk_memory_object_unmap_address(&obj, 0, p, 1, NULL);
    TASK_CHECK(t == COORD_ADJ + 0x10);

    mk_memory_map_free_object(&map, &obj);
    free(expected);
    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

// Holes terminate full mappings with failure and short mappings return only
// the contiguous prefix; __PAGEZERO is unmapped.
static int
coordinates_holes_and_pagezero(void)
{
    size_t size = COORD_SIZE;
    uint8_t *bytes = make_coordinates_image();
    TASK_CHECK(bytes != NULL);

    mk_memory_map_file_t map;
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes, size, 0, &map) == MK_ESUCCESS);

    mk_memory_object_t obj;

    // Extending past __ADJ into the hole: full request fails.
    mk_error_t err = mk_memory_map_init_object(&map, 0, COORD_ADJ + 0xF00, 0x200, true, &obj);
    TASK_CHECK(err != MK_ESUCCESS);

    // Short request returns exactly the 0x100 contiguous prefix.
    err = mk_memory_map_init_object(&map, 0, COORD_ADJ + 0xF00, 0x200, false, &obj);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_length(&obj) == 0x100);
    TASK_CHECK(memcmp((void*)mk_memory_object_address(&obj), bytes + 0x2F00, 0x100) == 0);
    mk_memory_map_free_object(&map, &obj);

    // Starting inside the hole yields nothing, even for a short mapping.
    TASK_CHECK(mk_memory_map_init_object(&map, 0, COORD_ADJ + 0x1000, 0x10, true, &obj) != MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_init_object(&map, 0, COORD_ADJ + 0x1000, 0x10, false, &obj) != MK_ESUCCESS);

    // The far segment is independently reachable; no hole memory is exposed.
    err = mk_memory_map_init_object(&map, 0, COORD_FAR, 0x1000, true, &obj);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_length(&obj) == 0x1000);
    TASK_CHECK(memcmp((void*)mk_memory_object_address(&obj), bytes + 0x3000, 0x1000) == 0);
    mk_memory_map_free_object(&map, &obj);

    // One byte past the far segment is unmapped.
    TASK_CHECK(mk_memory_map_init_object(&map, 0, COORD_FAR + 0x1000, 1, true, &obj) != MK_ESUCCESS);

    // __PAGEZERO provides no mapping at all.
    TASK_CHECK(mk_memory_map_init_object(&map, 0, 0x1000, 4, true, &obj) != MK_ESUCCESS);
    mk_error_t has_err = MK_ESUCCESS;
    TASK_CHECK(mk_memory_map_has_mapping(&map, 0, 0, 0x1000, &has_err) == false);

    // Offset/address overflow follows the standard map rules.
    TASK_CHECK(mk_memory_map_init_object(&map, 1, UINT64_MAX, 4, true, &obj) == MK_EOVERFLOW);

    // copy_bytes reports only the contiguous prefix for a short read.
    {
        unsigned char buf[0x300] = {0};
        mk_error_t cp_err = MK_ESUCCESS;
        size_t got = mk_memory_map_copy_bytes(&map, 0, COORD_ADJ + 0xF00, buf, sizeof(buf), false, &cp_err);
        TASK_CHECK(got == 0x100);
        TASK_CHECK(memcmp(buf, bytes + 0x2F00, 0x100) == 0);
        // A full copy across the hole fails and reports zero.
        TASK_CHECK(mk_memory_map_copy_bytes(&map, 0, COORD_ADJ + 0xF00, buf, 0x200, true, &cp_err) == 0);
        TASK_CHECK(cp_err != MK_ESUCCESS);
    }

    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

// Three mutually adjacent virtual segments are crossed in one object, and
// unsorted load command order does not affect the coordinate view.
static int
coordinates_multi_adjacent_and_order(void)
{
    const struct seg_spec segs[] = {
        { "__C", 0x12000, 0x1000, 0x2000, 0x1000 },
        { "__A", 0x10000, 0x1000, 0x0000, 0x1000 },
        { "__B", 0x11000, 0x1000, 0x1000, 0x1000 },
    };
    uint8_t *bytes = make_macho64_image(0x3000, 0x60, segs, 3);
    TASK_CHECK(bytes != NULL);

    mk_memory_map_file_t map;
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes, 0x3000, 0, &map) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_file_get_header_address(&map) == 0x10000);

    mk_memory_object_t obj;
    TASK_CHECK(mk_memory_map_init_object(&map, 0, 0x10F00, 0x1200, true, &obj) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_length(&obj) == 0x1200);

    uint8_t *expected = calloc(1, 0x1200);
    TASK_CHECK(expected != NULL);
    memcpy(expected + 0x0000, bytes + 0x0F00, 0x100); // tail of __A file 0
    memcpy(expected + 0x0100, bytes + 0x1000, 0x1000); // all of __B file 0x1000
    memcpy(expected + 0x1100, bytes + 0x2000, 0x100); // head of __C file 0x2000
    TASK_CHECK(memcmp((void*)mk_memory_object_address(&obj), expected, 0x1200) == 0);

    // has_mapping agrees with the full contiguous range.
    TASK_CHECK(mk_memory_map_has_mapping(&map, 0, 0x10000, 0x3000, NULL) == true);
    TASK_CHECK(mk_memory_map_has_mapping(&map, 0, 0x10000, 0x3001, NULL) == false);

    mk_memory_map_free_object(&map, &obj);
    free(expected);
    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

// A non-zero slide shifts the header address and every virtual coordinate.
static int
coordinates_slide(void)
{
    const mk_vm_slide_t slide = 0x5000;
    size_t size = COORD_SIZE;
    uint8_t *bytes = make_coordinates_image();
    TASK_CHECK(bytes != NULL);

    mk_memory_map_file_t map;
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes, size, slide, &map) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_file_get_header_address(&map) == COORD_TEXT + (uint64_t)slide);

    mk_memory_object_t obj;
    TASK_CHECK(mk_memory_map_init_object(&map, 0, COORD_TEXT + slide, 0x800, true, &obj) == MK_ESUCCESS);
    TASK_CHECK(memcmp((void*)mk_memory_object_address(&obj), bytes, 0x800) == 0);
    mk_memory_map_free_object(&map, &obj);

    // Adjacent crossing works at slid addresses too.
    TASK_CHECK(mk_memory_map_init_object(&map, 0, COORD_TEXT + 0xF00 + slide, 0x2000, true, &obj) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_length(&obj) == 0x2000);
    mk_memory_map_free_object(&map, &obj);

    // Unslid addresses are not mapped by a slid map.
    TASK_CHECK(mk_memory_map_init_object(&map, 0, COORD_TEXT, 4, true, &obj) != MK_ESUCCESS);

    mk_memory_map_file_free(&map);

    // Negative slides are rejected.
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes, size, -1, &map) == MK_EINVAL);

    free(bytes);
    return 0;
}

// Invalid structure/geometry inputs must be rejected by init.
static int
coordinates_reject_inputs(void)
{
    mk_memory_map_file_t map;

    // NULL / too small.
    TASK_CHECK(mk_memory_map_file_init(NULL, NULL, 16, 0, &map) == MK_EINVAL);
    uint8_t tiny[16] = {0};
    TASK_CHECK(mk_memory_map_file_init(NULL, tiny, sizeof(tiny), 0, &map) != MK_ESUCCESS);

    // Bad magic (not little-endian MH_MAGIC_64).
    {
        size_t size = COORD_SIZE;
        uint8_t *bytes = make_coordinates_image();
        TASK_CHECK(bytes != NULL);
        bytes[0] ^= 0xFF;
        TASK_CHECK(mk_memory_map_file_init(NULL, bytes, size, 0, &map) == MK_EINVALID_DATA);
        free(bytes);
    }

    // File range runs past the end of the provided bytes.
    {
        const struct seg_spec segs[] = {
            { SEG_TEXT, 0x10000, 0x2000, 0x3000, 0x1000 },
        };
        uint8_t *bytes = make_macho64_image(0x2000, 0x10, segs, 1);
        TASK_CHECK(bytes != NULL);
        TASK_CHECK(mk_memory_map_file_init(NULL, bytes, 0x2000, 0, &map) != MK_ESUCCESS);
        free(bytes);
    }

    // filesize greater than vmsize.
    {
        const struct seg_spec segs[] = {
            { SEG_TEXT, 0x10000, 0x1000, 0x0000, 0x2000 },
        };
        uint8_t *bytes = make_macho64_image(0x4000, 0x20, segs, 1);
        TASK_CHECK(bytes != NULL);
        TASK_CHECK(mk_memory_map_file_init(NULL, bytes, 0x4000, 0, &map) == MK_EINVAL);
        free(bytes);
    }

    // Overlapping non-empty virtual segments.
    {
        const struct seg_spec segs[] = {
            { "__A", 0x1000, 0x2000, 0, 0 },
            { "__B", 0x1800, 0x1000, 0, 0 },
        };
        uint8_t *bytes = make_macho64_image(0x1000, 0x30, segs, 2);
        TASK_CHECK(bytes != NULL);
        TASK_CHECK(mk_memory_map_file_init(NULL, bytes, 0x1000, 0, &map) == MK_EINVAL);
        free(bytes);
    }

    return 0;
}

//----------------------------------------------------------------------------//
#pragma mark -  overlays
//----------------------------------------------------------------------------//

#define OVERLAY_VMADDR 0x70000ULL
#define OVERLAY_VMSIZE 0x4000ULL
#define OVERLAY_FILE   0x2000ULL

static uint8_t*
make_overlay_image(uint8_t fill)
{
    const struct seg_spec segs[] = {
        { SEG_TEXT, OVERLAY_VMADDR, OVERLAY_VMSIZE, 0, OVERLAY_FILE },
    };
    return make_macho64_image(OVERLAY_FILE, fill, segs, 1);
}

// Several objects live at once; freeing one leaves the others valid.
static int
overlays_concurrent_objects(void)
{
    uint8_t *bytes = make_overlay_image(0x50);
    TASK_CHECK(bytes != NULL);

    mk_memory_map_file_t map;
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes, OVERLAY_FILE, 0, &map) == MK_ESUCCESS);

    mk_memory_object_t whole, backed, zeroed;
    TASK_CHECK(mk_memory_map_init_object(&map, 0, OVERLAY_VMADDR, OVERLAY_VMSIZE, true, &whole) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_init_object(&map, 0, OVERLAY_VMADDR, OVERLAY_FILE, true, &backed) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_init_object(&map, 0, OVERLAY_VMADDR + OVERLAY_FILE, OVERLAY_FILE, true, &zeroed) == MK_ESUCCESS);

    vm_address_t a_whole = mk_memory_object_address(&whole);
    vm_address_t a_backed = mk_memory_object_address(&backed);
    vm_address_t a_zero = mk_memory_object_address(&zeroed);
    TASK_CHECK(a_whole != a_backed);
    TASK_CHECK(a_whole != a_zero);
    TASK_CHECK(a_backed != a_zero);

    TASK_CHECK(memcmp((void*)a_whole, bytes, OVERLAY_FILE) == 0);
    TASK_CHECK(memcmp((void*)a_backed, bytes, 0x800) == 0);
    bool zero_ok = true;
    for (uint64_t i = 0; i < OVERLAY_FILE; ++i) {
        if (((const uint8_t*)a_zero)[i] != 0) { zero_ok = false; break; }
    }
    TASK_CHECK(zero_ok);

    // Free the middle object; the other two must remain fully usable.
    mk_memory_map_free_object(&map, &backed);

    TASK_CHECK(memcmp((void*)a_whole, bytes, OVERLAY_FILE) == 0);
    TASK_CHECK(((const uint8_t*)a_whole)[OVERLAY_VMSIZE - 1] == 0);
    TASK_CHECK(((const uint8_t*)a_zero)[123] == 0);

    vm_address_t rt = mk_memory_object_remap_address(&whole, 0, OVERLAY_VMADDR + 0x10, 4, NULL);
    TASK_CHECK(rt == a_whole + 0x10);
    TASK_CHECK(memcmp((void*)rt, bytes + 0x10, 4) == 0);

    mk_memory_map_free_object(&map, &whole);
    mk_memory_map_free_object(&map, &zeroed);
    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

// Two maps with identical preferred addresses never share bytes or zero fill.
static int
overlays_two_independent_maps(void)
{
    uint8_t *bytes_a = make_overlay_image(0x33);
    uint8_t *bytes_b = make_overlay_image(0xCC);
    TASK_CHECK(bytes_a != NULL && bytes_b != NULL);

    mk_memory_map_file_t map_a, map_b;
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes_a, OVERLAY_FILE, 0, &map_a) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes_b, OVERLAY_FILE, 0, &map_b) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_file_get_header_address(&map_a) == mk_memory_map_file_get_header_address(&map_b));

    mk_memory_object_t a, b;
    TASK_CHECK(mk_memory_map_init_object(&map_a, 0, OVERLAY_VMADDR + 0x400, 0x100, true, &a) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_init_object(&map_b, 0, OVERLAY_VMADDR + 0x400, 0x100, true, &b) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_address(&a) != mk_memory_object_address(&b));

    const uint8_t *pa = (const uint8_t*)mk_memory_object_address(&a);
    const uint8_t *pb = (const uint8_t*)mk_memory_object_address(&b);
    TASK_CHECK(pa[0] == (uint8_t)(0x33 + (uint8_t)((0x400u * 31u) & 0x1f)));
    TASK_CHECK(pb[0] == (uint8_t)(0xCC + (uint8_t)((0x400u * 31u) & 0x1f)));
    TASK_CHECK(pa[0] != pb[0]);

    mk_memory_object_t za, zb;
    TASK_CHECK(mk_memory_map_init_object(&map_a, 0, OVERLAY_VMADDR + OVERLAY_FILE, 0x800, true, &za) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_init_object(&map_b, 0, OVERLAY_VMADDR + OVERLAY_FILE, 0x800, true, &zb) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_address(&za) != mk_memory_object_address(&zb));
    TASK_CHECK(((const uint8_t*)mk_memory_object_address(&za))[7] == 0);
    TASK_CHECK(((const uint8_t*)mk_memory_object_address(&zb))[7] == 0);

    mk_memory_map_free_object(&map_a, &a);
    mk_memory_map_free_object(&map_b, &b);
    mk_memory_map_free_object(&map_a, &za);
    mk_memory_map_free_object(&map_b, &zb);
    mk_memory_map_file_free(&map_a);
    mk_memory_map_file_free(&map_b);
    free(bytes_a);
    free(bytes_b);
    return 0;
}

// The caller's input bytes may be released immediately after init.
static int
overlays_input_can_release(void)
{
    uint8_t *bytes = make_overlay_image(0x5A);
    TASK_CHECK(bytes != NULL);

    mk_memory_map_file_t map;
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes, OVERLAY_FILE, 0, &map) == MK_ESUCCESS);

    // Mutating the caller's buffer after init must not change the snapshot.
    memset(bytes, 0xEE, OVERLAY_FILE);
    mk_memory_object_t probe;
    TASK_CHECK(mk_memory_map_init_object(&map, 0, OVERLAY_VMADDR + 0x400, 16, true, &probe) == MK_ESUCCESS);
    TASK_CHECK(((const uint8_t*)mk_memory_object_address(&probe))[0] != 0xEE);
    mk_memory_map_free_object(&map, &probe);

    // Release the caller-owned input before touching the map.
    free(bytes);
    bytes = NULL;

    mk_memory_object_t obj;
    TASK_CHECK(mk_memory_map_init_object(&map, 0, OVERLAY_VMADDR + 0x400, 16, true, &obj) == MK_ESUCCESS);
    const uint8_t *p = (const uint8_t*)mk_memory_object_address(&obj);
    uint8_t expected0 = (uint8_t)(0x5A + (uint8_t)((0x400u * 31u) & 0x1f));
    TASK_CHECK(p[0] == expected0);
    TASK_CHECK(p[1] == (uint8_t)(expected0 + 31));
    mk_memory_map_free_object(&map, &obj);

    // The stock mk_macho entry point works from the snapshot as well.
    mk_macho_t image;
    mk_error_t err = mk_macho_init_with_slide(NULL, "released-input", 0,
                                              mk_memory_map_file_get_header_address(&map),
                                              &map, &image);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_macho_is_64_bit(&image));

    struct load_command *lc = mk_macho_next_command_type(&image, NULL, LC_SEGMENT_64, NULL);
    TASK_CHECK(lc != NULL);
    mk_segment_t segment;
    TASK_CHECK(mk_segment_init_with_mach_load_command(&image, lc, &segment) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_length(mk_segment_get_mapping(&segment)) == OVERLAY_VMSIZE);

    mk_segment_free(&segment);
    mk_macho_free(&image);
    mk_memory_map_file_free(&map);
    return 0;
}

//----------------------------------------------------------------------------//
#pragma mark -  linkedit
//----------------------------------------------------------------------------//

#define LE_FILE_SIZE    0x3000
#define LE_BASE_VMADDR  0x100000000ULL
#define LE_TEXT_VMADDR  0x100000000ULL
#define LE_EDIT_VMADDR  0x100004000ULL
#define LE_EDIT_VMSIZE  0x2000ULL
#define LE_EDIT_FILEOFF 0x2000ULL
#define LE_EDIT_FILESZ  0x1000ULL
#define LE_SYMOFF       0x2040u
#define LE_STROFF       0x2200u
#define LE_STRSIZE      0x40u

// Builds a __TEXT + __LINKEDIT image containing two nlist_64 symbols and a
// string table with the supplied names.
static uint8_t*
make_linkedit_image(const char *first, const char *second)
{
    uint8_t *bytes = calloc(1, LE_FILE_SIZE);
    if (!bytes) return NULL;

    struct mach_header_64 *h = (struct mach_header_64*)bytes;
    h->magic = MH_MAGIC_64;
    h->cputype = CPU_TYPE_X86_64;
    h->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
    h->filetype = MH_DYLIB;
    h->ncmds = 3;
    h->sizeofcmds = 2 * (uint32_t)sizeof(struct segment_command_64) + (uint32_t)sizeof(struct symtab_command);

    struct segment_command_64 *text = (struct segment_command_64*)(bytes + sizeof(*h));
    text->cmd = LC_SEGMENT_64;
    text->cmdsize = sizeof(*text);
    strncpy(text->segname, SEG_TEXT, sizeof(text->segname));
    text->vmaddr = LE_TEXT_VMADDR;
    text->vmsize = 0x2000;
    text->fileoff = 0;
    text->filesize = 0x2000;
    text->initprot = VM_PROT_READ;
    text->maxprot = VM_PROT_READ;

    struct segment_command_64 *edit = (struct segment_command_64*)((uint8_t*)text + sizeof(*text));
    edit->cmd = LC_SEGMENT_64;
    edit->cmdsize = sizeof(*edit);
    strncpy(edit->segname, SEG_LINKEDIT, sizeof(edit->segname));
    edit->vmaddr = LE_EDIT_VMADDR;
    edit->vmsize = LE_EDIT_VMSIZE;
    edit->fileoff = LE_EDIT_FILEOFF;
    edit->filesize = LE_EDIT_FILESZ;
    edit->initprot = VM_PROT_READ;
    edit->maxprot = VM_PROT_READ;

    struct symtab_command *symtab = (struct symtab_command*)((uint8_t*)edit + sizeof(*edit));
    symtab->cmd = LC_SYMTAB;
    symtab->cmdsize = sizeof(*symtab);
    symtab->symoff = LE_SYMOFF;
    symtab->nsyms = 2;
    symtab->stroff = LE_STROFF;
    symtab->strsize = LE_STRSIZE;

    // String table bytes.  Offset 0 is reserved as an empty string.
    char *strings = (char*)(bytes + LE_STROFF);
    strings[0] = '\0';
    uint32_t off_first = 1;
    strcpy(strings + off_first, first);
    uint32_t off_second = off_first + (uint32_t)strlen(first) + 1;
    strcpy(strings + off_second, second);

    struct nlist_64 *syms = (struct nlist_64*)(bytes + LE_SYMOFF);
    memset(syms, 0, 2 * sizeof(*syms));
    syms[0].n_un.n_strx = off_first;
    syms[0].n_type = N_SECT;
    syms[0].n_sect = 1;
    syms[0].n_value = 0x1000;
    syms[1].n_un.n_strx = off_second;
    syms[1].n_type = N_SECT;
    syms[1].n_sect = 1;
    syms[1].n_value = 0x2000;

    return bytes;
}

// Locates __LINKEDIT and runs the stock symbol/string table consumers.
static int
linkedit_read_names(mk_memory_map_file_t *map, mk_vm_slide_t slide,
                    const char *expected0, const char *expected1)
{
    mk_macho_t image;
    mk_error_t err = mk_macho_init_with_slide(NULL, "linkedit-fixture", slide,
                                              mk_memory_map_file_get_header_address(map),
                                              map, &image);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_macho_is_64_bit(&image));

    mk_segment_t linkedit;
    bool found_linkedit = false;
    struct load_command *lc = NULL;
    while ((lc = mk_macho_next_command_type(&image, lc, LC_SEGMENT_64, NULL)) != NULL) {
        mk_segment_t candidate;
        if (mk_segment_init_with_mach_load_command(&image, lc, &candidate) != MK_ESUCCESS)
            continue;

        char name[17] = {0};
        mk_segment_copy_name(&candidate, name);
        if (strcmp(name, SEG_LINKEDIT) == 0) {
            linkedit = candidate;
            found_linkedit = true;
            break;
        }
        mk_segment_free(&candidate);
    }
    TASK_CHECK(found_linkedit);

    // Stock __LINKEDIT symbol table entry point (consumes LC_SYMTAB itself).
    mk_symbol_table_t symbols;
    err = mk_symbol_table_init_with_segment(&linkedit, &symbols);
    TASK_CHECK(err == MK_ESUCCESS);
    TASK_CHECK(mk_symbol_table_get_symbol_count(&symbols) == 2);

    // Stock __LINKEDIT string table entry point (stroff coordinates).
    mk_string_table_t strings;
    err = mk_string_table_init_with_segment(&linkedit, &strings);
    TASK_CHECK(err == MK_ESUCCESS);

    const char *expected[2] = { expected0, expected1 };
    for (uint32_t i = 0; i < 2; ++i) {
        mk_macho_nlist_ptr nlist = mk_symbol_table_get_mach_symbol_at_index(&symbols, i, NULL);
        TASK_CHECK(nlist.any != NULL);

        mk_symbol_t symbol;
        TASK_CHECK(mk_symbol_init(&symbols, nlist, &symbol) == MK_ESUCCESS);
        TASK_CHECK(mk_symbol_get_value(&symbol) == (i == 0 ? 0x1000 : 0x2000));

        uint32_t strx = mk_symbol_get_strx(&symbol);
        const char *name = mk_string_table_get_string_at_offset(&strings, strx, NULL);
        TASK_CHECK(name != NULL);
        TASK_CHECK(strcmp(name, expected[i]) == 0);

        char copied[64] = {0};
        size_t n = mk_string_table_copy_string_at_offset(&strings, strx, copied, sizeof(copied) - 1);
        TASK_CHECK(n == strlen(expected[i]));
        TASK_CHECK(strcmp(copied, expected[i]) == 0);
    }

    mk_string_table_free(&strings);
    mk_symbol_table_free(&symbols);
    mk_segment_free(&linkedit);
    mk_macho_free(&image);
    return 0;
}

static int
linkedit_symbols_and_strings(void)
{
    uint8_t *bytes = make_linkedit_image("_alpha", "_beta");
    TASK_CHECK(bytes != NULL);

    mk_memory_map_file_t map;
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes, LE_FILE_SIZE, 0, &map) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_file_get_header_address(&map) == LE_BASE_VMADDR);

    int rc = linkedit_read_names(&map, 0, "_alpha", "_beta");
    TASK_CHECK(rc == 0);

    mk_memory_map_file_free(&map);
    free(bytes);
    return 0;
}

// Same preferred addresses, different bytes and a non-zero slide: the two
// images must resolve their own symbols/strings independently.
static int
linkedit_two_images_same_preferred_address(void)
{
    uint8_t *bytes_a = make_linkedit_image("_aaa_symbol", "_bbb_symbol");
    uint8_t *bytes_b = make_linkedit_image("_gamma", "_delta");
    TASK_CHECK(bytes_a != NULL && bytes_b != NULL);

    mk_memory_map_file_t map_a, map_b;
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes_a, LE_FILE_SIZE, 0, &map_a) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_map_file_init(NULL, bytes_b, LE_FILE_SIZE, 0x200000, &map_b) == MK_ESUCCESS);

    // Identical preferred header addresses before the slide.
    TASK_CHECK(mk_memory_map_file_get_header_address(&map_a) == LE_BASE_VMADDR);
    TASK_CHECK(mk_memory_map_file_get_header_address(&map_b) == LE_BASE_VMADDR + 0x200000);

    TASK_CHECK(linkedit_read_names(&map_a, 0, "_aaa_symbol", "_bbb_symbol") == 0);
    TASK_CHECK(linkedit_read_names(&map_b, 0x200000, "_gamma", "_delta") == 0);
    // Re-check A after B was exercised.
    TASK_CHECK(linkedit_read_names(&map_a, 0, "_aaa_symbol", "_bbb_symbol") == 0);

    mk_memory_map_file_free(&map_a);
    mk_memory_map_file_free(&map_b);
    free(bytes_a);
    free(bytes_b);
    return 0;
}

//----------------------------------------------------------------------------//
#pragma mark -  Short Call Example
//----------------------------------------------------------------------------//

// This is the minimal intended usage pattern: raw Mach-O bytes -> file memory
// map -> header address -> mk_macho -> segment inspection.  It is registered
// as a regression case so the example is always compiled and executed.
static int
example_short_usage(void)
{
    const struct seg_spec segs[] = {
        { SEG_TEXT, 0x10000, 0x1000, 0, 0x1000 },
    };
    uint8_t *file_bytes = make_macho64_image(0x1000, 0x70, segs, 1);
    TASK_CHECK(file_bytes != NULL);

    mk_memory_map_file_t file_map;
    mk_error_t err = mk_memory_map_file_init(NULL, file_bytes, 0x1000, 0, &file_map);
    TASK_CHECK(err == MK_ESUCCESS);

    // The map snapshots the bytes; the caller's buffer can go away now.
    free(file_bytes);

    mk_macho_t macho;
    err = mk_macho_init_with_slide(NULL, "example", 0,
                                   mk_memory_map_file_get_header_address(&file_map),
                                   &file_map, &macho);
    TASK_CHECK(err == MK_ESUCCESS);

    struct load_command *lc = mk_macho_next_command_type(&macho, NULL, LC_SEGMENT_64, NULL);
    TASK_CHECK(lc != NULL);

    mk_segment_t segment;
    TASK_CHECK(mk_segment_init_with_mach_load_command(&macho, lc, &segment) == MK_ESUCCESS);
    TASK_CHECK(mk_memory_object_length(mk_segment_get_mapping(&segment)) == 0x1000);

    mk_segment_free(&segment);
    mk_macho_free(&macho);
    mk_memory_map_file_free(&file_map);
    return 0;
}

// The same flow starting from a Mach-O on disk using POSIX/macOS file APIs.
// Compile-checked documentation of the intended caller pattern.
__attribute__((unused))
static int
example_read_macho_file_from_disk(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 1;

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) { fclose(file); return 1; }

    void *bytes = malloc((size_t)length);
    if (bytes == NULL) { fclose(file); return 1; }
    if (fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes); fclose(file); return 1;
    }
    fclose(file);

    mk_memory_map_file_t file_map;
    if (mk_memory_map_file_init(NULL, bytes, (size_t)length, 0, &file_map)) {
        free(bytes);
        return 1;
    }
    // The map has its own snapshot; the caller buffer is releasable now.
    free(bytes);

    mk_macho_t macho;
    mk_error_t err = mk_macho_init_with_slide(NULL, path, 0,
                                              mk_memory_map_file_get_header_address(&file_map),
                                              &file_map, &macho);
    if (err == MK_ESUCCESS) {
        /* ... mk_macho_next_command_type / segments / symbols ... */
        mk_macho_free(&macho);
    }
    mk_memory_map_file_free(&file_map);
    return err == MK_ESUCCESS ? 0 : 1;
}

//----------------------------------------------------------------------------//
#pragma mark -  Case Registry
//----------------------------------------------------------------------------//

const struct task_case task_cases[] = {
    { "coordinates", "header",            coordinates_header },
    { "coordinates", "zero_fill",         coordinates_zero_fill },
    { "coordinates", "adjacent_crossing", coordinates_adjacent_crossing },
    { "coordinates", "holes_pagezero",    coordinates_holes_and_pagezero },
    { "coordinates", "multi_adjacent",    coordinates_multi_adjacent_and_order },
    { "coordinates", "slide",             coordinates_slide },
    { "coordinates", "reject_inputs",     coordinates_reject_inputs },
    { "coordinates", "example_usage",     example_short_usage },

    { "overlays", "concurrent_objects",       overlays_concurrent_objects },
    { "overlays", "two_independent_maps",     overlays_two_independent_maps },
    { "overlays", "input_can_release",        overlays_input_can_release },

    { "linkedit", "symbols_and_strings",                  linkedit_symbols_and_strings },
    { "linkedit", "two_images_same_preferred_address",    linkedit_two_images_same_preferred_address },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
