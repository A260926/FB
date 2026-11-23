/*****************************************************************
|
|    AP4 - Edited Sample Reader
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

#ifndef _AP4_EDITED_SAMPLE_READER_H_
#define _AP4_EDITED_SAMPLE_READER_H_

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4Types.h"
#include "Ap4Results.h"
#include "Ap4Array.h"
#include "Ap4DataBuffer.h"

/*----------------------------------------------------------------------
|   class references
+---------------------------------------------------------------------*/
class AP4_Track;
class AP4_ByteStream;
class AP4_SampleSpool;

/*----------------------------------------------------------------------
|   AP4_EditedSampleReader
|
|   Delivers the compressed samples of a track in edited (movie
|   timeline) order, following the track edit list, from a source
|   stream that can only be consumed forward.
|
|   The source stream must be positioned at the first byte of the
|   (single, file-ending) mdat payload when Create() is called, must
|   support Tell() and GetSize(), and is never seeked: it is consumed
|   with ReadPartial() only. Sample bytes that are needed more than
|   once, or before their physical position is reached, are spooled
|   into the caller-supplied scratch stream (which must be empty,
|   readable, writable and seekable). Both streams and the track
|   remain owned by the caller and must outlive the reader.
|
|   The display windows carried by the packets are expressed in the
|   common timescale C, the least common multiple of the movie and
|   media timescales, exposed by GetTimeScale().
+---------------------------------------------------------------------*/
class AP4_EditedSampleReader
{
public:
    /**
     * One unit of edited output. MEDIA packets carry a compressed
     * sample payload; GAP packets mark an empty edit segment and
     * carry no payload.
     */
    class Packet
    {
    public:
        typedef enum {
            MEDIA = 0,
            GAP   = 1
        } Kind;

        Packet() :
            kind(MEDIA),
            edit_index(0),
            source_sample_index((AP4_Ordinal)-1),
            dts(0),
            cts(0),
            duration(0),
            description_index(0),
            sync(false),
            movie_begin(0),
            movie_duration(0) {}

        Kind           kind;
        AP4_UI32       edit_index;          // zero-based edit entry index
        AP4_Ordinal    source_sample_index; // zero-based, (AP4_Ordinal)-1 for GAP
        AP4_UI64       dts;                 // original media decode time
        AP4_SI64       cts;                 // original media composition time
        AP4_UI32       duration;            // original media duration
        AP4_Ordinal    description_index;
        bool           sync;
        AP4_UI64       movie_begin;         // display window, common timescale C
        AP4_UI64       movie_duration;      // 0/0 for decode-only preroll
        AP4_DataBuffer payload;
    };

    /**
     * Validate a track and plan an edited reading session.
     *
     * Only track metadata is read here; no media bytes are consumed
     * from the source and the scratch stream is left untouched. If
     * `capacity` is smaller than the planned spool peak,
     * AP4_ERROR_OUT_OF_RANGE is returned and `reader` is set to NULL,
     * with both streams left in their original state.
     */
    static AP4_Result Create(AP4_Track&               track,
                             AP4_ByteStream&          source,
                             AP4_ByteStream&          scratch,
                             AP4_UI64                 capacity,
                             AP4_EditedSampleReader*& reader);

    ~AP4_EditedSampleReader();

    /**
     * Read the next packet in edited order. Returns AP4_ERROR_EOS
     * when the edited timeline has been fully delivered. After an
     * I/O failure the session enters a terminal error state and every
     * subsequent call returns the same error.
     */
    AP4_Result ReadNext(Packet& packet);

    // accessors
    AP4_UI64 GetTimeScale()          const { return m_TimeScale;  } // common timescale C
    AP4_UI64 GetRequiredSpoolBytes() const { return m_Required;   } // planned spool peak

private:
    // types
    struct SampleInfo {
        SampleInfo() :
            offset(0), size(0), dts(0), cts(0), duration(0),
            description_index(0), sync(false), refs(0) {}
        AP4_UI64    offset; // relative to the source origin
        AP4_UI32    size;
        AP4_UI64    dts;
        AP4_SI64    cts;
        AP4_UI32    duration;
        AP4_Ordinal description_index;
        bool        sync;
        AP4_UI32    refs;   // remaining deliveries
    };
    struct PlanEntry {
        PlanEntry() :
            edit_index(0), sample(0), gap(false),
            movie_begin(0), movie_duration(0) {}
        AP4_UI32    edit_index;
        AP4_UI32    sample;
        bool        gap;
        AP4_UI64    movie_begin;
        AP4_UI64    movie_duration;
    };

    // methods
    AP4_EditedSampleReader(AP4_ByteStream& source,
                           AP4_SampleSpool* spool,
                           AP4_UI64         time_scale,
                           AP4_UI64         required);
    AP4_Result AdvanceFrontier(AP4_UI64 target);

    // members
    AP4_ByteStream&       m_Source;
    AP4_SampleSpool*      m_Spool;
    AP4_UI64              m_TimeScale;
    AP4_UI64              m_Required;
    AP4_Array<SampleInfo> m_Samples;
    AP4_Array<AP4_UI32>   m_SpoolOrder; // referenced samples, by physical offset
    AP4_Ordinal           m_NextSpool;
    AP4_Array<PlanEntry>  m_Plan;
    AP4_Ordinal           m_Cursor;
    AP4_UI64              m_Frontier;   // source bytes consumed so far
    AP4_Result            m_TerminalError;
};

#endif // _AP4_EDITED_SAMPLE_READER_H_
