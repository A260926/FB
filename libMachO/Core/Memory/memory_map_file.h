//----------------------------------------------------------------------------//
//|
//|             MachOKit - A Lightweight Mach-O Parsing Library
//! @file       memory_map_file.h
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

//----------------------------------------------------------------------------//
//! @defgroup MEMORY_MAP_FILE File Image Memory Map
//! @ingroup MEMORY_MAP
//!
//! A read-only memory map backed by the bytes of an on-disk Mach-O file.
//! Segment load commands are used to present the virtual memory layout of the
//! image directly from the file bytes: the first \c filesize bytes of each
//! segment are backed by the file, the remainder of \c vmsize is zero filled,
//! and unmapped gaps between segments are not accessible.
//!
//! Only little-endian, thin, 64-bit (\c MH_MAGIC_64) images are supported.
//! Fat images, filesets and executable execution are explicitly out of scope.
//----------------------------------------------------------------------------//

#ifndef _memory_map_file_h
#define _memory_map_file_h

#include "core.h"

//! @addtogroup MEMORY_MAP_FILE
//! @{
//!

//----------------------------------------------------------------------------//
#pragma mark -  Types
//! @name       Types
//----------------------------------------------------------------------------//

//! @internal
struct mk_memory_map_file_segment_s;

//◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦//
//! A memory map backed by a snapshot of a Mach-O file image.
//!
//! Initialize with \ref mk_memory_map_file_init and clean up with
//! \ref mk_memory_map_file_free.  The structure itself is client allocated.
//
typedef struct mk_memory_map_file_s {
    struct mk_memory_map_s base;
    //! Owned copy of the input file bytes.
    void *image_bytes;
    //! Size of \ref image_bytes.
    size_t image_size;
    //! Non-negative slide applied to preferred virtual addresses.
    mk_vm_slide_t slide;
    //! Slid virtual address of the Mach-O header (file offset zero).
    mk_vm_address_t header_address;
    //! Segment descriptors parsed from the load commands, sorted by address.
    struct mk_memory_map_file_segment_s *segments;
    //! Number of entries in \ref segments.
    size_t segment_count;
} mk_memory_map_file_t;

//! The identifier for the Memory Map File type.
_mk_export intptr_t mk_memory_map_file_type;


//----------------------------------------------------------------------------//
#pragma mark -  Creating A File Image Memory Map
//! @name       Creating A File Image Memory Map
//----------------------------------------------------------------------------//

//! Parses the \c LC_SEGMENT_64 load commands in \a bytes and initializes a
//! file image memory map.
//!
//! The map saves its own snapshot of the input bytes; the caller may release
//! \a bytes as soon as this function returns successfully.
//!
//! @param  ctx
//!         An optional library context.  May be \c NULL.
//! @param  bytes
//!         The raw bytes of a little-endian thin 64-bit Mach-O image.
//! @param  size
//!         The number of bytes at \a bytes.
//! @param  slide
//!         A non-negative slide applied to the preferred virtual addresses.
//! @param  file_map
//!         The map structure to initialize.
_mk_export mk_error_t
mk_memory_map_file_init(mk_context_t *ctx, const void *bytes, size_t size, mk_vm_slide_t slide, mk_memory_map_file_t *file_map);

//! Returns the slid preferred virtual address of the Mach-O header.  The
//! returned value can be passed as the \c address argument to
//! \ref mk_macho_init_with_slide using the same slide.
_mk_export mk_vm_address_t
mk_memory_map_file_get_header_address(mk_memory_map_file_t *file_map);

//! Releases all resources owned by \a file_map, including the byte snapshot.
//! Memory objects vended by the map must be freed before calling this
//! function.
_mk_export void
mk_memory_map_file_free(mk_memory_map_file_t *file_map);


//! @} MEMORY_MAP_FILE !//

#endif /* _memory_map_file_h */
