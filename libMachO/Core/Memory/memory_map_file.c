//----------------------------------------------------------------------------//
//|
//|             MachOKit - A Lightweight Mach-O Parsing Library
//|             memory_map_file.c
//|
//|             D.V.
//|             Copyright (c) 2014-2015 D.V. All rights reserved.
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
//| OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//| MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//| IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//| CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//| TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//| SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//----------------------------------------------------------------------------//

#include "core_internal.h"
#include "memory_map_file.h"

#include <mach-o/loader.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MH_FILESET
#   define MH_FILESET 0xc
#endif

//----------------------------------------------------------------------------//
#pragma mark -  Byte Readers
//----------------------------------------------------------------------------//

// The file memory map only accepts little-endian images.  Read fields
// explicitly so that the code remains correct regardless of the host byte
// order and avoids requiring alignment of the input buffer.
//|++++++++++++++++++++++++++++++++++++|//
static uint32_t
_mk_file_read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0]
         | ((uint32_t)bytes[1] << 8)
         | ((uint32_t)bytes[2] << 16)
         | ((uint32_t)bytes[3] << 24);
}

//|++++++++++++++++++++++++++++++++++++|//
static uint64_t
_mk_file_read_le64(const uint8_t *bytes)
{
    return (uint64_t)bytes[0]
         | ((uint64_t)bytes[1] << 8)
         | ((uint64_t)bytes[2] << 16)
         | ((uint64_t)bytes[3] << 24)
         | ((uint64_t)bytes[4] << 32)
         | ((uint64_t)bytes[5] << 40)
         | ((uint64_t)bytes[6] << 48)
         | ((uint64_t)bytes[7] << 56);
}

//----------------------------------------------------------------------------//
#pragma mark -  Class Helpers
//----------------------------------------------------------------------------//

//|++++++++++++++++++++++++++++++++++++|//
//! Compares a zero-padded 16-byte Mach-O segment name against \a expected.
static bool
__mk_file_segment_name_equals(const uint8_t *name, const char *expected)
{
    size_t length = strlen(expected);
    if (length > 16)
        return false;

    if (memcmp(name, expected, length) != 0)
        return false;

    for (size_t i = length; i < 16; i++) {
        if (name[i] != 0)
            return false;
    }

    return true;
}

//|++++++++++++++++++++++++++++++++++++|//
static int
__mk_memory_map_file_segment_compare(const void *left, const void *right)
{
    const mk_memory_map_file_segment_t *l = left;
    const mk_memory_map_file_segment_t *r = right;

    if (l->target_address < r->target_address) return -1;
    if (l->target_address > r->target_address) return 1;
    return 0;
}

//|++++++++++++++++++++++++++++++++++++|//
//! Returns the index of the segment whose target range contains \a address,
//! or \c SIZE_MAX if no segment does.
static size_t
__mk_memory_map_file_segment_for_address(mk_memory_map_file_t *file_map, mk_vm_address_t address)
{
    size_t low = 0;
    size_t high = file_map->segment_count;

    while (low < high) {
        size_t mid = low + (high - low) / 2;
        mk_memory_map_file_segment_t *segment = &file_map->segments[mid];
        mk_vm_address_t segment_end = segment->target_address + segment->vmsize;

        if (address < segment->target_address)
            high = mid;
        else if (address >= segment_end)
            low = mid + 1;
        else
            return mid;
    }

    return SIZE_MAX;
}

//|++++++++++++++++++++++++++++++++++++|//
//! Computes the number of contiguous mapped bytes beginning at \a address,
//! clamped to \a length.  Adjacent segments are traversed; the first hole or
//! unmapped address ends the prefix.
static mk_vm_size_t
__mk_memory_map_file_contiguous_length(mk_memory_map_file_t *file_map, mk_vm_address_t address, mk_vm_size_t length)
{
    mk_vm_address_t cursor = address;
    mk_vm_address_t limit;

    // The caller has already checked that this can not overflow.
    limit = address + length;

    while (cursor < limit) {
        size_t index = __mk_memory_map_file_segment_for_address(file_map, cursor);
        if (index == SIZE_MAX)
            break;

        mk_vm_address_t segment_end = file_map->segments[index].target_address + file_map->segments[index].vmsize;
        if (segment_end >= limit) {
            cursor = limit;
            break;
        }

        // Move to the start of the next segment.  If the next segment does not
        // start exactly here, the next loop iteration will discover the hole.
        cursor = segment_end;
    }

    return cursor - address;
}

//|++++++++++++++++++++++++++++++++++++|//
//! Verifies that \a length bytes at \a address are fully accessible.
static bool
__mk_memory_map_file_has_mapping(mk_memory_map_ref self, mk_vm_offset_t offset, mk_vm_address_t address, mk_vm_size_t length, mk_error_t* error)
{
    mk_context_t *ctx = mk_type_get_context(self.memory_map);
    mk_memory_map_file_t *file_map = (mk_memory_map_file_t*)self.memory_map;

    // Verify that adding the offset value will not overflow.
    if (MK_VM_ADDRESS_MAX - offset < address) {
        _mkl_debug(ctx, "Adding input offset [%" MK_VM_PRIuOFFSET "] to input address [0x%" MK_VM_PRIxADDR "] would overflow.", offset, address);
        MK_ERROR_OUT = MK_EOVERFLOW;
        return false;
    }

    mk_vm_address_t context_address = address + offset;

    if (mk_vm_address_check_length(context_address, length) != MK_ESUCCESS) {
        _mkl_debug(ctx, "Input range (offset address = 0x%" MK_VM_PRIxADDR ", length = %" MK_VM_PRIuSIZE ") is not valid.", context_address, length);
        MK_ERROR_OUT = MK_EOVERFLOW;
        return false;
    }

    if (length == 0) {
        if (__mk_memory_map_file_segment_for_address(file_map, context_address) == SIZE_MAX) {
            MK_ERROR_OUT = MK_EBAD_ACCESS;
            return false;
        }
        MK_ERROR_OUT = MK_ESUCCESS;
        return true;
    }

    if (__mk_memory_map_file_contiguous_length(file_map, context_address, length) < length) {
        _mkl_debug(ctx, "Input range (offset address = 0x%" MK_VM_PRIxADDR ", length = %" MK_VM_PRIuSIZE ") is not fully mapped.", context_address, length);
        MK_ERROR_OUT = MK_EBAD_ACCESS;
        return false;
    }

    MK_ERROR_OUT = MK_ESUCCESS;
    return true;
}

//|++++++++++++++++++++++++++++++++++++|//
//! Allocates a contiguous local copy of the mapped bytes beginning at
//! \a context_address.  \a length is the verified contiguous length.
static mk_error_t
__mk_memory_map_file_copy_mapping(mk_memory_map_file_t *file_map, mk_vm_address_t context_address, mk_vm_size_t length, uint8_t **buffer)
{
    size_t allocation_length = (size_t)(length == 0 ? 1 : length);

    // Use an anonymous mapping so that the resulting view is page aligned and
    // can be made read-only after it has been populated.
    void *mapping = mmap(NULL, allocation_length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED)
        return MK_EINTERNAL_ERROR;

    uint8_t *output = mapping;
    mk_vm_address_t cursor = context_address;

    while (cursor < context_address + length) {
        size_t index = __mk_memory_map_file_segment_for_address(file_map, cursor);
        if (index == SIZE_MAX) {
            // Should be unreachable: the contiguous length was verified.
            munmap(mapping, allocation_length);
            return MK_EINTERNAL_ERROR;
        }

        mk_memory_map_file_segment_t *segment = &file_map->segments[index];
        mk_vm_size_t segment_offset = cursor - segment->target_address;
        mk_vm_size_t chunk = segment->vmsize - segment_offset;
        mk_vm_size_t remaining = (context_address + length) - cursor;
        if (chunk > remaining)
            chunk = remaining;

        // Copy the file-backed prefix of the segment.
        if (segment_offset < segment->filesize) {
            mk_vm_size_t file_bytes = segment->filesize - segment_offset;
            if (file_bytes > chunk)
                file_bytes = chunk;

            memcpy(output, file_map->bytes + segment->fileoff + segment_offset, (size_t)file_bytes);
            output += file_bytes;
            cursor += file_bytes;
            chunk -= file_bytes;
        }

        // The remainder of the segment is zero-filled.  The anonymous mapping
        // is demand-zero, so leave those pages untouched; only the pages that
        // are actually read are committed.
        output += chunk;
        cursor += chunk;
    }

    // The memory object vends a read-only view.
    if (mprotect(mapping, allocation_length, PROT_READ) != 0) {
        munmap(mapping, allocation_length);
        return MK_EINTERNAL_ERROR;
    }

    *buffer = mapping;
    return MK_ESUCCESS;
}

//|++++++++++++++++++++++++++++++++++++|//
static mk_error_t
__mk_memory_map_file_init_object(mk_memory_map_ref self, mk_vm_offset_t offset, mk_vm_address_t address, mk_vm_size_t length, bool require_full, mk_memory_object_t* memory_object)
{
    mk_context_t *ctx = mk_type_get_context(self.memory_map);
    mk_memory_map_file_t *file_map = (mk_memory_map_file_t*)self.memory_map;

    if (memory_object == NULL)
        return MK_EINVAL;

    // Verify that adding the offset value will not overflow.
    if (MK_VM_ADDRESS_MAX - offset < address) {
        _mkl_debug(ctx, "Adding input offset [%" MK_VM_PRIuOFFSET "] to input address [0x%" MK_VM_PRIxADDR "] would overflow.", offset, address);
        return MK_EOVERFLOW;
    }

    mk_vm_address_t context_address = address + offset;

    // Verify that adding the length will not overflow.
    if (mk_vm_address_check_length(context_address, length) != MK_ESUCCESS) {
        _mkl_debug(ctx, "Input range (offset address = 0x%" MK_VM_PRIxADDR ", length = %" MK_VM_PRIuSIZE ") is not valid.", context_address, length);
        return MK_EOVERFLOW;
    }

    // The starting address must be mapped.
    if (__mk_memory_map_file_segment_for_address(file_map, context_address) == SIZE_MAX) {
        _mkl_debug(ctx, "Input address [0x%" MK_VM_PRIxADDR "] is not within the file image.", context_address);
        return MK_EBAD_ACCESS;
    }

    // Determine how much of the requested range is contiguous in the virtual
    // address space.  A hole ends the available prefix.
    mk_vm_size_t available = __mk_memory_map_file_contiguous_length(file_map, context_address, length);
    if (require_full && available < length) {
        _mkl_debug(ctx, "Input range (offset address = 0x%" MK_VM_PRIxADDR ", length = %" MK_VM_PRIuSIZE ") is not fully mapped.", context_address, length);
        return MK_EBAD_ACCESS;
    }

    uint8_t *buffer = NULL;
    mk_error_t err = __mk_memory_map_file_copy_mapping(file_map, context_address, available, &buffer);
    if (err != MK_ESUCCESS) {
        _mkl_debug(ctx, "Failed to allocate a local view for input range (offset address = 0x%" MK_VM_PRIxADDR ", length = %" MK_VM_PRIuSIZE ").", context_address, length);
        return err;
    }

    memory_object->vtable = &_mk_memory_object_class;
    memory_object->mapping = self.memory_map;
    memory_object->target_address = context_address;
    memory_object->address = (vm_address_t)buffer;
    memory_object->length = (vm_size_t)available;
    memory_object->reserved1 = (uint64_t)(uintptr_t)buffer;
    memory_object->reserved2 = (uint64_t)(available == 0 ? 1 : available);

    return MK_ESUCCESS;
}

//|++++++++++++++++++++++++++++++++++++|//
static void
__mk_memory_map_file_free_object(mk_memory_map_ref self, mk_memory_object_t* memory_object)
{
#pragma unused (self)
    if (memory_object == NULL || memory_object->reserved1 == 0)
        return;

    munmap((void*)(uintptr_t)memory_object->reserved1, (size_t)memory_object->reserved2);
    memory_object->reserved1 = 0;
    memory_object->reserved2 = 0;
}

const struct _mk_memory_map_vtable _mk_memory_map_file_class = {
    .base.super                 = &_mk_memory_map_class,
    .base.name                  = "memory_map_file",
    .init_object                = &__mk_memory_map_file_init_object,
    .free_object                = &__mk_memory_map_file_free_object,
    .has_mapping                = &__mk_memory_map_file_has_mapping
};

intptr_t mk_memory_map_file_type = (intptr_t)&_mk_memory_map_file_class;

//----------------------------------------------------------------------------//
#pragma mark -  Creating A File Memory Map
//----------------------------------------------------------------------------//

//|++++++++++++++++++++++++++++++++++++|//
mk_error_t
mk_memory_map_file_init(mk_context_t *ctx, const void *bytes, size_t size, mk_vm_slide_t slide, mk_memory_map_file_t *file_map)
{
    if (file_map == NULL) return MK_EINVAL;
    if (bytes == NULL || size == 0) return MK_EINVAL;
    if (slide < 0) {
        _mkl_debug(ctx, "Negative slide [%" MK_VM_PRIiSLIDE "] is not supported.", slide);
        return MK_EINVAL;
    }

    const uint8_t *input = bytes;

    if (size < sizeof(struct mach_header_64)) {
        _mkl_debug(ctx, "Input is smaller than a 64-bit Mach-O header.");
        return MK_EINVALID_DATA;
    }

    uint32_t magic = _mk_file_read_le32(input + 0);
    uint32_t filetype = _mk_file_read_le32(input + 12);
    uint32_t ncmds = _mk_file_read_le32(input + 16);
    uint32_t sizeofcmds = _mk_file_read_le32(input + 20);

    // Only thin, little-endian, 64-bit images are supported.
    if (magic != MH_MAGIC_64) {
        _mkl_debug(ctx, "Unsupported Mach-O magic [0x%" PRIx32 "].  Only thin little-endian 64-bit images are supported.", magic);
        return MK_EINVALID_DATA;
    }

    // Filesets are not expanded.
    if (filetype == MH_FILESET) {
        _mkl_debug(ctx, "Unsupported Mach-O file type MH_FILESET.");
        return MK_EINVALID_DATA;
    }

    // Verify that the load commands fit within the input.
    size_t header_size = sizeof(struct mach_header_64);
    if (sizeofcmds > size - header_size) {
        _mkl_debug(ctx, "Mach-O load commands (size = %" PRIu32 ") extend beyond the input.", sizeofcmds);
        return MK_EINVALID_DATA;
    }

    // Bound the command count so a malformed header can not force a huge
    // allocation.  Every load command is at least sizeof(struct load_command).
    if (ncmds > sizeofcmds / sizeof(struct load_command)) {
        _mkl_debug(ctx, "Mach-O header 'ncmds' [%" PRIu32 "] exceeds what fits in 'sizeofcmds' [%" PRIu32 "].", ncmds, sizeofcmds);
        return MK_EINVALID_DATA;
    }

    size_t commands_end = header_size + sizeofcmds;

    // A Mach-O image must contain at least one segment command.
    mk_memory_map_file_segment_t *segments = NULL;
    size_t segment_count = 0;
    size_t segment_capacity = ncmds > 0 ? (size_t)ncmds : 1;

    segments = calloc(segment_capacity, sizeof(*segments));
    uint8_t *snapshot = malloc(size);
    if (segments == NULL || snapshot == NULL) {
        free(segments);
        free(snapshot);
        _mkl_error(ctx, "Failed to allocate file memory map storage.");
        return MK_EINTERNAL_ERROR;
    }

    memcpy(snapshot, input, size);

    mk_error_t err = MK_ESUCCESS;
    size_t command_offset = header_size;

    for (uint32_t i = 0; i < ncmds; i++) {
        if (command_offset + sizeof(struct load_command) > commands_end) {
            _mkl_debug(ctx, "Load command [%" PRIu32 "] extends beyond the load commands region.", i);
            err = MK_EINVALID_DATA;
            goto fail;
        }

        uint32_t cmd = _mk_file_read_le32(snapshot + command_offset + 0);
        uint32_t cmdsize = _mk_file_read_le32(snapshot + command_offset + 4);

        if (cmdsize < sizeof(struct load_command) || cmdsize > commands_end - command_offset) {
            _mkl_debug(ctx, "Load command [%" PRIu32 "] has invalid size [%" PRIu32 "].", i, cmdsize);
            err = MK_EINVALID_DATA;
            goto fail;
        }

        if (cmd == LC_SEGMENT_64) {
            if (cmdsize < sizeof(struct segment_command_64)) {
                _mkl_debug(ctx, "LC_SEGMENT_64 load command [%" PRIu32 "] is too small.", i);
                err = MK_EINVALID_DATA;
                goto fail;
            }

            const uint8_t *command = snapshot + command_offset;
            const uint8_t *name = command + 8;

            uint64_t vmaddr = _mk_file_read_le64(command + 24);
            uint64_t vmsize = _mk_file_read_le64(command + 32);
            uint64_t fileoff = _mk_file_read_le64(command + 40);
            uint64_t filesize = _mk_file_read_le64(command + 48);

            // A segment can not contain more file bytes than virtual bytes.
            if (filesize > vmsize) {
                _mkl_debug(ctx, "Segment [%.16s] has a filesize [0x%" PRIx64 "] larger than its vmsize [0x%" PRIx64 "].", name, filesize, vmsize);
                err = MK_EINVALID_DATA;
                goto fail;
            }

            // The file-backed portion must lie within the input.
            if (filesize > 0 && (fileoff > size || filesize > size - fileoff)) {
                _mkl_debug(ctx, "Segment [%.16s] file range (offset = 0x%" PRIx64 ", size = 0x%" PRIx64 ") extends beyond the input.", name, fileoff, filesize);
                err = MK_EINVALID_DATA;
                goto fail;
            }

            // __PAGEZERO is never mapped.
            if (__mk_file_segment_name_equals(name, SEG_PAGEZERO))
                goto next_command;

            // Empty segments contribute no virtual mapping.
            if (vmsize == 0)
                goto next_command;

            mk_vm_address_t target_address;
            if ((err = mk_vm_address_apply_slide(vmaddr, slide, &target_address))) {
                _mkl_debug(ctx, "Arithmetic error [%s] applying slide [%" MK_VM_PRIiSLIDE "] to segment [%.16s] VM address [0x%" PRIx64 "].", mk_error_string(err), slide, name, vmaddr);
                goto fail;
            }

            if (mk_vm_address_check_length(target_address, vmsize) != MK_ESUCCESS) {
                _mkl_debug(ctx, "Segment [%.16s] virtual range (address = 0x%" MK_VM_PRIxADDR ", size = 0x%" PRIx64 ") overflows.", name, target_address, vmsize);
                err = MK_EOVERFLOW;
                goto fail;
            }

            segments[segment_count].target_address = target_address;
            segments[segment_count].vmsize = vmsize;
            segments[segment_count].fileoff = fileoff;
            segments[segment_count].filesize = filesize;
            segment_count++;
        }

    next_command:
        command_offset += cmdsize;
    }

    if (segment_count == 0) {
        _mkl_debug(ctx, "Mach-O image does not contain any mapped segments.");
        err = MK_EINVALID_DATA;
        goto fail;
    }

    qsort(segments, segment_count, sizeof(*segments), __mk_memory_map_file_segment_compare);

    // Non-empty virtual segments may not overlap.
    for (size_t i = 1; i < segment_count; i++) {
        mk_memory_map_file_segment_t *previous = &segments[i - 1];
        mk_memory_map_file_segment_t *current = &segments[i];
        mk_vm_address_t previous_end = previous->target_address + previous->vmsize;

        if (current->target_address < previous_end) {
            _mkl_debug(ctx, "Virtual segments at addresses [0x%" MK_VM_PRIxADDR "] and [0x%" MK_VM_PRIxADDR "] overlap.", previous->target_address, current->target_address);
            err = MK_EINVALID_DATA;
            goto fail;
        }
    }

    // The file header lives at file offset zero.  Determine the preferred
    // address that corresponds to it.
    mk_vm_address_t header_address = MK_VM_ADDRESS_INVALID;
    for (size_t i = 0; i < segment_count; i++) {
        mk_memory_map_file_segment_t *segment = &segments[i];
        if (segment->fileoff == 0 && segment->filesize >= sizeof(struct mach_header_64)) {
            header_address = segment->target_address;
            break;
        }
    }

    if (header_address == MK_VM_ADDRESS_INVALID) {
        _mkl_debug(ctx, "No segment maps the Mach-O header at file offset zero.");
        err = MK_EINVALID_DATA;
        goto fail;
    }

    file_map->base.vtable = &_mk_memory_map_file_class;
    file_map->base.context = ctx;
    file_map->bytes = snapshot;
    file_map->size = size;
    file_map->slide = slide;
    file_map->header_address = header_address;
    file_map->segments = segments;
    file_map->segment_count = segment_count;

    return MK_ESUCCESS;

fail:
    free(snapshot);
    free(segments);
    return err;
}

//|++++++++++++++++++++++++++++++++++++|//
mk_vm_address_t
mk_memory_map_file_get_header_address(mk_memory_map_file_t *file_map)
{
    if (file_map == NULL) return MK_VM_ADDRESS_INVALID;
    return file_map->header_address;
}

//|++++++++++++++++++++++++++++++++++++|//
mk_error_t
mk_memory_map_file_free(mk_memory_map_file_t *file_map)
{
    if (file_map == NULL) return MK_EINVAL;

    free(file_map->segments);
    free(file_map->bytes);

    file_map->segments = NULL;
    file_map->segment_count = 0;
    file_map->bytes = NULL;
    file_map->size = 0;
    file_map->header_address = MK_VM_ADDRESS_INVALID;
    file_map->base.vtable = NULL;
    file_map->base.context = NULL;

    return MK_ESUCCESS;
}
