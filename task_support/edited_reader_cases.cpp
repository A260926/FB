/*****************************************************************
|
|    Regression cases for AP4_EditedSampleReader / AP4_SampleSpool.
|
|    Micro MP4 files are assembled byte-by-byte here (independently
|    of the library writers) and then demuxed with the original
|    library. The source stream genuinely refuses Seek(), the scratch
|    stream records every address it is asked to touch.
|
+---------------------------------------------------------------------*/
#include "check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Ap4.h"
#include "Ap4LinearReader.h"
#include "Ap4EditedSampleReader.h"
#include "Ap4SampleSpool.h"

/*----------------------------------------------------------------------
|   micro MP4 builder
+---------------------------------------------------------------------*/
struct SampleSpec {
    AP4_UI32 size;
    AP4_UI32 duration;
    AP4_SI32 cts_offset;
    bool     sync;
};

struct ChunkSpec {
    AP4_UI32 sample_count;
    AP4_UI32 desc_index;  // zero-based
    AP4_UI32 rel_offset;  // relative to the mdat payload start
};

struct ElstSpec {
    AP4_UI64 segment_duration;
    AP4_SI64 media_time;
    AP4_UI16 rate;
    AP4_UI16 rate_fraction;
};

struct Scenario {
    AP4_UI32 movie_timescale;
    AP4_UI64 movie_duration;   // written in mvhd and tkhd
    AP4_UI32 media_timescale;
    AP4_UI64 media_duration;   // written in mdhd
    std::vector<SampleSpec> samples; // decode order
    std::vector<ChunkSpec>  chunks;  // decode-order partition
    int  ctts_version;       // -1: no ctts atom
    bool write_stss;         // write stss when some sample is not sync
    std::vector<ElstSpec> elst; // empty: no edts at all
    int  elst_version;
    bool omit_stsc;
    AP4_UI32 desc_count;
    AP4_UI32 media_size_override; // 0: derive from the chunks

    Scenario() :
        movie_timescale(3), movie_duration(0),
        media_timescale(4), media_duration(0),
        ctts_version(-1), write_stss(true),
        elst_version(0), omit_stsc(false),
        desc_count(1), media_size_override(0) {}
};

struct BuiltFile {
    std::vector<AP4_UI08> bytes;
    AP4_UI64              mdat_payload_offset;
    AP4_UI64              mdat_payload_size;
};

static AP4_UI08
SamplePattern(AP4_UI32 index)
{
    return (AP4_UI08)(0xA0+index);
}

class ByteVec {
public:
    std::vector<AP4_UI08> b;
    size_t Pos() const { return b.size(); }
    void U8(AP4_UI08 v)  { b.push_back(v); }
    void U16(AP4_UI16 v) { U8((AP4_UI08)(v>>8)); U8((AP4_UI08)(v&0xFF)); }
    void U32(AP4_UI32 v) { U16((AP4_UI16)(v>>16)); U16((AP4_UI16)(v&0xFFFF)); }
    void U64(AP4_UI64 v) { U32((AP4_UI32)(v>>32)); U32((AP4_UI32)(v&0xFFFFFFFFUL)); }
    void Tag(const char* t) { U8((AP4_UI08)t[0]); U8((AP4_UI08)t[1]); U8((AP4_UI08)t[2]); U8((AP4_UI08)t[3]); }
    void Bytes(const AP4_UI08* data, size_t size) { for (size_t i=0; i<size; i++) b.push_back(data[i]); }
    void Zeros(size_t size) { for (size_t i=0; i<size; i++) b.push_back(0); }
    void Patch32(size_t at, AP4_UI32 v) {
        b[at]   = (AP4_UI08)(v>>24);
        b[at+1] = (AP4_UI08)((v>>16)&0xFF);
        b[at+2] = (AP4_UI08)((v>>8)&0xFF);
        b[at+3] = (AP4_UI08)(v&0xFF);
    }
    size_t AtomBegin(const char* type) { size_t at = Pos(); U32(0); Tag(type); return at; }
    void   AtomEnd(size_t at) { Patch32(at, (AP4_UI32)(Pos()-at)); }
};

static bool
BuildFile(const Scenario& sc, BuiltFile& out)
{
    // lay out the media payload
    AP4_UI64 derived = 0;
    {
        AP4_UI32 sample  = 0;
        for (size_t c=0; c<sc.chunks.size(); c++) {
            AP4_UI64 chunk_bytes = 0;
            for (AP4_UI32 i=0; i<sc.chunks[c].sample_count; i++) {
                if (sample >= sc.samples.size()) return false;
                chunk_bytes += sc.samples[sample++].size;
            }
            AP4_UI64 end = (AP4_UI64)sc.chunks[c].rel_offset+chunk_bytes;
            if (end > derived) derived = end;
        }
        if (sample != sc.samples.size()) return false;
    }
    // an explicit override wins (sample bytes outside are clipped),
    // so that ranges can be made to escape the mdat on purpose
    AP4_UI64 media_size = sc.media_size_override ? sc.media_size_override : derived;
    std::vector<AP4_UI08> media((size_t)media_size, 0);
    {
        AP4_UI32 sample = 0;
        for (size_t c=0; c<sc.chunks.size(); c++) {
            AP4_UI64 at = sc.chunks[c].rel_offset;
            for (AP4_UI32 i=0; i<sc.chunks[c].sample_count; i++, sample++) {
                for (AP4_UI32 k=0; k<sc.samples[sample].size; k++) {
                    if (at+k < media.size()) media[(size_t)(at+k)] = SamplePattern(sample);
                }
                at += sc.samples[sample].size;
            }
        }
    }

    ByteVec f;
    // ftyp
    {
        size_t a = f.AtomBegin("ftyp");
        f.Tag("isom"); f.U32(0); f.Tag("isom");
        f.AtomEnd(a);
    }
    // moov
    std::vector<size_t> stco_patch;
    {
        size_t moov = f.AtomBegin("moov");
        // mvhd
        {
            size_t a = f.AtomBegin("mvhd");
            f.U8(0); f.U8(0); f.U8(0); f.U8(0);       // version+flags
            f.U32(0); f.U32(0);                        // creation/modification
            f.U32(sc.movie_timescale);
            f.U32((AP4_UI32)sc.movie_duration);
            f.U32(0x00010000);                         // rate
            f.U16(0x0100); f.U16(0);                   // volume + reserved
            f.U32(0); f.U32(0);                        // reserved
            static const AP4_UI32 matrix[9] = {
                0x00010000,0,0, 0,0x00010000,0, 0,0,0x40000000
            };
            for (int i=0; i<9; i++) f.U32(matrix[i]);
            for (int i=0; i<6; i++) f.U32(0);          // predefined
            f.U32(2);                                  // next track id
            f.AtomEnd(a);
        }
        // trak
        {
            size_t trak = f.AtomBegin("trak");
            // tkhd
            {
                size_t a = f.AtomBegin("tkhd");
                f.U8(0); f.U8(0); f.U8(0); f.U8(3);    // version+flags
                f.U32(0); f.U32(0);                    // creation/modification
                f.U32(1);                              // track id
                f.U32(0);                              // reserved
                f.U32((AP4_UI32)sc.movie_duration);
                f.U32(0); f.U32(0);                    // reserved
                f.U16(0); f.U16(0);                    // layer + alternate group
                f.U16(0); f.U16(0);                    // volume + reserved
                static const AP4_UI32 matrix[9] = {
                    0x00010000,0,0, 0,0x00010000,0, 0,0,0x40000000
                };
                for (int i=0; i<9; i++) f.U32(matrix[i]);
                f.U32(16<<16); f.U32(16<<16);          // width/height
                f.AtomEnd(a);
            }
            // edts/elst
            if (!sc.elst.empty()) {
                size_t edts = f.AtomBegin("edts");
                size_t elst = f.AtomBegin("elst");
                f.U8((AP4_UI08)sc.elst_version); f.U8(0); f.U8(0); f.U8(0);
                f.U32((AP4_UI32)sc.elst.size());
                for (size_t i=0; i<sc.elst.size(); i++) {
                    const ElstSpec& e = sc.elst[i];
                    if (sc.elst_version == 0) {
                        f.U32((AP4_UI32)e.segment_duration);
                        f.U32((AP4_UI32)e.media_time);
                    } else {
                        f.U64(e.segment_duration);
                        f.U64((AP4_UI64)e.media_time);
                    }
                    f.U16(e.rate);
                    f.U16(e.rate_fraction);
                }
                f.AtomEnd(elst);
                f.AtomEnd(edts);
            }
            // mdia
            {
                size_t mdia = f.AtomBegin("mdia");
                // mdhd
                {
                    size_t a = f.AtomBegin("mdhd");
                    f.U8(0); f.U8(0); f.U8(0); f.U8(0);
                    f.U32(0); f.U32(0);
                    f.U32(sc.media_timescale);
                    f.U32((AP4_UI32)sc.media_duration);
                    f.U16(0x55C4);                       // language 'und'
                    f.U16(0);
                    f.AtomEnd(a);
                }
                // hdlr
                {
                    size_t a = f.AtomBegin("hdlr");
                    f.U8(0); f.U8(0); f.U8(0); f.U8(0);
                    f.U32(0);                            // pre_defined
                    f.Tag("vide");                       // handler type
                    f.U32(0); f.U32(0); f.U32(0);        // reserved
                    const char* name = "VideoHandler";
                    f.Bytes((const AP4_UI08*)name, 12);  // includes NUL
                    f.AtomEnd(a);
                }
                // minf
                {
                    size_t minf = f.AtomBegin("minf");
                    // vmhd
                    {
                        size_t a = f.AtomBegin("vmhd");
                        f.U8(0); f.U8(0); f.U8(0); f.U8(1);
                        f.U16(0); f.U16(0); f.U16(0); f.U16(0);
                        f.AtomEnd(a);
                    }
                    // dinf
                    {
                        size_t dinf = f.AtomBegin("dinf");
                        size_t dref = f.AtomBegin("dref");
                        f.U8(0); f.U8(0); f.U8(0); f.U8(0);
                        f.U32(1);
                        size_t url = f.AtomBegin("url ");
                        f.U8(0); f.U8(0); f.U8(0); f.U8(1);
                        f.AtomEnd(url);
                        f.AtomEnd(dref);
                        f.AtomEnd(dinf);
                    }
                    // stbl
                    {
                        size_t stbl = f.AtomBegin("stbl");
                        // stsd
                        {
                            size_t a = f.AtomBegin("stsd");
                            f.U8(0); f.U8(0); f.U8(0); f.U8(0);
                            f.U32(sc.desc_count);
                            for (AP4_UI32 i=0; i<sc.desc_count; i++) {
                                char type[5] = {'z','z','0','0','\0'};
                                type[3] = (char)('1'+i);
                                size_t e = f.AtomBegin(type);
                                f.Zeros(6); f.U16(1);    // reserved + data reference index
                                f.AtomEnd(e);
                            }
                            f.AtomEnd(a);
                        }
                        // stts
                        {
                            size_t a = f.AtomBegin("stts");
                            f.U8(0); f.U8(0); f.U8(0); f.U8(0);
                            std::vector<AP4_UI32> counts, deltas;
                            for (size_t i=0; i<sc.samples.size(); i++) {
                                if (!deltas.empty() && deltas.back() == sc.samples[i].duration) {
                                    counts.back()++;
                                } else {
                                    counts.push_back(1);
                                    deltas.push_back(sc.samples[i].duration);
                                }
                            }
                            f.U32((AP4_UI32)counts.size());
                            for (size_t i=0; i<counts.size(); i++) {
                                f.U32(counts[i]); f.U32(deltas[i]);
                            }
                            f.AtomEnd(a);
                        }
                        // stsc
                        if (!sc.omit_stsc) {
                            size_t a = f.AtomBegin("stsc");
                            f.U8(0); f.U8(0); f.U8(0); f.U8(0);
                            f.U32((AP4_UI32)sc.chunks.size());
                            for (size_t i=0; i<sc.chunks.size(); i++) {
                                f.U32((AP4_UI32)(i+1));
                                f.U32(sc.chunks[i].sample_count);
                                f.U32(sc.chunks[i].desc_index+1);
                            }
                            f.AtomEnd(a);
                        }
                        // stsz
                        {
                            size_t a = f.AtomBegin("stsz");
                            f.U8(0); f.U8(0); f.U8(0); f.U8(0);
                            f.U32(0);                      // uniform size: no
                            f.U32((AP4_UI32)sc.samples.size());
                            for (size_t i=0; i<sc.samples.size(); i++) {
                                f.U32(sc.samples[i].size);
                            }
                            f.AtomEnd(a);
                        }
                        // stco (patched later)
                        {
                            size_t a = f.AtomBegin("stco");
                            f.U8(0); f.U8(0); f.U8(0); f.U8(0);
                            f.U32((AP4_UI32)sc.chunks.size());
                            for (size_t i=0; i<sc.chunks.size(); i++) {
                                stco_patch.push_back(f.Pos());
                                f.U32(0);
                            }
                            f.AtomEnd(a);
                        }
                        // ctts
                        if (sc.ctts_version >= 0) {
                            size_t a = f.AtomBegin("ctts");
                            f.U8((AP4_UI08)sc.ctts_version); f.U8(0); f.U8(0); f.U8(0);
                            std::vector<AP4_UI32> counts;
                            std::vector<AP4_SI32> offsets;
                            for (size_t i=0; i<sc.samples.size(); i++) {
                                if (!offsets.empty() && offsets.back() == sc.samples[i].cts_offset) {
                                    counts.back()++;
                                } else {
                                    counts.push_back(1);
                                    offsets.push_back(sc.samples[i].cts_offset);
                                }
                            }
                            f.U32((AP4_UI32)counts.size());
                            for (size_t i=0; i<counts.size(); i++) {
                                f.U32(counts[i]); f.U32((AP4_UI32)offsets[i]);
                            }
                            f.AtomEnd(a);
                        }
                        // stss
                        {
                            bool any_non_sync = false;
                            for (size_t i=0; i<sc.samples.size(); i++) {
                                if (!sc.samples[i].sync) any_non_sync = true;
                            }
                            if (sc.write_stss && any_non_sync) {
                                size_t a = f.AtomBegin("stss");
                                f.U8(0); f.U8(0); f.U8(0); f.U8(0);
                                AP4_UI32 count = 0;
                                for (size_t i=0; i<sc.samples.size(); i++) {
                                    if (sc.samples[i].sync) count++;
                                }
                                f.U32(count);
                                for (size_t i=0; i<sc.samples.size(); i++) {
                                    if (sc.samples[i].sync) f.U32((AP4_UI32)(i+1));
                                }
                                f.AtomEnd(a);
                            }
                        }
                        f.AtomEnd(stbl);
                    }
                    f.AtomEnd(minf);
                }
                f.AtomEnd(mdia);
            }
            f.AtomEnd(trak);
        }
        f.AtomEnd(moov);
    }
    // mdat
    size_t mdat = f.AtomBegin("mdat");
    size_t payload_at = f.Pos();
    f.Bytes(media.size() ? &media[0] : NULL, media.size());
    f.AtomEnd(mdat);

    // patch the chunk offsets
    for (size_t i=0; i<sc.chunks.size(); i++) {
        f.Patch32(stco_patch[i], (AP4_UI32)(payload_at+sc.chunks[i].rel_offset));
    }

    out.bytes = f.b;
    out.mdat_payload_offset = payload_at;
    out.mdat_payload_size   = media_size;
    return true;
}

/*----------------------------------------------------------------------
|   test streams
+---------------------------------------------------------------------*/
class ForwardSource : public AP4_ByteStream
{
public:
    ForwardSource(const AP4_UI08* data, AP4_UI64 size, AP4_UI64 position) :
        m_Data(data), m_Size(size), m_Pos(position),
        m_Truncate(~(AP4_UI64)0), m_EosError(false),
        m_SeekCalls(0), m_BytesRead(0), m_MaxPos(position) {}

    AP4_Result ReadPartial(void* buffer, AP4_Size bytes_to_read, AP4_Size& bytes_read) {
        AP4_UI64 limit = m_Truncate < m_Size ? m_Truncate : m_Size;
        if (m_Pos >= limit) {
            bytes_read = 0;
            return m_EosError ? AP4_ERROR_EOS : AP4_SUCCESS;
        }
        AP4_UI64 available = limit-m_Pos;
        if (bytes_to_read > available) bytes_to_read = (AP4_Size)available;
        AP4_UI64 start = m_Pos;
        memcpy(buffer, m_Data+(size_t)m_Pos, bytes_to_read);
        m_Pos        += bytes_to_read;
        m_BytesRead  += bytes_to_read;
        if (m_Pos > m_MaxPos) m_MaxPos = m_Pos;
        bytes_read = bytes_to_read;
        m_Ranges.push_back(std::make_pair(start, m_Pos));
        return AP4_SUCCESS;
    }
    AP4_Result WritePartial(const void*, AP4_Size, AP4_Size& bytes_written) {
        bytes_written = 0;
        return AP4_ERROR_NOT_SUPPORTED;
    }
    AP4_Result Seek(AP4_Position) {
        ++m_SeekCalls;
        return AP4_ERROR_NOT_SUPPORTED;
    }
    AP4_Result Tell(AP4_Position& position) { position = m_Pos; return AP4_SUCCESS; }
    AP4_Result GetSize(AP4_LargeSize& size) { size = m_Size; return AP4_SUCCESS; }
    void AddReference() {}
    void Release()      {}

    const AP4_UI08* m_Data;
    AP4_UI64        m_Size;
    AP4_UI64        m_Pos;
    AP4_UI64        m_Truncate;
    bool            m_EosError;
    unsigned        m_SeekCalls;
    AP4_UI64        m_BytesRead;
    AP4_UI64        m_MaxPos;
    std::vector<std::pair<AP4_UI64,AP4_UI64> > m_Ranges;
};

class RecordingScratch : public AP4_ByteStream
{
public:
    RecordingScratch(AP4_UI64 capacity) :
        m_Capacity(capacity), m_Pos(0),
        m_SeekCalls(0), m_ReadCalls(0), m_WriteCalls(0),
        m_WriteLimit(~(AP4_UI64)0), m_Written(0),
        m_RangeViolation(false), m_Mem((size_t)capacity) {}

    AP4_Result ReadPartial(void* buffer, AP4_Size bytes_to_read, AP4_Size& bytes_read) {
        ++m_ReadCalls;
        if (m_Pos+bytes_to_read > m_Capacity) {
            m_RangeViolation = true;
            bytes_read = 0;
            return AP4_ERROR_READ_FAILED;
        }
        memcpy(buffer, &m_Mem[(size_t)m_Pos], bytes_to_read);
        m_Pos += bytes_to_read;
        bytes_read = bytes_to_read;
        return AP4_SUCCESS;
    }
    AP4_Result WritePartial(const void* buffer, AP4_Size bytes_to_write, AP4_Size& bytes_written) {
        ++m_WriteCalls;
        if (m_Written+bytes_to_write > m_WriteLimit) {
            bytes_written = 0;
            return AP4_ERROR_WRITE_FAILED;
        }
        if (m_Pos+bytes_to_write > m_Capacity) {
            m_RangeViolation = true;
            bytes_written = 0;
            return AP4_ERROR_WRITE_FAILED;
        }
        AP4_UI64 start = m_Pos;
        memcpy(&m_Mem[(size_t)m_Pos], buffer, bytes_to_write);
        m_Pos      += bytes_to_write;
        m_Written  += bytes_to_write;
        bytes_written = bytes_to_write;
        m_WriteRanges.push_back(std::make_pair(start, m_Pos));
        return AP4_SUCCESS;
    }
    AP4_Result Seek(AP4_Position position) {
        ++m_SeekCalls;
        if (position > m_Capacity) {
            m_RangeViolation = true;
            return AP4_ERROR_OUT_OF_RANGE;
        }
        m_Pos = position;
        return AP4_SUCCESS;
    }
    AP4_Result Tell(AP4_Position& position) { position = m_Pos; return AP4_SUCCESS; }
    AP4_Result GetSize(AP4_LargeSize& size) { size = m_Capacity; return AP4_SUCCESS; }
    void AddReference() {}
    void Release()      {}

    AP4_UI64        m_Capacity;
    AP4_UI64        m_Pos;
    unsigned        m_SeekCalls;
    unsigned        m_ReadCalls;
    unsigned        m_WriteCalls;
    AP4_UI64        m_WriteLimit;
    AP4_UI64        m_Written;
    bool            m_RangeViolation;
    std::vector<AP4_UI08> m_Mem;
    std::vector<std::pair<AP4_UI64,AP4_UI64> > m_WriteRanges;
};

/*----------------------------------------------------------------------
|   session helper
+---------------------------------------------------------------------*/
struct Session {
    BuiltFile            built;
    AP4_MemoryByteStream* parse_stream;
    AP4_File*             file;
    AP4_Track*            track;
    ForwardSource*        source;
    RecordingScratch*     scratch;
    AP4_EditedSampleReader* reader;

    Session() :
        parse_stream(0), file(0), track(0),
        source(0), scratch(0), reader(0) {}

    ~Session() {
        delete reader;
        delete source;
        delete scratch;
        delete file;
        if (parse_stream) parse_stream->Release();
    }

    int Parse() {
        parse_stream = new AP4_MemoryByteStream(&built.bytes[0], (AP4_Size)built.bytes.size());
        file = new AP4_File(*parse_stream);
        if (!file->GetMovie()) return 1;
        track = file->GetMovie()->GetTrack(AP4_Track::TYPE_VIDEO);
        if (!track) return 1;
        return 0;
    }

    void MakeStreams(AP4_UI64 capacity) {
        source  = new ForwardSource(&built.bytes[0],
                                    (AP4_UI64)built.bytes.size(),
                                    built.mdat_payload_offset);
        scratch = new RecordingScratch(capacity);
    }
};

/*----------------------------------------------------------------------
|   packet expectation helper
+---------------------------------------------------------------------*/
struct Expect {
    int         kind;    // 0 = media, 1 = gap
    AP4_UI32    edit;
    AP4_Ordinal sample;  // media only
    AP4_UI64    dts;
    AP4_SI64    cts;
    AP4_UI32    dur;
    AP4_UI32    desc;
    bool        sync;
    AP4_UI64    begin;
    AP4_UI64    mdur;
};

static int
CheckPacket(const AP4_EditedSampleReader::Packet& p, const Expect& e)
{
    TASK_CHECK((int)p.kind == e.kind);
    TASK_CHECK(p.edit_index == e.edit);
    TASK_CHECK(p.movie_begin == e.begin);
    TASK_CHECK(p.movie_duration == e.mdur);
    if (e.kind == (int)AP4_EditedSampleReader::Packet::GAP) {
        TASK_CHECK(p.source_sample_index == (AP4_Ordinal)-1);
        TASK_CHECK(p.dts == 0);
        TASK_CHECK(p.cts == 0);
        TASK_CHECK(p.duration == 0);
        TASK_CHECK(p.description_index == 0);
        TASK_CHECK(p.sync == false);
        TASK_CHECK(p.payload.GetDataSize() == 0);
    } else {
        TASK_CHECK(p.source_sample_index == e.sample);
        TASK_CHECK(p.dts == e.dts);
        TASK_CHECK(p.cts == e.cts);
        TASK_CHECK(p.duration == e.dur);
        TASK_CHECK(p.description_index == e.desc);
        TASK_CHECK(p.sync == e.sync);
        TASK_CHECK(p.payload.GetDataSize() > 0);
        for (AP4_Size i=0; i<p.payload.GetDataSize(); i++) {
            if (p.payload.GetData()[i] != SamplePattern(e.sample)) return 1;
        }
    }
    return 0;
}

// drain a reader into a vector of packets; returns the final ReadNext
// status (AP4_ERROR_EOS on a clean end)
static AP4_Result
Drain(AP4_EditedSampleReader* reader, std::vector<AP4_EditedSampleReader::Packet>& packets)
{
    for (;;) {
        AP4_EditedSampleReader::Packet packet;
        AP4_Result result = reader->ReadNext(packet);
        if (result == AP4_ERROR_EOS) return result;
        if (AP4_FAILED(result)) return result;
        packets.push_back(packet);
    }
}

// check that the source was consumed in a single forward pass
static int
CheckSinglePass(const ForwardSource* source, AP4_UI64 first, AP4_UI64 last)
{
    TASK_CHECK(source->m_SeekCalls == 0);
    TASK_CHECK(!source->m_Ranges.empty());
    TASK_CHECK(source->m_Ranges[0].first == first);
    for (size_t i=1; i<source->m_Ranges.size(); i++) {
        TASK_CHECK(source->m_Ranges[i].first == source->m_Ranges[i-1].second);
    }
    TASK_CHECK(source->m_Ranges.back().second == last);
    TASK_CHECK(source->m_BytesRead == last-first);
    return 0;
}

/*----------------------------------------------------------------------
|   shared scenarios
+---------------------------------------------------------------------*/

// Main scenario: B frames (ctts v1, negative offsets), reversed chunk
// order, a description switch, an unreferenced sample, a gap edit and
// a repeated edit. Media timescale 4, movie timescale 3, C = 12.
static Scenario
MainScenario()
{
    Scenario sc;
    sc.movie_timescale = 3;
    sc.media_timescale = 4;
    sc.movie_duration  = 5;
    sc.media_duration  = 7;
    sc.desc_count      = 2;
    sc.ctts_version    = 1;
    const AP4_UI32 sizes[7]   = {10, 20, 30, 40, 50, 60, 15};
    const AP4_SI32 offsets[7] = {0, 2, -1, -1, 0, 0, 0};
    const bool     syncs[7]   = {true, false, false, false, true, false, false};
    for (int i=0; i<7; i++) {
        SampleSpec s;
        s.size       = sizes[i];
        s.duration   = 1;
        s.cts_offset = offsets[i];
        s.sync       = syncs[i];
        sc.samples.push_back(s);
    }
    ChunkSpec c0 = {2, 0, 85};  // s0, s1
    ChunkSpec c1 = {2, 1, 0};   // s2, s3 (description 2)
    ChunkSpec c2 = {2, 0, 115}; // s4, s5
    ChunkSpec c3 = {1, 0, 70};  // s6 (unreferenced, physically between)
    sc.chunks.push_back(c0);
    sc.chunks.push_back(c1);
    sc.chunks.push_back(c2);
    sc.chunks.push_back(c3);
    ElstSpec e0 = {2, -1, 1, 0}; // gap
    ElstSpec e1 = {1,  1, 1, 0}; // media time [1, 7/3)
    ElstSpec e2 = {1,  4, 1, 0}; // media time [4, 16/3)
    ElstSpec e3 = {1,  1, 1, 0}; // repeat of e1
    sc.elst.push_back(e0);
    sc.elst.push_back(e1);
    sc.elst.push_back(e2);
    sc.elst.push_back(e3);
    return sc;
}

// Hand-computed spool peak for the main scenario: the maximum of live
// spooled bytes is reached when s5 is spooled: at that point s0..s3
// are still referenced by the repeated edit, so the peak is
// 10+20+30+40+60 = 160 bytes.
static const AP4_UI64 MAIN_SCENARIO_PEAK = 160;

static const Expect MAIN_EXPECT[] = {
    {1, 0, 0,         0, 0, 0, 0, false, 0,  8}, // GAP, 2 movie units -> 8 C units
    {0, 1, 0,         0, 0, 1, 0, true,  0,  0}, // s0 hidden preroll
    {0, 1, 1,         1, 3, 1, 0, false, 0,  0}, // s1 not visible
    {0, 1, 2,         2, 1, 1, 1, false, 8,  3}, // s2 window [8,11)
    {0, 1, 3,         3, 2, 1, 1, false, 11, 1}, // s3 clipped to [11,12)
    {0, 2, 4,         4, 4, 1, 0, true,  12, 3}, // s4 window [12,15)
    {0, 2, 5,         5, 5, 1, 0, false, 15, 1}, // s5 clipped to [15,16)
    {0, 3, 0,         0, 0, 1, 0, true,  0,  0}, // s0 again (repeated edit)
    {0, 3, 1,         1, 3, 1, 0, false, 0,  0},
    {0, 3, 2,         2, 1, 1, 1, false, 16, 3},
    {0, 3, 3,         3, 2, 1, 1, false, 19, 1}
};
static const size_t MAIN_EXPECT_COUNT = sizeof(MAIN_EXPECT)/sizeof(MAIN_EXPECT[0]);

/*----------------------------------------------------------------------
|   forward group
+---------------------------------------------------------------------*/
static int
forward_basic_playback()
{
    Session session;
    TASK_CHECK(BuildFile(MainScenario(), session.built));
    TASK_CHECK(session.Parse() == 0);
    session.MakeStreams(MAIN_SCENARIO_PEAK);

    AP4_EditedSampleReader* reader = NULL;
    AP4_Result result = AP4_EditedSampleReader::Create(*session.track,
                                                       *session.source,
                                                       *session.scratch,
                                                       MAIN_SCENARIO_PEAK,
                                                       reader);
    TASK_CHECK(result == AP4_SUCCESS);
    TASK_CHECK(reader != NULL);
    session.reader = reader;

    // the factory must not have read media bytes or touched scratch
    TASK_CHECK(session.source->m_BytesRead == 0);
    TASK_CHECK(session.source->m_Pos == session.built.mdat_payload_offset);
    TASK_CHECK(session.scratch->m_SeekCalls == 0);
    TASK_CHECK(session.scratch->m_WriteCalls == 0);
    TASK_CHECK(session.scratch->m_ReadCalls == 0);

    TASK_CHECK(reader->GetTimeScale() == 12);
    TASK_CHECK(reader->GetRequiredSpoolBytes() == MAIN_SCENARIO_PEAK);

    std::vector<AP4_EditedSampleReader::Packet> packets;
    AP4_Result final = Drain(reader, packets);
    TASK_CHECK(final == AP4_ERROR_EOS);
    // repeated calls after the end keep returning EOS
    {
        AP4_EditedSampleReader::Packet extra;
        TASK_CHECK(reader->ReadNext(extra) == AP4_ERROR_EOS);
        TASK_CHECK(reader->ReadNext(extra) == AP4_ERROR_EOS);
    }
    TASK_CHECK(packets.size() == MAIN_EXPECT_COUNT);
    if (packets.size() == MAIN_EXPECT_COUNT) {
        for (size_t i=0; i<MAIN_EXPECT_COUNT; i++) {
            if (CheckPacket(packets[i], MAIN_EXPECT[i])) return 1;
        }
    }

    // the source was consumed exactly once, strictly forward, and the
    // frontier stopped at the end of the last referenced sample
    TASK_CHECK(CheckSinglePass(session.source,
                               session.built.mdat_payload_offset,
                               session.built.mdat_payload_offset+225) == 0);

    // every scratch address stayed inside [0, capacity)
    TASK_CHECK(!session.scratch->m_RangeViolation);
    for (size_t i=0; i<session.scratch->m_WriteRanges.size(); i++) {
        TASK_CHECK(session.scratch->m_WriteRanges[i].second <= MAIN_SCENARIO_PEAK);
    }

    // export the input for the EditedPreview app regression
    {
        FILE* out = fopen(".task-build/preview-input.mp4", "wb");
        TASK_CHECK(out != NULL);
        size_t written = fwrite(&session.built.bytes[0], 1, session.built.bytes.size(), out);
        TASK_CHECK(written == session.built.bytes.size());
        fclose(out);
    }
    return 0;
}

static int
forward_no_elst_full_track()
{
    // no edit list: the whole track plays in decode order, with
    // positive-only ctts v0 offsets and a description switch
    Scenario sc;
    sc.movie_timescale = 1000;
    sc.media_timescale = 1000;
    sc.movie_duration  = 160;
    sc.media_duration  = 160;
    sc.desc_count      = 2;
    sc.ctts_version    = 0;
    const AP4_UI32 sizes[3] = {10, 11, 12};
    const AP4_UI32 durs[3]  = {40, 40, 80};
    const bool     syncs[3] = {true, false, false};
    for (int i=0; i<3; i++) {
        SampleSpec s;
        s.size       = sizes[i];
        s.duration   = durs[i];
        s.cts_offset = 0;
        s.sync       = syncs[i];
        sc.samples.push_back(s);
    }
    ChunkSpec c0 = {2, 0, 90}; // s0, s1
    ChunkSpec c1 = {1, 1, 0};  // s2 (description 2, physically first)
    sc.chunks.push_back(c0);
    sc.chunks.push_back(c1);

    Session session;
    TASK_CHECK(BuildFile(sc, session.built));
    TASK_CHECK(session.Parse() == 0);
    session.MakeStreams(64);

    AP4_EditedSampleReader* reader = NULL;
    TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                              *session.source,
                                              *session.scratch,
                                              64,
                                              reader) == AP4_SUCCESS);
    session.reader = reader;
    TASK_CHECK(reader->GetTimeScale() == 1000);
    // delivery order s0, s1, s2; physical order s2, s0, s1:
    // spooling s2+s0 peaks at 22, then s2+s1 peaks at 23
    TASK_CHECK(reader->GetRequiredSpoolBytes() == 23);

    std::vector<AP4_EditedSampleReader::Packet> packets;
    TASK_CHECK(Drain(reader, packets) == AP4_ERROR_EOS);
    TASK_CHECK(packets.size() == 3);
    if (packets.size() == 3) {
        const Expect e0 = {0, 0, 0, 0,  0, 40, 0, true,  0,  40};
        const Expect e1 = {0, 0, 1, 40, 40, 40, 0, false, 40, 40};
        const Expect e2 = {0, 0, 2, 80, 80, 80, 1, false, 80, 80};
        TASK_CHECK(CheckPacket(packets[0], e0) == 0);
        TASK_CHECK(CheckPacket(packets[1], e1) == 0);
        TASK_CHECK(CheckPacket(packets[2], e2) == 0);
    }
    TASK_CHECK(CheckSinglePass(session.source,
                               session.built.mdat_payload_offset,
                               session.built.mdat_payload_offset+111) == 0);
    TASK_CHECK(!session.scratch->m_RangeViolation);
    return 0;
}

/*----------------------------------------------------------------------
|   budget group
+---------------------------------------------------------------------*/
static int
budget_peak_exact_and_below()
{
    BuiltFile built;
    TASK_CHECK(BuildFile(MainScenario(), built));

    // one byte below the planned peak must be rejected without
    // disturbing either stream
    {
        Session session;
        session.built = built;
        TASK_CHECK(session.Parse() == 0);
        session.MakeStreams(MAIN_SCENARIO_PEAK-1);
        AP4_EditedSampleReader* reader = NULL;
        AP4_Result result = AP4_EditedSampleReader::Create(*session.track,
                                                           *session.source,
                                                           *session.scratch,
                                                           MAIN_SCENARIO_PEAK-1,
                                                           reader);
        TASK_CHECK(result == AP4_ERROR_OUT_OF_RANGE);
        TASK_CHECK(reader == NULL);
        TASK_CHECK(session.source->m_Pos == built.mdat_payload_offset);
        TASK_CHECK(session.source->m_BytesRead == 0);
        TASK_CHECK(session.scratch->m_SeekCalls == 0);
        TASK_CHECK(session.scratch->m_WriteCalls == 0);
        TASK_CHECK(session.scratch->m_ReadCalls == 0);
    }

    // exactly the peak must work and must stay inside the budget
    {
        Session session;
        session.built = built;
        TASK_CHECK(session.Parse() == 0);
        session.MakeStreams(MAIN_SCENARIO_PEAK);
        AP4_EditedSampleReader* reader = NULL;
        TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                                  *session.source,
                                                  *session.scratch,
                                                  MAIN_SCENARIO_PEAK,
                                                  reader) == AP4_SUCCESS);
        session.reader = reader;
        TASK_CHECK(reader->GetRequiredSpoolBytes() == MAIN_SCENARIO_PEAK);
        std::vector<AP4_EditedSampleReader::Packet> packets;
        TASK_CHECK(Drain(reader, packets) == AP4_ERROR_EOS);
        TASK_CHECK(packets.size() == MAIN_EXPECT_COUNT);
        TASK_CHECK(!session.scratch->m_RangeViolation);
    }
    return 0;
}

static int
budget_fragment_reuse()
{
    // delivery order s1, s0, s2 with physical order s0, s1, s2:
    // s0 and s1 are spooled together (peak 100), freed, and s2 must
    // reuse the freed space at the start of the scratch area
    Scenario sc;
    sc.movie_timescale = 4;
    sc.media_timescale = 4;
    sc.movie_duration  = 3;
    sc.media_duration  = 3;
    const AP4_UI32 sizes[3] = {60, 40, 60};
    for (int i=0; i<3; i++) {
        SampleSpec s;
        s.size = sizes[i]; s.duration = 1; s.cts_offset = 0; s.sync = true;
        sc.samples.push_back(s);
    }
    ChunkSpec c0 = {3, 0, 0};
    sc.chunks.push_back(c0);
    ElstSpec e0 = {1, 1, 1, 0};
    ElstSpec e1 = {1, 0, 1, 0};
    ElstSpec e2 = {1, 2, 1, 0};
    sc.elst.push_back(e0);
    sc.elst.push_back(e1);
    sc.elst.push_back(e2);

    Session session;
    TASK_CHECK(BuildFile(sc, session.built));
    TASK_CHECK(session.Parse() == 0);
    session.MakeStreams(100);

    AP4_EditedSampleReader* reader = NULL;
    TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                              *session.source,
                                              *session.scratch,
                                              100,
                                              reader) == AP4_SUCCESS);
    session.reader = reader;
    // peak: s0+s1 = 100 while delivering s1
    TASK_CHECK(reader->GetRequiredSpoolBytes() == 100);

    std::vector<AP4_EditedSampleReader::Packet> packets;
    TASK_CHECK(Drain(reader, packets) == AP4_ERROR_EOS);
    TASK_CHECK(packets.size() == 3);
    if (packets.size() == 3) {
        const Expect e0 = {0, 0, 1, 1, 1, 1, 0, true, 0, 1};
        const Expect e1 = {0, 1, 0, 0, 0, 1, 0, true, 1, 1};
        const Expect e2 = {0, 2, 2, 2, 2, 1, 0, true, 2, 1};
        TASK_CHECK(CheckPacket(packets[0], e0) == 0);
        TASK_CHECK(CheckPacket(packets[1], e1) == 0);
        TASK_CHECK(CheckPacket(packets[2], e2) == 0);
    }
    TASK_CHECK(!session.scratch->m_RangeViolation);
    // s2 was written over the freed space at address 0
    TASK_CHECK(session.scratch->m_WriteRanges.size() == 3);
    if (session.scratch->m_WriteRanges.size() == 3) {
        TASK_CHECK(session.scratch->m_WriteRanges[0].first == 0);
        TASK_CHECK(session.scratch->m_WriteRanges[0].second == 60);
        TASK_CHECK(session.scratch->m_WriteRanges[1].first == 60);
        TASK_CHECK(session.scratch->m_WriteRanges[1].second == 100);
        TASK_CHECK(session.scratch->m_WriteRanges[2].first == 0);
        TASK_CHECK(session.scratch->m_WriteRanges[2].second == 60);
    }
    TASK_CHECK(session.scratch->m_Mem[0] == SamplePattern(2));
    return 0;
}

static int
budget_large_slot_compaction()
{
    // samples larger than the 64 KiB move buffer, so that spooling,
    // skipping and compaction all have to chunk their I/O; a live
    // 80 KiB slot is relocated by compaction while capacity is exact
    Scenario sc;
    sc.movie_timescale = 4;
    sc.media_timescale = 4;
    sc.movie_duration  = 4;
    sc.media_duration  = 3;
    const AP4_UI32 sizes[3] = {60000, 80000, 70000}; // A, B, C
    for (int i=0; i<3; i++) {
        SampleSpec s;
        s.size = sizes[i]; s.duration = 1; s.cts_offset = 0; s.sync = true;
        sc.samples.push_back(s);
    }
    // physical: A@0, 70000 unreferenced bytes, B@130000, C@210000
    ChunkSpec c0 = {1, 0, 0};
    ChunkSpec c1 = {1, 0, 130000};
    ChunkSpec c2 = {1, 0, 210000};
    sc.chunks.push_back(c0);
    sc.chunks.push_back(c1);
    sc.chunks.push_back(c2);
    ElstSpec e0 = {1, 1, 1, 0}; // B
    ElstSpec e1 = {1, 0, 1, 0}; // A
    ElstSpec e2 = {1, 2, 1, 0}; // C
    ElstSpec e3 = {1, 1, 1, 0}; // B again
    sc.elst.push_back(e0);
    sc.elst.push_back(e1);
    sc.elst.push_back(e2);
    sc.elst.push_back(e3);

    Session session;
    TASK_CHECK(BuildFile(sc, session.built));
    TASK_CHECK(session.Parse() == 0);
    // peak: A+B live while delivering B (140000), then B+C (150000)
    session.MakeStreams(150000);

    AP4_EditedSampleReader* reader = NULL;
    TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                              *session.source,
                                              *session.scratch,
                                              150000,
                                              reader) == AP4_SUCCESS);
    session.reader = reader;
    TASK_CHECK(reader->GetRequiredSpoolBytes() == 150000);

    std::vector<AP4_EditedSampleReader::Packet> packets;
    TASK_CHECK(Drain(reader, packets) == AP4_ERROR_EOS);
    TASK_CHECK(packets.size() == 4);
    if (packets.size() == 4) {
        const Expect e0 = {0, 0, 1, 1, 1, 1, 0, true, 0, 1};
        const Expect e1 = {0, 1, 0, 0, 0, 1, 0, true, 1, 1};
        const Expect e2 = {0, 2, 2, 2, 2, 1, 0, true, 2, 1};
        const Expect e3 = {0, 3, 1, 1, 1, 1, 0, true, 3, 1};
        TASK_CHECK(CheckPacket(packets[0], e0) == 0);
        TASK_CHECK(CheckPacket(packets[1], e1) == 0);
        TASK_CHECK(CheckPacket(packets[2], e2) == 0);
        TASK_CHECK(CheckPacket(packets[3], e3) == 0);
    }
    TASK_CHECK(!session.scratch->m_RangeViolation);
    // the whole media payload passed through the source exactly once
    TASK_CHECK(CheckSinglePass(session.source,
                               session.built.mdat_payload_offset,
                               session.built.mdat_payload_offset+280000) == 0);
    return 0;
}

static int
budget_all_gap_zero_capacity()
{
    // an edit list made only of empty segments: no media traffic at
    // all, so a zero capacity is sufficient
    Scenario sc;
    sc.movie_timescale = 2;
    sc.media_timescale = 4;
    sc.movie_duration  = 5;
    sc.media_duration  = 2;
    {
        SampleSpec s0 = {5, 1, 0, true};
        SampleSpec s1 = {6, 1, 0, true};
        sc.samples.push_back(s0);
        sc.samples.push_back(s1);
    }
    ChunkSpec c0 = {2, 0, 0};
    sc.chunks.push_back(c0);
    ElstSpec e0 = {3, -1, 1, 0};
    ElstSpec e1 = {2, -1, 1, 0};
    sc.elst.push_back(e0);
    sc.elst.push_back(e1);

    Session session;
    TASK_CHECK(BuildFile(sc, session.built));
    TASK_CHECK(session.Parse() == 0);
    session.MakeStreams(0);

    AP4_EditedSampleReader* reader = NULL;
    TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                              *session.source,
                                              *session.scratch,
                                              0,
                                              reader) == AP4_SUCCESS);
    session.reader = reader;
    TASK_CHECK(reader->GetRequiredSpoolBytes() == 0);
    TASK_CHECK(reader->GetTimeScale() == 4);

    std::vector<AP4_EditedSampleReader::Packet> packets;
    TASK_CHECK(Drain(reader, packets) == AP4_ERROR_EOS);
    TASK_CHECK(packets.size() == 2);
    if (packets.size() == 2) {
        const Expect e0 = {1, 0, 0, 0, 0, 0, 0, false, 0, 6};
        const Expect e1 = {1, 1, 0, 0, 0, 0, 0, false, 6, 4};
        TASK_CHECK(CheckPacket(packets[0], e0) == 0);
        TASK_CHECK(CheckPacket(packets[1], e1) == 0);
    }
    // no byte of the source was consumed and scratch was never used
    TASK_CHECK(session.source->m_BytesRead == 0);
    TASK_CHECK(session.scratch->m_SeekCalls == 0);
    TASK_CHECK(session.scratch->m_WriteCalls == 0);
    TASK_CHECK(session.scratch->m_ReadCalls == 0);
    return 0;
}

/*----------------------------------------------------------------------
|   timeline group
+---------------------------------------------------------------------*/
static int
timeline_negative_ctts()
{
    // ctts v1 with negative offsets only: the sync sample itself has
    // cts -1 and is delivered as decode-only preroll
    Scenario sc;
    sc.movie_timescale = 4;
    sc.media_timescale = 4;
    sc.movie_duration  = 2;
    sc.media_duration  = 3;
    sc.ctts_version    = 1;
    const AP4_UI32 sizes[3] = {8, 9, 7};
    for (int i=0; i<3; i++) {
        SampleSpec s;
        s.size = sizes[i]; s.duration = 1; s.cts_offset = -1; s.sync = (i == 0);
        sc.samples.push_back(s);
    }
    ChunkSpec c0 = {1, 0, 16}; // s0 physically last
    ChunkSpec c1 = {2, 0, 0};  // s1, s2
    sc.chunks.push_back(c0);
    sc.chunks.push_back(c1);
    ElstSpec e0 = {2, 0, 1, 0}; // covers cts [0, 2)
    sc.elst.push_back(e0);

    Session session;
    TASK_CHECK(BuildFile(sc, session.built));
    TASK_CHECK(session.Parse() == 0);
    session.MakeStreams(24);

    AP4_EditedSampleReader* reader = NULL;
    TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                              *session.source,
                                              *session.scratch,
                                              24,
                                              reader) == AP4_SUCCESS);
    session.reader = reader;
    TASK_CHECK(reader->GetTimeScale() == 4);

    std::vector<AP4_EditedSampleReader::Packet> packets;
    TASK_CHECK(Drain(reader, packets) == AP4_ERROR_EOS);
    TASK_CHECK(packets.size() == 3);
    if (packets.size() == 3) {
        const Expect e0 = {0, 0, 0, 0, -1, 1, 0, true,  0, 0}; // preroll, negative cts
        const Expect e1 = {0, 0, 1, 1,  0, 1, 0, false, 0, 1};
        const Expect e2 = {0, 0, 2, 2,  1, 1, 0, false, 1, 1};
        TASK_CHECK(CheckPacket(packets[0], e0) == 0);
        TASK_CHECK(CheckPacket(packets[1], e1) == 0);
        TASK_CHECK(CheckPacket(packets[2], e2) == 0);
    }
    TASK_CHECK(!session.scratch->m_RangeViolation);
    return 0;
}

static int
timeline_reverse_overlapping_edits()
{
    // elst v1, all samples sync, strictly decreasing chunk offsets,
    // edits that go backwards in media time and overlap each other
    Scenario sc;
    sc.movie_timescale = 4;
    sc.media_timescale = 4;
    sc.movie_duration  = 5;
    sc.media_duration  = 6;
    sc.elst_version    = 1;
    const AP4_UI32 sizes[6] = {5, 6, 7, 8, 9, 10};
    for (int i=0; i<6; i++) {
        SampleSpec s;
        s.size = sizes[i]; s.duration = 1; s.cts_offset = 0; s.sync = true;
        sc.samples.push_back(s);
    }
    const AP4_UI32 offsets[6] = {40, 34, 27, 19, 10, 0};
    for (int i=0; i<6; i++) {
        ChunkSpec c = {1, 0, offsets[i]};
        sc.chunks.push_back(c);
    }
    ElstSpec e0 = {1, 4, 1, 0}; // [4,5)
    ElstSpec e1 = {2, 0, 1, 0}; // [0,2)
    ElstSpec e2 = {2, 1, 1, 0}; // [1,3) overlaps e1
    sc.elst.push_back(e0);
    sc.elst.push_back(e1);
    sc.elst.push_back(e2);

    Session session;
    TASK_CHECK(BuildFile(sc, session.built));
    TASK_CHECK(session.Parse() == 0);
    session.MakeStreams(18);

    AP4_EditedSampleReader* reader = NULL;
    TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                              *session.source,
                                              *session.scratch,
                                              18,
                                              reader) == AP4_SUCCESS);
    session.reader = reader;
    // s2+s1+s0 spooled together while delivering s0: 7+6+5 = 18
    TASK_CHECK(reader->GetRequiredSpoolBytes() == 18);

    std::vector<AP4_EditedSampleReader::Packet> packets;
    TASK_CHECK(Drain(reader, packets) == AP4_ERROR_EOS);
    TASK_CHECK(packets.size() == 5);
    if (packets.size() == 5) {
        const Expect e0 = {0, 0, 4, 4, 4, 1, 0, true, 0, 1};
        const Expect e1 = {0, 1, 0, 0, 0, 1, 0, true, 1, 1};
        const Expect e2 = {0, 1, 1, 1, 1, 1, 0, true, 2, 1};
        const Expect e3 = {0, 2, 1, 1, 1, 1, 0, true, 3, 1}; // s1 repeated
        const Expect e4 = {0, 2, 2, 2, 2, 1, 0, true, 4, 1};
        TASK_CHECK(CheckPacket(packets[0], e0) == 0);
        TASK_CHECK(CheckPacket(packets[1], e1) == 0);
        TASK_CHECK(CheckPacket(packets[2], e2) == 0);
        TASK_CHECK(CheckPacket(packets[3], e3) == 0);
        TASK_CHECK(CheckPacket(packets[4], e4) == 0);
    }
    // single forward pass over the source, stopping after s0
    TASK_CHECK(CheckSinglePass(session.source,
                               session.built.mdat_payload_offset,
                               session.built.mdat_payload_offset+45) == 0);
    TASK_CHECK(!session.scratch->m_RangeViolation);
    return 0;
}

/*----------------------------------------------------------------------
|   errors group
+---------------------------------------------------------------------*/
static int
errors_rate_fraction_rejected()
{
    // a 1.5x media rate (integer 1, fraction 0x8000) must not be
    // mistaken for plain rate 1
    {
        Scenario sc = MainScenario();
        sc.elst[1].rate          = 1;
        sc.elst[1].rate_fraction = 0x8000;
        Session session;
        TASK_CHECK(BuildFile(sc, session.built));
        TASK_CHECK(session.Parse() == 0);
        session.MakeStreams(1024);
        AP4_EditedSampleReader* reader = NULL;
        TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                                  *session.source,
                                                  *session.scratch,
                                                  1024,
                                                  reader) == AP4_ERROR_INVALID_FORMAT);
        TASK_CHECK(reader == NULL);
        TASK_CHECK(session.source->m_BytesRead == 0);
        TASK_CHECK(session.scratch->m_WriteCalls == 0);

        // the parsed atom exposes the fraction to old-style callers
        AP4_ElstAtom* elst = AP4_DYNAMIC_CAST(AP4_ElstAtom,
                                              session.track->UseTrakAtom()->FindChild("edts/elst"));
        TASK_CHECK(elst != NULL);
        TASK_CHECK(elst->GetEntries().ItemCount() == 4);
        TASK_CHECK(elst->GetEntries()[1].m_MediaRate == 1);
        TASK_CHECK(elst->GetEntries()[1].m_MediaRateFraction == 0x8000);
    }
    // a 2x rate is rejected as well
    {
        Scenario sc = MainScenario();
        sc.elst[1].rate = 2;
        Session session;
        TASK_CHECK(BuildFile(sc, session.built));
        TASK_CHECK(session.Parse() == 0);
        session.MakeStreams(1024);
        AP4_EditedSampleReader* reader = NULL;
        TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                                  *session.source,
                                                  *session.scratch,
                                                  1024,
                                                  reader) == AP4_ERROR_INVALID_FORMAT);
        TASK_CHECK(reader == NULL);
    }
    // the old three-argument entry constructor still works
    {
        AP4_ElstEntry entry(100, 50, 1);
        TASK_CHECK(entry.m_SegmentDuration == 100);
        TASK_CHECK(entry.m_MediaTime == 50);
        TASK_CHECK(entry.m_MediaRate == 1);
        TASK_CHECK(entry.m_MediaRateFraction == 0);
        // and a written atom round-trips the fraction
        AP4_ElstAtom atom;
        AP4_ElstEntry with_fraction(100, 50, 1, 0x8000);
        TASK_CHECK(atom.AddEntry(with_fraction) == AP4_SUCCESS);
        AP4_MemoryByteStream* encoded = new AP4_MemoryByteStream();
        TASK_CHECK(atom.Write(*encoded) == AP4_SUCCESS);
        TASK_CHECK(encoded->Seek(8) == AP4_SUCCESS); // past the atom header
        AP4_ElstAtom* decoded = AP4_ElstAtom::Create((AP4_Size)encoded->GetDataSize(), *encoded);
        TASK_CHECK(decoded != NULL);
        TASK_CHECK(decoded->GetEntries().ItemCount() == 1);
        TASK_CHECK(decoded->GetEntries()[0].m_MediaRateFraction == 0x8000);
        delete decoded;
        encoded->Release();
    }
    return 0;
}

static int
errors_bad_physical_ranges()
{
    // overlapping sample ranges
    {
        Scenario sc;
        sc.movie_duration = 2;
        sc.media_duration = 2;
        SampleSpec s0 = {10, 1, 0, true};
        SampleSpec s1 = {10, 1, 0, true};
        sc.samples.push_back(s0);
        sc.samples.push_back(s1);
        ChunkSpec c0 = {1, 0, 0};
        ChunkSpec c1 = {1, 0, 5}; // overlaps s0
        sc.chunks.push_back(c0);
        sc.chunks.push_back(c1);
        Session session;
        TASK_CHECK(BuildFile(sc, session.built));
        TASK_CHECK(session.Parse() == 0);
        session.MakeStreams(1024);
        AP4_EditedSampleReader* reader = NULL;
        TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                                  *session.source,
                                                  *session.scratch,
                                                  1024,
                                                  reader) == AP4_ERROR_INVALID_FORMAT);
        TASK_CHECK(reader == NULL);
    }
    // a sample range beyond the end of the mdat
    {
        Scenario sc;
        sc.movie_duration = 2;
        sc.media_duration = 2;
        SampleSpec s0 = {10, 1, 0, true};
        SampleSpec s1 = {10, 1, 0, true};
        sc.samples.push_back(s0);
        sc.samples.push_back(s1);
        ChunkSpec c0 = {1, 0, 0};
        ChunkSpec c1 = {1, 0, 1000}; // way past the end
        sc.chunks.push_back(c0);
        sc.chunks.push_back(c1);
        sc.media_size_override = 20; // keep the mdat small
        Session session;
        TASK_CHECK(BuildFile(sc, session.built));
        TASK_CHECK(session.Parse() == 0);
        session.MakeStreams(1024);
        AP4_EditedSampleReader* reader = NULL;
        TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                                  *session.source,
                                                  *session.scratch,
                                                  1024,
                                                  reader) == AP4_ERROR_INVALID_FORMAT);
        TASK_CHECK(reader == NULL);
        TASK_CHECK(session.source->m_BytesRead == 0);
    }
    return 0;
}

static int
errors_time_overflow()
{
    // a media_time so large that mapping it into the common timescale
    // overflows 64 bits (needs the 64-bit elst v1 encoding)
    Scenario sc = MainScenario();
    sc.elst_version = 1;
    sc.elst[1].media_time = (AP4_SI64)0x4000000000000000ULL;

    Session session;
    TASK_CHECK(BuildFile(sc, session.built));
    TASK_CHECK(session.Parse() == 0);
    session.MakeStreams(1024);
    AP4_EditedSampleReader* reader = NULL;
    TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                              *session.source,
                                              *session.scratch,
                                              1024,
                                              reader) == AP4_ERROR_OUT_OF_RANGE);
    TASK_CHECK(reader == NULL);
    TASK_CHECK(session.source->m_BytesRead == 0);
    TASK_CHECK(session.scratch->m_WriteCalls == 0);
    return 0;
}

static int
errors_bad_coverage()
{
    // frames that do not tile the edit interval: a hole between the
    // visible frames
    {
        Scenario sc;
        sc.movie_duration = 6;
        sc.media_duration = 6;
        sc.ctts_version   = 0;
        SampleSpec s0 = {8, 1, 0, true};  // cts 0
        SampleSpec s1 = {8, 1, 4, false}; // cts 5: hole in [1,5)
        sc.samples.push_back(s0);
        sc.samples.push_back(s1);
        ChunkSpec c0 = {2, 0, 0};
        sc.chunks.push_back(c0);
        ElstSpec e0 = {6, 0, 1, 0};
        sc.elst.push_back(e0);
        Session session;
        TASK_CHECK(BuildFile(sc, session.built));
        TASK_CHECK(session.Parse() == 0);
        session.MakeStreams(1024);
        AP4_EditedSampleReader* reader = NULL;
        TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                                  *session.source,
                                                  *session.scratch,
                                                  1024,
                                                  reader) == AP4_ERROR_INVALID_FORMAT);
        TASK_CHECK(reader == NULL);
    }
    // frames that overlap each other in CTS
    {
        Scenario sc;
        sc.movie_duration = 2;
        sc.media_duration = 4;
        sc.ctts_version   = 1;
        SampleSpec s0 = {8, 2, 0, true};  // cts [0,2)
        SampleSpec s1 = {8, 2, -1, false}; // cts [1,3): overlaps
        sc.samples.push_back(s0);
        sc.samples.push_back(s1);
        ChunkSpec c0 = {2, 0, 0};
        sc.chunks.push_back(c0);
        ElstSpec e0 = {2, 0, 1, 0};
        sc.elst.push_back(e0);
        Session session;
        TASK_CHECK(BuildFile(sc, session.built));
        TASK_CHECK(session.Parse() == 0);
        session.MakeStreams(1024);
        AP4_EditedSampleReader* reader = NULL;
        TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                                  *session.source,
                                                  *session.scratch,
                                                  1024,
                                                  reader) == AP4_ERROR_INVALID_FORMAT);
        TASK_CHECK(reader == NULL);
    }
    // a zero-duration edit segment
    {
        Scenario sc = MainScenario();
        sc.elst[1].segment_duration = 0;
        Session session;
        TASK_CHECK(BuildFile(sc, session.built));
        TASK_CHECK(session.Parse() == 0);
        session.MakeStreams(1024);
        AP4_EditedSampleReader* reader = NULL;
        TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                                  *session.source,
                                                  *session.scratch,
                                                  1024,
                                                  reader) == AP4_ERROR_INVALID_FORMAT);
        TASK_CHECK(reader == NULL);
    }
    // a media_time below -1
    {
        Scenario sc = MainScenario();
        sc.elst[1].media_time = -2;
        Session session;
        TASK_CHECK(BuildFile(sc, session.built));
        TASK_CHECK(session.Parse() == 0);
        session.MakeStreams(1024);
        AP4_EditedSampleReader* reader = NULL;
        TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                                  *session.source,
                                                  *session.scratch,
                                                  1024,
                                                  reader) == AP4_ERROR_INVALID_FORMAT);
        TASK_CHECK(reader == NULL);
    }
    return 0;
}

static int
errors_missing_sync()
{
    Scenario sc;
    sc.movie_duration = 2;
    sc.media_duration = 2;
    SampleSpec s0 = {8, 1, 0, false};
    SampleSpec s1 = {8, 1, 0, true}; // only the second sample is a sync
    sc.samples.push_back(s0);
    sc.samples.push_back(s1);
    ChunkSpec c0 = {2, 0, 0};
    sc.chunks.push_back(c0);
    ElstSpec e0 = {2, 0, 1, 0};
    sc.elst.push_back(e0);

    Session session;
    TASK_CHECK(BuildFile(sc, session.built));
    TASK_CHECK(session.Parse() == 0);
    session.MakeStreams(1024);
    AP4_EditedSampleReader* reader = NULL;
    TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                              *session.source,
                                              *session.scratch,
                                              1024,
                                              reader) == AP4_ERROR_INVALID_FORMAT);
    TASK_CHECK(reader == NULL);
    return 0;
}

static int
errors_bad_timescale()
{
    Scenario sc;
    sc.media_timescale = 0; // broken mdhd
    sc.movie_duration  = 1;
    sc.media_duration  = 1;
    SampleSpec s0 = {8, 1, 0, true};
    sc.samples.push_back(s0);
    ChunkSpec c0 = {1, 0, 0};
    sc.chunks.push_back(c0);

    Session session;
    TASK_CHECK(BuildFile(sc, session.built));
    TASK_CHECK(session.Parse() == 0);
    session.MakeStreams(1024);
    AP4_EditedSampleReader* reader = NULL;
    TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                              *session.source,
                                              *session.scratch,
                                              1024,
                                              reader) == AP4_ERROR_INVALID_FORMAT);
    TASK_CHECK(reader == NULL);
    return 0;
}

static int
errors_malformed_sample_table()
{
    Scenario sc = MainScenario();
    sc.omit_stsc = true;

    Session session;
    TASK_CHECK(BuildFile(sc, session.built));
    TASK_CHECK(session.Parse() == 0);
    session.MakeStreams(1024);
    AP4_EditedSampleReader* reader = NULL;
    TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                              *session.source,
                                              *session.scratch,
                                              1024,
                                              reader) == AP4_ERROR_INVALID_FORMAT);
    TASK_CHECK(reader == NULL);
    return 0;
}

static int
errors_short_read_terminal()
{
    BuiltFile built;
    TASK_CHECK(BuildFile(MainScenario(), built));

    for (int variant=0; variant<2; variant++) {
        Session session;
        session.built = built;
        TASK_CHECK(session.Parse() == 0);
        session.MakeStreams(MAIN_SCENARIO_PEAK);
        // the stream pretends to be complete but stalls after 100
        // bytes of media payload
        session.source->m_Truncate = built.mdat_payload_offset+100;
        session.source->m_EosError = (variant == 1);
        AP4_EditedSampleReader* reader = NULL;
        TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                                  *session.source,
                                                  *session.scratch,
                                                  MAIN_SCENARIO_PEAK,
                                                  reader) == AP4_SUCCESS);
        session.reader = reader;

        // the GAP and s0 need at most 95 bytes of media payload,
        // then delivering s1 must spool up to 115
        AP4_EditedSampleReader::Packet packet;
        for (int i=0; i<2; i++) {
            TASK_CHECK(reader->ReadNext(packet) == AP4_SUCCESS);
        }
        AP4_Result first = reader->ReadNext(packet);
        TASK_CHECK(first == AP4_ERROR_READ_FAILED);
        TASK_CHECK(first != AP4_ERROR_EOS);
        // the session is now in a terminal error state
        TASK_CHECK(reader->ReadNext(packet) == first);
        TASK_CHECK(reader->ReadNext(packet) == first);
    }
    return 0;
}

static int
errors_scratch_failure_terminal()
{
    BuiltFile built;
    TASK_CHECK(BuildFile(MainScenario(), built));

    Session session;
    session.built = built;
    TASK_CHECK(session.Parse() == 0);
    session.MakeStreams(MAIN_SCENARIO_PEAK);
    // scratch writes fail after 50 bytes
    session.scratch->m_WriteLimit = 50;
    AP4_EditedSampleReader* reader = NULL;
    TASK_CHECK(AP4_EditedSampleReader::Create(*session.track,
                                              *session.source,
                                              *session.scratch,
                                              MAIN_SCENARIO_PEAK,
                                              reader) == AP4_SUCCESS);
    session.reader = reader;

    AP4_EditedSampleReader::Packet packet;
    TASK_CHECK(reader->ReadNext(packet) == AP4_SUCCESS); // the GAP needs no spool
    AP4_Result first = reader->ReadNext(packet);         // spooling s2+s3 fails
    TASK_CHECK(first == AP4_ERROR_WRITE_FAILED);
    TASK_CHECK(reader->ReadNext(packet) == first);
    TASK_CHECK(reader->ReadNext(packet) == first);
    return 0;
}

static int
errors_legacy_readers_unaffected()
{
    BuiltFile built;
    TASK_CHECK(BuildFile(MainScenario(), built));

    AP4_MemoryByteStream* parse_stream = new AP4_MemoryByteStream(&built.bytes[0],
                                                                  (AP4_Size)built.bytes.size());
    AP4_File* file = new AP4_File(*parse_stream);
    TASK_CHECK(file->GetMovie() != NULL);
    AP4_Track* track = file->GetMovie()->GetTrack(AP4_Track::TYPE_VIDEO);
    TASK_CHECK(track != NULL);

    // the classic random-access sample reader still works
    {
        AP4_Sample sample;
        AP4_DataBuffer data;
        TASK_CHECK(track->ReadSample(2, sample, data) == AP4_SUCCESS);
        TASK_CHECK(sample.GetDts() == 2);
        TASK_CHECK(data.GetDataSize() == 30);
        TASK_CHECK(data.GetData()[0] == SamplePattern(2));
    }

    // the linear reader still delivers every sample in storage order
    {
        AP4_LinearReader linear(*file->GetMovie());
        TASK_CHECK(linear.EnableTrack(1) == AP4_SUCCESS);
        unsigned int count = 0;
        for (;;) {
            AP4_Sample sample;
            AP4_DataBuffer data;
            AP4_UI32     track_id = 0;
            AP4_Result result = linear.ReadNextSample(sample, data, track_id);
            if (result == AP4_ERROR_EOS) break;
            TASK_CHECK(result == AP4_SUCCESS);
            TASK_CHECK(track_id == 1);
            TASK_CHECK(sample.GetDts() == count);
            TASK_CHECK(data.GetDataSize() > 0);
            TASK_CHECK(data.GetData()[0] == SamplePattern(count));
            ++count;
        }
        TASK_CHECK(count == 7);
    }

    // the edit list is still readable through the classic API
    {
        AP4_ElstAtom* elst = AP4_DYNAMIC_CAST(AP4_ElstAtom,
                                              track->UseTrakAtom()->FindChild("edts/elst"));
        TASK_CHECK(elst != NULL);
        TASK_CHECK(elst->GetEntries().ItemCount() == 4);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == -1);
        TASK_CHECK(elst->GetEntries()[1].m_MediaTime == 1);
        TASK_CHECK(elst->GetEntries()[1].m_MediaRate == 1);
        TASK_CHECK(elst->GetEntries()[1].m_MediaRateFraction == 0);
    }

    delete file;
    parse_stream->Release();
    return 0;
}

/*----------------------------------------------------------------------
|   case table
+---------------------------------------------------------------------*/
const struct task_case task_cases[] = {
    {"forward",  "basic_playback",              forward_basic_playback},
    {"forward",  "no_elst_full_track",          forward_no_elst_full_track},
    {"budget",   "peak_exact_and_below",        budget_peak_exact_and_below},
    {"budget",   "fragment_reuse",              budget_fragment_reuse},
    {"budget",   "large_slot_compaction",       budget_large_slot_compaction},
    {"budget",   "all_gap_zero_capacity",       budget_all_gap_zero_capacity},
    {"timeline", "negative_ctts",               timeline_negative_ctts},
    {"timeline", "reverse_overlapping_edits",   timeline_reverse_overlapping_edits},
    {"errors",   "rate_fraction_rejected",      errors_rate_fraction_rejected},
    {"errors",   "bad_physical_ranges",         errors_bad_physical_ranges},
    {"errors",   "time_overflow",               errors_time_overflow},
    {"errors",   "bad_coverage",                errors_bad_coverage},
    {"errors",   "missing_sync",                errors_missing_sync},
    {"errors",   "bad_timescale",               errors_bad_timescale},
    {"errors",   "malformed_sample_table",      errors_malformed_sample_table},
    {"errors",   "short_read_terminal",         errors_short_read_terminal},
    {"errors",   "scratch_failure_terminal",    errors_scratch_failure_terminal},
    {"errors",   "legacy_readers_unaffected",   errors_legacy_readers_unaffected}
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
