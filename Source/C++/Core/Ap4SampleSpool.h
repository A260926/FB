/*****************************************************************
|
|    AP4 - Sample Spool
|
|    Copyright 2002-2026 Axiomatic Systems, LLC
|
|
|    This file is part of Bento4/AP4 (MP4 Atom Processing Library).
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2, or (at your option)
|    any later version.
|
|    Bento4|GPL is distributed in the hope that it will be useful,
|    but WITHOUT ANY WARRANTY; without even the implied warranty of
|    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
|    GNU General Public License for more details.
|
|    You should have received a copy of the GNU General Public License
|    along with Bento4|GPL; see the file COPYING.  If not, write to the
|    Free Software Foundation, 59 Temple Place - Suite 330, Boston, MA
|    02111-1307, USA.
|
 ****************************************************************/

#ifndef _AP4_SAMPLE_SPOOL_H_
#define _AP4_SAMPLE_SPOOL_H_

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4Types.h"
#include "Ap4Results.h"
#include "Ap4Array.h"

/*----------------------------------------------------------------------
|   class references
+---------------------------------------------------------------------*/
class AP4_ByteStream;

/*----------------------------------------------------------------------
|   constants
+---------------------------------------------------------------------*/
const AP4_Size AP4_SAMPLE_SPOOL_MOVE_BUFFER_SIZE = 64*1024;

/*----------------------------------------------------------------------
|   AP4_SampleSpool
|
|   Address-space manager for a caller-owned scratch stream. Sample
|   payloads are stored in numbered slots inside the [0, capacity)
|   region of the stream. The spool only keeps slot bookkeeping in
|   memory plus one bounded move buffer; all payload bytes live in the
|   scratch stream. Allocation compacts the live slots towards the
|   origin when the append frontier would overflow the capacity, so
|   fragmentation never causes a spurious failure while the total
|   number of live bytes fits in the capacity.
+---------------------------------------------------------------------*/
class AP4_SampleSpool
{
public:
    /**
     * Create a spool managing the [0, capacity) region of a scratch
     * stream. The scratch stream is not owned by the spool and must
     * remain valid (readable, writable and seekable) for the lifetime
     * of the spool. No I/O is performed on the scratch stream by this
     * method.
     */
    static AP4_Result Create(AP4_ByteStream&   scratch,
                             AP4_UI64          capacity,
                             AP4_SampleSpool*& spool);

    ~AP4_SampleSpool();

    // accessors
    AP4_UI64 GetCapacity()  const { return m_Capacity;  }
    AP4_UI64 GetLiveBytes() const { return m_LiveBytes; }

    /**
     * Test whether a slot currently holds bytes.
     */
    bool IsOpen(AP4_UI32 slot) const;

    /**
     * Reserve a slot of `size` bytes. Live slots may be relocated
     * (compaction) to make room; their contents are preserved.
     * Returns AP4_ERROR_OUT_OF_RANGE if the total live bytes would
     * exceed the capacity, AP4_ERROR_INVALID_STATE if the slot is
     * already open.
     */
    AP4_Result Open(AP4_UI32 slot, AP4_UI64 size);

    /**
     * Copy `size` bytes from the current position of a source stream
     * into an open slot at `offset` within the slot. The source is
     * consumed strictly forward with a bounded buffer.
     */
    AP4_Result Store(AP4_UI32         slot,
                     AP4_UI64         offset,
                     AP4_ByteStream&  source,
                     AP4_UI64         size);

    /**
     * Read `size` bytes from an open slot at `offset` within the slot
     * into a memory buffer.
     */
    AP4_Result Load(AP4_UI32  slot,
                    AP4_UI64  offset,
                    void*     buffer,
                    AP4_Size  size);

    /**
     * Release a slot. The space becomes reusable after the next
     * compaction.
     */
    AP4_Result Close(AP4_UI32 slot);

    /**
     * Consume and discard `byte_count` bytes from a source stream,
     * reading strictly forward through the bounded move buffer.
     */
    AP4_Result Discard(AP4_ByteStream& source, AP4_UI64 byte_count);

private:
    // methods
    AP4_SampleSpool(AP4_ByteStream& scratch, AP4_UI64 capacity);
    AP4_Result EnsureSlot(AP4_UI32 slot);
    AP4_Result Compact();
    AP4_Result ReadFull(AP4_ByteStream& stream, void* buffer, AP4_Size size);
    AP4_Result WriteFull(AP4_ByteStream& stream, const void* buffer, AP4_Size size);

    // types
    struct Slot {
        Slot() : m_Position(0), m_Size(0), m_Open(false) {}
        AP4_UI64 m_Position;
        AP4_UI64 m_Size;
        bool     m_Open;
    };

    // members
    AP4_ByteStream& m_Scratch;
    AP4_UI64        m_Capacity;
    AP4_UI64        m_LiveBytes;
    AP4_UI64        m_Append;
    AP4_Array<Slot> m_Slots;
    AP4_UI08*       m_MoveBuffer;
};

#endif // _AP4_SAMPLE_SPOOL_H_
