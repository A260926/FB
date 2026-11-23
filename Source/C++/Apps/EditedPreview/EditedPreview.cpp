/*****************************************************************
|
|    AP4 - Edited Preview Exporter
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Ap4.h"
#include "Ap4EditedSampleReader.h"
#include "Ap4SampleSpool.h"

/*----------------------------------------------------------------------
|   constants
+---------------------------------------------------------------------*/
#define BANNER "Edited Preview Exporter - Version 1.0\n"\
               "(Bento4 Version " AP4_VERSION_STRING ")\n"\
               "(c) 2002-2026 Axiomatic Systems, LLC"

const AP4_UI32 AP4_EDITED_PREVIEW_FORMAT_VERSION = 1;

/*----------------------------------------------------------------------
|   PrintUsageAndExit
+---------------------------------------------------------------------*/
static void
PrintUsageAndExit()
{
    fprintf(stderr,
            BANNER
            "\n\nusage: editedpreview <input.mp4> <scratch> <output> <capacity>\n"
            "    <input.mp4> : fast-start MP4 with a single video track\n"
            "    <scratch>   : path of a scratch file usable as spool space\n"
            "    <output>    : path of the exported packet byte stream\n"
            "    <capacity>  : decimal spool capacity in bytes\n");
    exit(1);
}

/*----------------------------------------------------------------------
|   ForwardOnlyStream
|
|   Read-only adapter that hides the Seek() method of its delegate:
|   it models a source (such as an already-open pipe) that can only
|   be consumed forward from its current position.
+---------------------------------------------------------------------*/
class ForwardOnlyStream : public AP4_ByteStream
{
public:
    ForwardOnlyStream(AP4_ByteStream& delegate) : m_Delegate(delegate) {}

    // AP4_ByteStream methods
    AP4_Result ReadPartial(void*     buffer,
                           AP4_Size  bytes_to_read,
                           AP4_Size& bytes_read) {
        return m_Delegate.ReadPartial(buffer, bytes_to_read, bytes_read);
    }
    AP4_Result WritePartial(const void*, AP4_Size, AP4_Size& bytes_written) {
        bytes_written = 0;
        return AP4_ERROR_NOT_SUPPORTED;
    }
    AP4_Result Seek(AP4_Position) {
        return AP4_ERROR_NOT_SUPPORTED;
    }
    AP4_Result Tell(AP4_Position& position) {
        return m_Delegate.Tell(position);
    }
    AP4_Result GetSize(AP4_LargeSize& size) {
        return m_Delegate.GetSize(size);
    }

    // AP4_Referenceable methods
    void AddReference() {}
    void Release()      {}

private:
    AP4_ByteStream& m_Delegate;
};

/*----------------------------------------------------------------------
|   FindMdatPayloadOffset
+---------------------------------------------------------------------*/
static AP4_Result
FindMdatPayloadOffset(AP4_File& file, AP4_UI64& offset)
{
    offset = 0;
    AP4_List<AP4_Atom>& atoms = file.GetTopLevelAtoms();
    for (AP4_List<AP4_Atom>::Item* item = atoms.FirstItem();
         item;
         item = item->GetNext()) {
        AP4_Atom* atom = item->GetData();
        if (atom->GetType() == AP4_ATOM_TYPE_MDAT) {
            offset += atom->GetHeaderSize();
            return AP4_SUCCESS;
        }
        offset += atom->GetSize();
    }
    return AP4_ERROR_NO_SUCH_ITEM;
}

/*----------------------------------------------------------------------
|   main
+---------------------------------------------------------------------*/
int
main(int argc, char** argv)
{
    if (argc != 5) {
        PrintUsageAndExit();
    }
    const char* input_filename   = argv[1];
    const char* scratch_filename = argv[2];
    const char* output_filename  = argv[3];

    // parse the capacity as a plain decimal number
    const char* capacity_arg = argv[4];
    if (capacity_arg[0] == '\0') PrintUsageAndExit();
    AP4_UI64 capacity = 0;
    {
        unsigned long long parsed = 0;
        for (const char* c = capacity_arg; *c; ++c) {
            if (*c < '0' || *c > '9') PrintUsageAndExit();
            unsigned int digit = (unsigned int)(*c-'0');
            if (parsed > (~0ULL-digit)/10ULL) PrintUsageAndExit();
            parsed = parsed*10ULL+digit;
        }
        capacity = (AP4_UI64)parsed;
    }

    // open and parse the input file
    AP4_ByteStream* input = NULL;
    AP4_Result result = AP4_FileByteStream::Create(input_filename,
                                                   AP4_FileByteStream::STREAM_MODE_READ,
                                                   input);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot open input file (%d)\n", result);
        return 1;
    }
    AP4_File file(*input);
    AP4_Movie* movie = file.GetMovie();
    if (movie == NULL) {
        fprintf(stderr, "ERROR: no movie found in input file\n");
        return 1;
    }
    AP4_Track* track = movie->GetTrack(AP4_Track::TYPE_VIDEO);
    if (track == NULL) {
        fprintf(stderr, "ERROR: no video track found in input file\n");
        return 1;
    }

    // position the source stream at the start of the mdat payload and
    // hide it behind a forward-only facade
    AP4_UI64 mdat_payload = 0;
    result = FindMdatPayloadOffset(file, mdat_payload);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: no mdat found in input file\n");
        return 1;
    }
    result = input->Seek(mdat_payload);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot position input stream (%d)\n", result);
        return 1;
    }
    ForwardOnlyStream source(*input);

    // create the empty scratch stream
    AP4_ByteStream* scratch = NULL;
    result = AP4_FileByteStream::Create(scratch_filename,
                                        AP4_FileByteStream::STREAM_MODE_WRITE,
                                        scratch);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot create scratch file (%d)\n", result);
        return 1;
    }

    // create the reader
    AP4_EditedSampleReader* reader = NULL;
    result = AP4_EditedSampleReader::Create(*track, source, *scratch, capacity, reader);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot create edited sample reader (%d)\n", result);
        return 1;
    }

    // create the output stream and write the stream header
    AP4_ByteStream* output = NULL;
    result = AP4_FileByteStream::Create(output_filename,
                                        AP4_FileByteStream::STREAM_MODE_WRITE,
                                        output);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot create output file (%d)\n", result);
        delete reader;
        return 1;
    }
    result = output->WriteString("EPRV");
    if (AP4_SUCCEEDED(result)) result = output->WriteUI32(AP4_EDITED_PREVIEW_FORMAT_VERSION);
    if (AP4_SUCCEEDED(result)) result = output->WriteUI64(reader->GetTimeScale());
    if (AP4_SUCCEEDED(result)) result = output->WriteUI64(reader->GetRequiredSpoolBytes());
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot write output header (%d)\n", result);
        delete reader;
        return 1;
    }

    // export the packet sequence
    AP4_UI64 packet_count = 0;
    AP4_UI64 media_bytes  = 0;
    for (;;) {
        AP4_EditedSampleReader::Packet packet;
        result = reader->ReadNext(packet);
        if (result == AP4_ERROR_EOS) break;
        if (AP4_FAILED(result)) {
            fprintf(stderr, "ERROR: reading edited packets failed (%d)\n", result);
            delete reader;
            return 1;
        }

        result = output->WriteUI32((AP4_UI32)packet.kind);
        if (AP4_SUCCEEDED(result)) result = output->WriteUI32(packet.edit_index);
        if (AP4_SUCCEEDED(result)) result = output->WriteUI32(packet.source_sample_index);
        if (AP4_SUCCEEDED(result)) result = output->WriteUI64(packet.dts);
        if (AP4_SUCCEEDED(result)) result = output->WriteUI64((AP4_UI64)packet.cts);
        if (AP4_SUCCEEDED(result)) result = output->WriteUI32(packet.duration);
        if (AP4_SUCCEEDED(result)) result = output->WriteUI32(packet.description_index);
        if (AP4_SUCCEEDED(result)) result = output->WriteUI32(packet.sync?1:0);
        if (AP4_SUCCEEDED(result)) result = output->WriteUI64(packet.movie_begin);
        if (AP4_SUCCEEDED(result)) result = output->WriteUI64(packet.movie_duration);
        if (AP4_SUCCEEDED(result)) result = output->WriteUI32(packet.payload.GetDataSize());
        if (AP4_SUCCEEDED(result) && packet.payload.GetDataSize()) {
            result = output->Write(packet.payload.GetData(), packet.payload.GetDataSize());
        }
        if (AP4_FAILED(result)) {
            fprintf(stderr, "ERROR: cannot write packet record (%d)\n", result);
            delete reader;
            return 1;
        }
        ++packet_count;
        media_bytes += packet.payload.GetDataSize();
    }

    output->Flush();
    printf("exported %llu packets, %llu media payload bytes, timescale %llu, spool peak %llu\n",
           (unsigned long long)packet_count,
           (unsigned long long)media_bytes,
           (unsigned long long)reader->GetTimeScale(),
           (unsigned long long)reader->GetRequiredSpoolBytes());

    delete reader;
    output->Release();
    scratch->Release();
    input->Release();
    return 0;
}
