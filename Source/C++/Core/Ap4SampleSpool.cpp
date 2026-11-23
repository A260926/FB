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

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include <stdlib.h>

#include "Ap4SampleSpool.h"
#include "Ap4ByteStream.h"

/*----------------------------------------------------------------------
|   AP4_SampleSpool::Create
+---------------------------------------------------------------------*/
AP4_Result
AP4_SampleSpool::Create(AP4_ByteStream&   scratch,
                        AP4_UI64          capacity,
                        AP4_SampleSpool*& spool)
{
    spool = NULL;

    AP4_SampleSpool* instance = new AP4_SampleSpool(scratch, capacity);
    if (instance->m_MoveBuffer == NULL) {
        delete instance;
        return AP4_ERROR_OUT_OF_MEMORY;
    }

    spool = instance;
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::AP4_SampleSpool
+---------------------------------------------------------------------*/
AP4_SampleSpool::AP4_SampleSpool(AP4_ByteStream& scratch, AP4_UI64 capacity) :
    m_Scratch(scratch),
    m_Capacity(capacity),
    m_LiveBytes(0),
    m_Append(0),
    m_MoveBuffer(new AP4_UI08[AP4_SAMPLE_SPOOL_MOVE_BUFFER_SIZE])
{
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::~AP4_SampleSpool
+---------------------------------------------------------------------*/
AP4_SampleSpool::~AP4_SampleSpool()
{
    delete[] m_MoveBuffer;
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::EnsureSlot
+---------------------------------------------------------------------*/
AP4_Result
AP4_SampleSpool::EnsureSlot(AP4_UI32 slot)
{
    if (slot < m_Slots.ItemCount()) return AP4_SUCCESS;
    return m_Slots.SetItemCount(slot+1);
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::IsOpen
+---------------------------------------------------------------------*/
bool
AP4_SampleSpool::IsOpen(AP4_UI32 slot) const
{
    return slot < m_Slots.ItemCount() && m_Slots[slot].m_Open;
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::ReadFull
+---------------------------------------------------------------------*/
AP4_Result
AP4_SampleSpool::ReadFull(AP4_ByteStream& stream, void* buffer, AP4_Size size)
{
    AP4_Byte* cursor = (AP4_Byte*)buffer;
    while (size) {
        AP4_Size  bytes_read = 0;
        AP4_Result result    = stream.ReadPartial(cursor, size, bytes_read);
        if (AP4_FAILED(result)) {
            // a short read is a truncation, not a clean end of stream
            return result == AP4_ERROR_EOS ? AP4_ERROR_READ_FAILED : result;
        }
        if (bytes_read == 0) return AP4_ERROR_READ_FAILED;
        cursor += bytes_read;
        size   -= bytes_read;
    }
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::WriteFull
+---------------------------------------------------------------------*/
AP4_Result
AP4_SampleSpool::WriteFull(AP4_ByteStream& stream, const void* buffer, AP4_Size size)
{
    const AP4_Byte* cursor = (const AP4_Byte*)buffer;
    while (size) {
        AP4_Size  bytes_written = 0;
        AP4_Result result       = stream.WritePartial(cursor, size, bytes_written);
        if (AP4_FAILED(result)) return result;
        if (bytes_written == 0) return AP4_ERROR_WRITE_FAILED;
        cursor += bytes_written;
        size   -= bytes_written;
    }
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::Open
+---------------------------------------------------------------------*/
AP4_Result
AP4_SampleSpool::Open(AP4_UI32 slot, AP4_UI64 size)
{
    AP4_Result result = EnsureSlot(slot);
    if (AP4_FAILED(result)) return result;

    Slot& entry = m_Slots[slot];
    if (entry.m_Open) return AP4_ERROR_INVALID_STATE;

    // the caller guarantees that the live bytes never exceed the
    // capacity; check defensively anyway
    if (size > m_Capacity || m_LiveBytes > m_Capacity-size) {
        return AP4_ERROR_OUT_OF_RANGE;
    }

    // compact if the append frontier cannot hold the new slot
    if (size > m_Capacity-m_Append) {
        result = Compact();
        if (AP4_FAILED(result)) return result;
    }

    entry.m_Position = m_Append;
    entry.m_Size     = size;
    entry.m_Open     = true;
    m_Append    += size;
    m_LiveBytes += size;

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::Compact
+---------------------------------------------------------------------*/
struct AP4_SampleSpoolSlotOrder {
    AP4_UI64 m_Position;
    AP4_UI32 m_Slot;
};

static int
AP4_SampleSpoolCompareSlots(const void* a, const void* b)
{
    const AP4_SampleSpoolSlotOrder* sa = (const AP4_SampleSpoolSlotOrder*)a;
    const AP4_SampleSpoolSlotOrder* sb = (const AP4_SampleSpoolSlotOrder*)b;
    if (sa->m_Position < sb->m_Position) return -1;
    if (sa->m_Position > sb->m_Position) return 1;
    return 0;
}

AP4_Result
AP4_SampleSpool::Compact()
{
    // collect the open slots
    AP4_Cardinal open_count = 0;
    AP4_Ordinal  i;
    for (i=0; i<m_Slots.ItemCount(); i++) {
        if (m_Slots[i].m_Open) ++open_count;
    }
    if (open_count == 0) {
        m_Append = 0;
        return AP4_SUCCESS;
    }

    AP4_SampleSpoolSlotOrder* order = new AP4_SampleSpoolSlotOrder[open_count];
    if (order == NULL) return AP4_ERROR_OUT_OF_MEMORY;
    open_count = 0;
    for (i=0; i<m_Slots.ItemCount(); i++) {
        if (m_Slots[i].m_Open) {
            order[open_count].m_Position = m_Slots[i].m_Position;
            order[open_count].m_Slot     = i;
            ++open_count;
        }
    }
    qsort(order, open_count, sizeof(order[0]), AP4_SampleSpoolCompareSlots);

    // move the live slots towards the origin, in ascending address
    // order, so that a move never overwrites bytes that have not been
    // relocated yet
    AP4_UI64   target = 0;
    AP4_Result result = AP4_SUCCESS;
    for (i=0; i<open_count; i++) {
        Slot& entry = m_Slots[order[i].m_Slot];
        if (entry.m_Position != target) {
            AP4_UI64 new_position = target;
            AP4_UI64 source       = entry.m_Position;
            AP4_UI64 remaining    = entry.m_Size;
            while (remaining) {
                AP4_Size chunk = remaining > AP4_SAMPLE_SPOOL_MOVE_BUFFER_SIZE
                               ? AP4_SAMPLE_SPOOL_MOVE_BUFFER_SIZE
                               : (AP4_Size)remaining;
                result = m_Scratch.Seek(source);
                if (AP4_FAILED(result)) break;
                result = ReadFull(m_Scratch, m_MoveBuffer, chunk);
                if (AP4_FAILED(result)) break;
                result = m_Scratch.Seek(target);
                if (AP4_FAILED(result)) break;
                result = WriteFull(m_Scratch, m_MoveBuffer, chunk);
                if (AP4_FAILED(result)) break;
                source    += chunk;
                target    += chunk;
                remaining -= chunk;
            }
            if (AP4_FAILED(result)) break;
            entry.m_Position = new_position;
        } else {
            target += entry.m_Size;
        }
    }

    delete[] order;
    if (AP4_FAILED(result)) return result;

    m_Append = target;
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::Store
+---------------------------------------------------------------------*/
AP4_Result
AP4_SampleSpool::Store(AP4_UI32        slot,
                       AP4_UI64        offset,
                       AP4_ByteStream& source,
                       AP4_UI64        size)
{
    if (!IsOpen(slot)) return AP4_ERROR_INVALID_STATE;
    Slot& entry = m_Slots[slot];
    if (offset > entry.m_Size || size > entry.m_Size-offset) {
        return AP4_ERROR_OUT_OF_RANGE;
    }

    AP4_UI64 position = entry.m_Position+offset;
    while (size) {
        AP4_Size chunk = size > AP4_SAMPLE_SPOOL_MOVE_BUFFER_SIZE
                       ? AP4_SAMPLE_SPOOL_MOVE_BUFFER_SIZE
                       : (AP4_Size)size;
        AP4_Result result = ReadFull(source, m_MoveBuffer, chunk);
        if (AP4_FAILED(result)) return result;
        result = m_Scratch.Seek(position);
        if (AP4_FAILED(result)) return result;
        result = WriteFull(m_Scratch, m_MoveBuffer, chunk);
        if (AP4_FAILED(result)) return result;
        position += chunk;
        size     -= chunk;
    }

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::Load
+---------------------------------------------------------------------*/
AP4_Result
AP4_SampleSpool::Load(AP4_UI32 slot,
                      AP4_UI64 offset,
                      void*    buffer,
                      AP4_Size size)
{
    if (!IsOpen(slot)) return AP4_ERROR_INVALID_STATE;
    Slot& entry = m_Slots[slot];
    if (offset > entry.m_Size || size > entry.m_Size-offset) {
        return AP4_ERROR_OUT_OF_RANGE;
    }
    if (size == 0) return AP4_SUCCESS;

    AP4_Result result = m_Scratch.Seek(entry.m_Position+offset);
    if (AP4_FAILED(result)) return result;
    return ReadFull(m_Scratch, buffer, size);
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::Close
+---------------------------------------------------------------------*/
AP4_Result
AP4_SampleSpool::Close(AP4_UI32 slot)
{
    if (!IsOpen(slot)) return AP4_ERROR_INVALID_STATE;
    Slot& entry = m_Slots[slot];
    entry.m_Open = false;
    m_LiveBytes -= entry.m_Size;
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_SampleSpool::Discard
+---------------------------------------------------------------------*/
AP4_Result
AP4_SampleSpool::Discard(AP4_ByteStream& source, AP4_UI64 byte_count)
{
    while (byte_count) {
        AP4_Size chunk = byte_count > AP4_SAMPLE_SPOOL_MOVE_BUFFER_SIZE
                       ? AP4_SAMPLE_SPOOL_MOVE_BUFFER_SIZE
                       : (AP4_Size)byte_count;
        AP4_Result result = ReadFull(source, m_MoveBuffer, chunk);
        if (AP4_FAILED(result)) return result;
        byte_count -= chunk;
    }
    return AP4_SUCCESS;
}
