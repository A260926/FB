//----------------------------------------------------------------------------//
//|
//|             MachOKit - A Lightweight Mach-O Parsing Library
//! @file       memory_map_file.c
//!
//! @copyright  Copyright (c) 2026. All rights reserved.
//|
//| Permission is hereby granted, free of charge, to any person obtaining a
//| copy of this software and associated documentation files (the "Software"),
//| to deal in the Software without restriction, including without limitation
//| the rights to use, copy, modify, merge, publish, distribute, sublicense,
//| and/or sell copies of the Software, and to permit persons to whom the
//| Software is furnished to do so, subject to the following conditions:
//|
//| The above copyright notice and this permission notice shall be included
//| in all copies or substantial portions of the Software.
//|
//| THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//| OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//| FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
//| THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//| LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//| FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//| DEALINGS IN THE SOFTWARE.
//----------------------------------------------------------------------------//

#include "core_internal.h"

#include <stdlib.h>
#include <string.h>

#include <mach-o/loader.h>

//----------------------------------------------------------------------------//
#pragma mark -  Segment Descriptors
//----------------------------------------------------------------------------//

//|++++++++++++++++++++++++++++++++++++|//
//! A single LC_SEGMENT_64 described in file coordinates and in the slid
//! virtual address space presented by this map.
struct mk_memory_map_file_segment_s {
    //! Preferred (unslid) virtual start address.
    mk_vm_address_t vmaddr;
    //! Preferred (unslid) virtual end address, exclusive.
    mk_vm_address_t vmend;
    //! Slid virtual start address.
    mk_vm_address_t start;
    //! Slid virtual end address, exclusive.
    mk_vm_address_t end;
    //! File offset of the segment's backed bytes.
    mk_vm_offset_t fileoff;
    //! Number of file backed bytes.  Never greater than \ref vmsize.
    mk_vm_size_t filesize;
    //! Total virtual size, including zero filled bytes.
    mk_vm_size_t vmsize;
    //! True for the __PAGEZERO segment, which is never mapped.
    bool pagezero;
};

//----------------------------------------------------------------------------//
#pragma mark -  Helpers
//----------------------------------------------------------------------------//

//|++++++++++++++++++++++++++++++++++++|//
//! Returns the first mapped (non-empty, non-\c __PAGEZERO) segment containing
//! \a address, or \c NULL if the address lies in a hole.
static struct mk_memory_map_file_segment_s*
__mk_memory_map_file_segment_containing(mk_memory_map_file_t *file_map, mk_vm_address_t address)
{
    for (size_t i = 0; i < file_map->segment_count; ++i) {
        struct mk_memory_map_file_segment_s *segment = &file_map->segments[i];

        if (segment->pagezero || segment->vmsize == 0)
            continue;

        if (address >= segment->start && address < segment->end)
            return segment;
    }
    return NULL;
}

//|++++++++++++++++++++++++++++++++++++|//
//! Returns the mapped segment whose virtual range begins at \a address.
static struct mk_memory_map_file_segment_s*
__mk_memory_map_file_segment_starting_at(mk_memory_map_file_t *file_map, mk_vm_address_t address)
{
    for (size_t i = 0; i < file_map->segment_count; ++i) {
        struct mk_memory_map_file_segment_s *segment = &file_map->segments[i];

        if (segment->pagezero || segment->vmsize == 0)
            continue;

        if (segment->start == address)
            return segment;
    }
    return NULL;
}

//|++++++++++++++++++++++++++++++++++++|//
//! Computes the number of contiguous mapped virtual bytes beginning at
//! \a address.  Adjacent virtual segments are joined even when their file
//! ranges are discontiguous.  Unmapped holes terminate the run.  Returns 0 if
//! \a address is not mapped.
static mk_vm_size_t
__mk_memory_map_file_contiguous_run(mk_memory_map_file_t *file_map, mk_vm_address_t address)
{
    struct mk_memory_map_file_segment_s *segment;

    // A zero length request exactly at a segment boundary is considered mapped.
    segment = __mk_memory_map_file_segment_containing(file_map, address);
    if (segment == NULL) {
        segment = __mk_memory_map_file_segment_starting_at(file_map, address);
        if (segment == NULL)
            return 0;
    }

    mk_vm_size_t run = segment->end - address;
    mk_vm_address_t frontier = segment->end;

    // Extend across segments that are exactly adjacent in virtual memory.
    while ((segment = __mk_memory_map_file_segment_starting_at(file_map, frontier)) != NULL) {
        mk_vm_size_t addition = segment->end - segment->start;
        if (run > MK_VM_SIZE_MAX - addition) {
            run = MK_VM_SIZE_MAX;
            break;
        }
        run += addition;
        frontier = segment->end;
    }

    return run;
}

//|++++++++++++++++++++++++++++++++++++|//
//! Copies \a length bytes from the map's virtual address space, starting at
//! \a address, into \a destination.  The complete range must be part of one
//! contiguous run.  File backed bytes come from the snapshot; everything
//! else (the tail of a segment where \c filesize \< \c vmsize) is left as zero,
//! so \a destination must already be zero filled.
static void
__mk_memory_map_file_fill(mk_memory_map_file_t *file_map, mk_vm_address_t address, mk_vm_size_t length, uint8_t *destination)
{
    mk_vm_address_t current = address;
    mk_vm_size_t remaining = length;
    uint8_t *output = destination;

    while (remaining > 0) {
        struct mk_memory_map_file_segment_s *segment = __mk_memory_map_file_segment_containing(file_map, current);
        if (segment == NULL)
            segment = __mk_memory_map_file_segment_starting_at(file_map, current);
        // The caller verified the full range is contiguous and mapped, so a
        // missing segment here is unreachable.  Avoid trapping regardless.
        if (segment == NULL)
            break;

        mk_vm_size_t segment_offset = current - segment->start;
        mk_vm_size_t chunk = segment->end - current;
        if (chunk > remaining)
            chunk = remaining;

        // Copy the file backed portion of this chunk, if any.
        if (segment_offset < segment->filesize) {
            mk_vm_size_t file_chunk = segment->filesize - segment_offset;
            if (file_chunk > chunk)
                file_chunk = chunk;

            const uint8_t *source = (const uint8_t*)file_map->image_bytes + segment->fileoff + segment_offset;
            memcpy(output, source, (size_t)file_chunk);

            output += file_chunk;
            current += file_chunk;
            remaining -= file_chunk;
            chunk -= file_chunk;
        }

        // Any leftover \c chunk bytes are zero fill; the destination buffer
        // was allocated with calloc(), so nothing to write.
        if (chunk > 0) {
            output += chunk;
            current += chunk;
            remaining -= chunk;
        }
    }
}

//|++++++++++++++++++++++++++++++++++++|//
static int
__mk_memory_map_file_compare_segments(const void *a, const void *b)
{
    const struct mk_memory_map_file_segment_s *sa = a;
    const struct mk_memory_map_file_segment_s *sb = b;

    if (sa->start < sb->start) return -1;
    if (sa->start > sb->start) return 1;
    if (sa->end < sb->end) return -1;
    if (sa->end > sb->end) return 1;
    return 0;
}

//----------------------------------------------------------------------------//
#pragma mark -  Memory Map Methods
//----------------------------------------------------------------------------//

//|++++++++++++++++++++++++++++++++++++|//
static mk_error_t
__mk_memory_map_file_init_object(mk_memory_map_ref self, mk_vm_offset_t offset, mk_vm_address_t address, mk_vm_size_t length, bool require_full, mk_memory_object_t* memory_object)
{
    mk_memory_map_file_t *file_map = (mk_memory_map_file_t*)self.memory_map;
    mk_context_t *ctx = file_map->base.context;

    // Verify that adding the offset value will not overflow.  This matches
    // the arithmetic rules used by the other memory maps.
    if (MK_VM_ADDRESS_MAX - offset < address) {
        _mkl_debug(ctx, "Adding input offset [%" MK_VM_PRIuOFFSET "] to input address [0x%" MK_VM_PRIxADDR "] would overflow.", offset, address);
        return MK_EOVERFLOW;
    }

    mk_vm_address_t context_address = address + offset;

    // Clamp a length that would run past the top of the address space.
    mk_vm_size_t requested_length = length;
    if (requested_length > MK_VM_ADDRESS_MAX - context_address) {
        if (require_full) {
            _mkl_debug(ctx, "Input range (address = 0x%" MK_VM_PRIxADDR ", length = %" MK_VM_PRIuSIZE ") would overflow.", context_address, length);
            return MK_EOVERFLOW;
        }
        requested_length = MK_VM_ADDRESS_MAX - context_address;
    }

    // Determine the contiguous mapped prefix available at the address.
    mk_vm_size_t available_length = __mk_memory_map_file_contiguous_run(file_map, context_address);
    if (available_length == 0) {
        _mkl_debug(ctx, "Input range (address = 0x%" MK_VM_PRIxADDR ", length = %" MK_VM_PRIuSIZE ") is not mapped by the file image.", context_address, length);
        return MK_EBAD_ACCESS;
    }

    mk_vm_size_t object_length = MIN(requested_length, available_length);

    // A complete request must fit before the first hole.
    if (require_full && object_length < requested_length) {
        _mkl_debug(ctx, "Full mapping of (address = 0x%" MK_VM_PRIxADDR ", length = %" MK_VM_PRIuSIZE ") crosses an unmapped hole.", context_address, length);
        return MK_EBAD_ACCESS;
    }

    if (object_length > SIZE_MAX) {
        _mkl_debug(ctx, "Mapping length %" MK_VM_PRIuSIZE " is not representable in this process.", object_length);
        return MK_EOVERFLOW;
    }

    // Each memory object gets its own contiguous local, readable copy.  The
    // buffer is zero initialized to provide the zero-fill tail of segments.
    void *local_address = NULL;
    if (object_length > 0) {
        local_address = calloc(1, (size_t)object_length);
        if (local_address == NULL)
            return MK_EINTERNAL_ERROR;

        __mk_memory_map_file_fill(file_map, context_address, object_length, local_address);
    }

    // Initialize the memory object.  target_address and address are kept in a
    // strictly linear relationship so the stock remap/unmap implementations
    // round-trip exactly.
    memory_object->vtable = &_mk_memory_object_class;
    memory_object->mapping = &file_map->base;
    memory_object->target_address = context_address;
    memory_object->address = (vm_address_t)(uintptr_t)local_address;
    memory_object->length = (vm_size_t)object_length;
    memory_object->reserved1 = 0;
    memory_object->reserved2 = 0;

    return MK_ESUCCESS;
}

//|++++++++++++++++++++++++++++++++++++|//
static void
__mk_memory_map_file_free_object(mk_memory_map_ref self, mk_memory_object_t* memory_object)
{
#pragma unused (self)
    if (memory_object == NULL)
        return;

    // Every object owns its own contiguous local buffer.  The address field
    // is copied along with wrapper structures such as mk_segment_t, so keying
    // cleanup to the buffer itself stays valid across shallow copies.
    free((void*)(uintptr_t)memory_object->address);
    memory_object->address = 0;
    memory_object->length = 0;
}

//|++++++++++++++++++++++++++++++++++++|//
static bool
__mk_memory_map_file_has_mapping(mk_memory_map_ref self, mk_vm_offset_t offset, mk_vm_address_t address, mk_vm_size_t length, mk_error_t* error)
{
    mk_error_t err;
    mk_memory_object_t memory_object;

    if ((err = mk_memory_map_init_object(self, offset, address, length, false, &memory_object))) {
        MK_ERROR_OUT = err;
        return false;
    }

    // The range is fully accessible only when the (possibly short) object
    // covers every requested byte.
    bool retValue = mk_memory_object_length(&memory_object) >= length;

    mk_memory_map_free_object(self, &memory_object);
    return retValue;
}

const struct _mk_memory_map_vtable _mk_memory_map_file_class = {
    .base.super                 = &_mk_memory_map_class,
    .base.name                  = "memory_map_file",
    .init_object                = &__mk_memory_map_file_init_object,
    .free_object                = &__mk_memory_map_file_free_object,
    .has_mapping                = &__mk_memory_map_file_has_mapping
    // copy_bytes and the typed readers inherit the default implementations,
    // which are built on top of init_object.
};

intptr_t mk_memory_map_file_type = (intptr_t)&_mk_memory_map_file_class;

//----------------------------------------------------------------------------//
#pragma mark -  Creating A File Image Memory Map
//----------------------------------------------------------------------------//

//|++++++++++++++++++++++++++++++++++++|//
mk_error_t
mk_memory_map_file_init(mk_context_t *ctx, const void *bytes, size_t size, mk_vm_slide_t slide, mk_memory_map_file_t *file_map)
{
    if (file_map == NULL)
        return MK_EINVAL;

    memset(file_map, 0, sizeof(*file_map));

    if (bytes == NULL || size < sizeof(struct mach_header_64))
        return MK_EINVAL;

    // Negative slides are not supported, matching mk_macho_init_with_slide.
    if (slide < 0) {
        _mkl_debug(ctx, "Negative slide [%" MK_VM_PRIiSLIDE "] is not supported by the file memory map.", slide);
        return MK_EINVAL;
    }

    // Save a private snapshot of the input bytes.
    void *snapshot = malloc(size);
    if (snapshot == NULL)
        return MK_EINTERNAL_ERROR;
    memcpy(snapshot, bytes, size);

    file_map->base.vtable = &_mk_memory_map_file_class;
    file_map->base.context = ctx;
    file_map->image_bytes = snapshot;
    file_map->image_size = size;
    file_map->slide = slide;
    file_map->header_address = MK_VM_ADDRESS_INVALID;

    const uint8_t *image = (const uint8_t*)snapshot;

    // Only little-endian thin 64-bit images are in scope.  Reading the native
    // fields directly is valid on the little-endian macOS targets this code
    // is built and tested for.
    struct mach_header_64 header;
    memcpy(&header, image, sizeof(header));

    if (header.magic != MH_MAGIC_64) {
        _mkl_debug(ctx, "Unsupported Mach-O magic [0x%" PRIx32 "]; only little-endian thin 64-bit images are supported.", header.magic);
        mk_memory_map_file_free(file_map);
        return MK_EINVALID_DATA;
    }

    // All load commands must reside inside the file bytes.
    uint64_t commands_end = (uint64_t)sizeof(struct mach_header_64) + header.sizeofcmds;
    if (commands_end > (uint64_t)size) {
        _mkl_debug(ctx, "Load command region (size = %" PRIu32 ") extends past the file image (size = %zu).", header.sizeofcmds, size);
        mk_memory_map_file_free(file_map);
        return MK_EINVALID_DATA;
    }

    mk_error_t result = MK_ESUCCESS;
    size_t cursor = sizeof(struct mach_header_64);

    for (uint32_t command_index = 0; command_index < header.ncmds; ++command_index) {
        if (cursor + sizeof(struct load_command) > size || cursor + sizeof(struct load_command) > commands_end) {
            result = MK_EINVALID_DATA;
            goto failed;
        }

        struct load_command load_command;
        memcpy(&load_command, image + cursor, sizeof(load_command));

        if (load_command.cmdsize < sizeof(struct load_command) ||
            (uint64_t)load_command.cmdsize > (uint64_t)size - cursor ||
            cursor + load_command.cmdsize > commands_end)
        {
            _mkl_debug(ctx, "Load command at offset %zu has invalid size %" PRIu32 ".", cursor, load_command.cmdsize);
            result = MK_EINVALID_DATA;
            goto failed;
        }

        if (load_command.cmd == LC_SEGMENT_64) {
            if (load_command.cmdsize < sizeof(struct segment_command_64)) {
                result = MK_EINVALID_DATA;
                goto failed;
            }

            struct segment_command_64 segment;
            memcpy(&segment, image + cursor, sizeof(segment));

            struct mk_memory_map_file_segment_s descriptor;
            memset(&descriptor, 0, sizeof(descriptor));

            descriptor.vmaddr = segment.vmaddr;
            descriptor.vmsize = segment.vmsize;
            descriptor.fileoff = segment.fileoff;
            descriptor.filesize = segment.filesize;
            descriptor.pagezero = (strncmp(segment.segname, SEG_PAGEZERO, sizeof(segment.segname)) == 0);

            // The file backed range must fit inside the provided bytes.
            if (segment.fileoff > (uint64_t)size ||
                segment.filesize > (uint64_t)size - segment.fileoff)
            {
                _mkl_debug(ctx, "Segment %.16s file range (off = 0x%" PRIx64 ", size = 0x%" PRIx64 ") extends past the file image (size = %zu).",
                           segment.segname, segment.fileoff, segment.filesize, size);
                result = MK_EINVAL;
                goto failed;
            }

            // A segment cannot back more bytes than its virtual size.
            if (segment.filesize > segment.vmsize) {
                _mkl_debug(ctx, "Segment %.16s has filesize 0x%" PRIx64 " greater than vmsize 0x%" PRIx64 ".",
                           segment.segname, segment.filesize, segment.vmsize);
                result = MK_EINVAL;
                goto failed;
            }

            // Virtual range arithmetic must not overflow.
            if (segment.vmsize > MK_VM_ADDRESS_MAX - segment.vmaddr) {
                result = MK_EOVERFLOW;
                goto failed;
            }
            descriptor.vmend = segment.vmaddr + segment.vmsize;

            uint64_t unsigned_slide = (uint64_t)slide; // slide >= 0 checked above
            if (unsigned_slide > MK_VM_ADDRESS_MAX - segment.vmaddr ||
                segment.vmsize > MK_VM_ADDRESS_MAX - (segment.vmaddr + unsigned_slide))
            {
                result = MK_EOVERFLOW;
                goto failed;
            }
            descriptor.start = segment.vmaddr + unsigned_slide;
            descriptor.end = descriptor.start + segment.vmsize;

            struct mk_memory_map_file_segment_s *grown = realloc(file_map->segments, (file_map->segment_count + 1) * sizeof(*grown));
            if (grown == NULL) {
                result = MK_EINTERNAL_ERROR;
                goto failed;
            }
            file_map->segments = grown;
            file_map->segments[file_map->segment_count++] = descriptor;
        }

        cursor += load_command.cmdsize;
    }

    // Sort by virtual address so adjacency, holes and overlap checks are
    // independent of load command ordering.
    qsort(file_map->segments, file_map->segment_count, sizeof(*file_map->segments), &__mk_memory_map_file_compare_segments);

    // Non-empty virtual segments may not overlap.
    for (size_t i = 0; i < file_map->segment_count; ++i) {
        struct mk_memory_map_file_segment_s *a = &file_map->segments[i];
        if (a->vmsize == 0)
            continue;

        for (size_t j = i + 1; j < file_map->segment_count; ++j) {
            struct mk_memory_map_file_segment_s *b = &file_map->segments[j];
            if (b->vmsize == 0)
                continue;

            // Segments are sorted by start address.
            if (b->start < a->end) {
                _mkl_debug(ctx, "Virtual segments overlap: [0x%" MK_VM_PRIxADDR ", 0x%" MK_VM_PRIxADDR ") and [0x%" MK_VM_PRIxADDR ", 0x%" MK_VM_PRIxADDR ").",
                           a->start, a->end, b->start, b->end);
                result = MK_EINVAL;
                goto failed;
            }
            // Later segments only start further to the right.
            break;
        }
    }

    // The Mach-O header lives at file offset zero.  Its preferred virtual
    // address is the (vmaddr - fileoff) projection of file offset 0 into a
    // segment that actually backs that offset; __PAGEZERO has no file bytes
    // and must not contribute.  In well formed images every file backed
    // segment shares the same projection.
    bool found_header_base = false;
    mk_vm_address_t preferred_header_address = 0;
    for (size_t i = 0; i < file_map->segment_count; ++i) {
        struct mk_memory_map_file_segment_s *segment = &file_map->segments[i];
        if (segment->pagezero || segment->filesize == 0)
            continue;
        if (segment->fileoff != 0 || segment->vmaddr < segment->fileoff)
            continue;

        mk_vm_address_t projection = segment->vmaddr - (mk_vm_address_t)segment->fileoff;
        if (!found_header_base || projection < preferred_header_address) {
            found_header_base = true;
            preferred_header_address = projection;
        }
    }
    if (!found_header_base) {
        // Fall back to the lowest projection among file backed segments.
        for (size_t i = 0; i < file_map->segment_count; ++i) {
            struct mk_memory_map_file_segment_s *segment = &file_map->segments[i];
            if (segment->pagezero || segment->filesize == 0 || segment->vmaddr < segment->fileoff)
                continue;

            mk_vm_address_t projection = segment->vmaddr - (mk_vm_address_t)segment->fileoff;
            if (!found_header_base || projection < preferred_header_address) {
                found_header_base = true;
                preferred_header_address = projection;
            }
        }
    }
    if (!found_header_base)
        preferred_header_address = 0;

    if ((uint64_t)slide > MK_VM_ADDRESS_MAX - preferred_header_address) {
        result = MK_EOVERFLOW;
        goto failed;
    }
    file_map->header_address = preferred_header_address + (mk_vm_address_t)slide;

    return MK_ESUCCESS;

failed:
    mk_memory_map_file_free(file_map);
    return result;
}

//|++++++++++++++++++++++++++++++++++++|//
mk_vm_address_t
mk_memory_map_file_get_header_address(mk_memory_map_file_t *file_map)
{
    if (file_map == NULL)
        return MK_VM_ADDRESS_INVALID;
    return file_map->header_address;
}

//|++++++++++++++++++++++++++++++++++++|//
void
mk_memory_map_file_free(mk_memory_map_file_t *file_map)
{
    if (file_map == NULL)
        return;

    // Clients free memory objects before the map, as the module contract
    // requires; each object releases its own independent local buffer.
    free(file_map->segments);
    file_map->segments = NULL;

    free(file_map->image_bytes);
    file_map->image_bytes = NULL;
    file_map->image_size = 0;
    file_map->segment_count = 0;
    file_map->header_address = MK_VM_ADDRESS_INVALID;
    file_map->base.vtable = NULL;
}
