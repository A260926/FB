//----------------------------------------------------------------------------//
//|
//|             MachOKit - A Lightweight Mach-O Parsing Library
//! @file       memory_map_file.h
//!
//! @author     D.V.
//! @copyright  Copyright (c) 2014-2015 D.V. All rights reserved.
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

//----------------------------------------------------------------------------//
//! @defgroup MEMORY_MAP_FILE File Memory Map
//! @ingroup MEMORY_MAP
//!
//! A read-only memory map whose address space is derived from the segment
//! load commands of a thin, little-endian, 64-bit Mach-O file.  The map
//! keeps a private snapshot of the file's bytes, so the caller's input
//! buffer may be released as soon as initialization succeeds.
//!
//! Each segment contributes a virtual range beginning at its \c vmaddr
//! (adjusted by the slide supplied at initialization).  The first
//! \c filesize bytes of the range are backed by the file bytes at
//! \c fileoff; the remaining \c vmsize - \c filesize bytes read as zero.
//! Gaps between segments, and \c __PAGEZERO, are not mapped.
//!
//! Memory objects vended by this map own a contiguous, read-only local
//! copy of the requested range.  As with the other memory maps, free the
//! Mach-O image and any memory objects before freeing the map.
//----------------------------------------------------------------------------//

#ifndef _memory_map_file_h
#define _memory_map_file_h

#include "memory_map.h"

//! @addtogroup MEMORY_MAP_FILE
//! @{
//!

//----------------------------------------------------------------------------//
#pragma mark -  Types
//! @name       Types
//----------------------------------------------------------------------------//

//◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦//
//! @internal
//!
//! A single mapped segment of the file image.
//
typedef struct mk_memory_map_file_segment_s {
    //! The virtual address of the first byte of the segment after the slide
    //! has been applied.
    mk_vm_address_t target_address;
    //! The virtual size of the segment, including zero-filled bytes.
    mk_vm_size_t vmsize;
    //! The file offset of the first byte of the segment's file-backed data.
    uint64_t fileoff;
    //! The number of file-backed bytes at the start of the segment.
    uint64_t filesize;
} mk_memory_map_file_segment_t;

//◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦◦//
//! @internal
//
typedef struct mk_memory_map_file_s {
    struct mk_memory_map_s base;
    //! A private snapshot of the input file bytes.
    uint8_t *bytes;
    //! The number of bytes in \ref bytes.
    size_t size;
    //! The slide applied to every segment in the image.
    mk_vm_slide_t slide;
    //! The preferred load address of the file header, plus \ref slide.
    mk_vm_address_t header_address;
    //! The mapped segments, sorted by ascending target address.
    mk_memory_map_file_segment_t *segments;
    //! The number of entries in \ref segments.
    size_t segment_count;
} mk_memory_map_file_t;

//! The identifier for the File Memory Map type.
_mk_export intptr_t mk_memory_map_file_type;


//----------------------------------------------------------------------------//
#pragma mark -  Creating A File Memory Map
//! @name       Creating A File Memory Map
//----------------------------------------------------------------------------//

//! Initializes a read-only memory map from the bytes of a thin, little-endian
//! 64-bit Mach-O file.
//!
//! The map copies \a bytes immediately; the caller may free the input buffer
//! as soon as this function returns successfully.  The segment load commands
//! of the file are parsed directly to build the virtual address view.  A
//! negative \a slide, a fat or 32-bit image, a fileset, a segment whose
//! \c filesize exceeds its \c vmsize, a segment extending beyond \a size, or
//! overlapping non-empty segments are rejected.
//!
//! @param  ctx
//!         A client supplied context.  May be \c NULL.
//! @param  bytes
//!         The bytes of the Mach-O file.
//! @param  size
//!         The number of bytes in \a bytes.
//! @param  slide
//!         A non-negative slide applied to every segment's \c vmaddr.
//! @param  file_map
//!         A valid \ref mk_memory_map_file_t structure.
_mk_export mk_error_t
mk_memory_map_file_init(mk_context_t *ctx, const void *bytes, size_t size, mk_vm_slide_t slide, mk_memory_map_file_t *file_map);

//! Returns the preferred load address of the file header plus the slide
//! supplied to \ref mk_memory_map_file_init.  This value may be passed
//! directly to \ref mk_macho_init_with_slide along with the same slide and
//! the map.
_mk_export mk_vm_address_t
mk_memory_map_file_get_header_address(mk_memory_map_file_t *file_map);

//! Frees the memory map's private snapshot and segment table.  Any Mach-O
//! image or memory objects obtained from the map must be freed first.
_mk_export mk_error_t
mk_memory_map_file_free(mk_memory_map_file_t *file_map);


//! @} MEMORY_MAP_FILE !//

#endif /* _memory_map_file_h */
