/*****************************************************************
|
|    EditedPreview output verifier.
|
|    Re-opens the input MP4 and independently recomputes the complete
|    expected packet sequence (edit list evaluation, visible-frame
|    selection, display windows in the common timescale, payloads
|    read through the classic seeking sample API), then compares it
|    against the byte stream exported by the editedpreview app.
|
|    usage: preview_verify <input.mp4> <preview.bin>
|
+---------------------------------------------------------------------*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Ap4.h"

/*----------------------------------------------------------------------
|   expected packet model
+---------------------------------------------------------------------*/
struct ExpectedPacket {
    int          kind;      // 0 = media, 1 = gap
    AP4_UI32     edit_index;
    AP4_Ordinal  sample;
    AP4_UI64     dts;
    AP4_SI64     cts;
    AP4_UI32     duration;
    AP4_Ordinal  description_index;
    bool         sync;
    AP4_UI64     movie_begin;
    AP4_UI64     movie_duration;
    AP4_DataBuffer payload;
};

static AP4_UI64
Gcd64(AP4_UI64 a, AP4_UI64 b)
{
    while (b) {
        AP4_UI64 t = a%b;
        a = b;
        b = t;
    }
    return a;
}

static int
Fail(const char* what, unsigned long long detail)
{
    fprintf(stderr, "VERIFY FAIL: %s (%llu)\n", what, detail);
    return 1;
}

/*----------------------------------------------------------------------
|   main
+---------------------------------------------------------------------*/
int
main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: preview_verify <input.mp4> <preview.bin>\n");
        return 2;
    }

    // parse the input
    AP4_ByteStream* input = NULL;
    AP4_Result result = AP4_FileByteStream::Create(argv[1],
                                                   AP4_FileByteStream::STREAM_MODE_READ,
                                                   input);
    if (AP4_FAILED(result)) return Fail("cannot open input", result);
    AP4_File file(*input);
    AP4_Movie* movie = file.GetMovie();
    if (!movie) return Fail("no movie", 0);
    AP4_Track* track = movie->GetTrack(AP4_Track::TYPE_VIDEO);
    if (!track) return Fail("no video track", 0);

    AP4_UI64 movie_ts = track->GetMovieTimeScale();
    AP4_UI64 media_ts = track->GetMediaTimeScale();
    if (!movie_ts || !media_ts) return Fail("bad timescale", 0);
    AP4_UI64 common = (movie_ts/Gcd64(movie_ts, media_ts))*media_ts;
    AP4_UI64 per_c  = common/media_ts;

    // gather the sample metadata
    AP4_Cardinal sample_count = track->GetSampleCount();
    std::vector<AP4_UI64> dts(sample_count);
    std::vector<AP4_SI64> cts(sample_count);
    std::vector<AP4_UI32> dur(sample_count);
    std::vector<AP4_UI32> desc(sample_count);
    std::vector<bool>     sync(sample_count);
    AP4_UI64 media_total = 0;
    for (AP4_Ordinal i=0; i<sample_count; i++) {
        AP4_Sample sample;
        result = track->GetSample(i, sample);
        if (AP4_FAILED(result)) return Fail("GetSample", i);
        dts[i]  = sample.GetDts();
        cts[i]  = (AP4_SI64)sample.GetDts()+(AP4_SI32)sample.GetCtsDelta();
        dur[i]  = sample.GetDuration();
        desc[i] = sample.GetDescriptionIndex();
        sync[i] = sample.IsSync();
        media_total += dur[i];
    }

    // build the edit list
    struct Edit { bool gap; AP4_UI64 begin_c; AP4_UI64 span_c; };
    std::vector<Edit> edits;
    AP4_ElstAtom* elst = AP4_DYNAMIC_CAST(AP4_ElstAtom,
                                          track->UseTrakAtom()->FindChild("edts/elst"));
    if (elst && elst->GetEntries().ItemCount()) {
        AP4_Array<AP4_ElstEntry>& entries = elst->GetEntries();
        for (AP4_Ordinal i=0; i<entries.ItemCount(); i++) {
            const AP4_ElstEntry& e = entries[i];
            Edit edit;
            edit.span_c = e.m_SegmentDuration*(common/movie_ts);
            if (e.m_MediaTime == -1) {
                edit.gap     = true;
                edit.begin_c = 0;
            } else {
                if (e.m_MediaRate != 1 || e.m_MediaRateFraction != 0) {
                    return Fail("unsupported rate", i);
                }
                edit.gap     = false;
                edit.begin_c = (AP4_UI64)e.m_MediaTime*per_c;
            }
            edits.push_back(edit);
        }
    } else {
        Edit edit;
        edit.gap     = false;
        edit.begin_c = 0;
        edit.span_c  = media_total*per_c;
        edits.push_back(edit);
    }

    // compute the expected packet sequence
    std::vector<ExpectedPacket> expected;
    AP4_UI64 cursor = 0;
    for (size_t e=0; e<edits.size(); e++) {
        const Edit& edit = edits[e];
        if (edit.gap) {
            ExpectedPacket p;
            p.kind              = 1;
            p.edit_index        = (AP4_UI32)e;
            p.sample            = (AP4_Ordinal)-1;
            p.dts               = 0;
            p.cts               = 0;
            p.duration          = 0;
            p.description_index = 0;
            p.sync              = false;
            p.movie_begin       = cursor;
            p.movie_duration    = edit.span_c;
            expected.push_back(p);
            cursor += edit.span_c;
            continue;
        }
        AP4_SI64 begin = (AP4_SI64)edit.begin_c;
        AP4_SI64 end   = begin+(AP4_SI64)edit.span_c;
        // visible frames, in presentation order
        std::vector<AP4_Ordinal> visible;
        for (AP4_Ordinal i=0; i<sample_count; i++) {
            AP4_SI64 f0 = cts[i]*(AP4_SI64)per_c;
            AP4_SI64 f1 = f0+(AP4_SI64)(dur[i]*per_c);
            if (f0 < end && f1 > begin) visible.push_back(i);
        }
        if (visible.empty()) return Fail("uncovered edit", e);
        // check tiling in presentation order
        for (size_t i=1; i<visible.size(); i++) {
            for (size_t j=i; j>0 && cts[visible[j]] < cts[visible[j-1]]; j--) {
                AP4_Ordinal tmp = visible[j-1];
                visible[j-1]    = visible[j];
                visible[j]      = tmp;
            }
        }
        AP4_SI64 chain = begin;
        for (size_t i=0; i<visible.size(); i++) {
            AP4_Ordinal f  = visible[i];
            AP4_SI64    f0 = cts[f]*(AP4_SI64)per_c;
            AP4_SI64    f1 = f0+(AP4_SI64)(dur[f]*per_c);
            AP4_SI64    cs = f0 > begin ? f0 : begin;
            if (cs != chain) return Fail("edit not tiled", e);
            chain = f1 < end ? f1 : end;
        }
        if (chain != end) return Fail("edit not covered", e);
        // delivery range in decode order
        AP4_Ordinal first = sample_count, last = 0;
        for (size_t i=0; i<visible.size(); i++) {
            if (visible[i] < first) first = visible[i];
            if (visible[i] > last)  last  = visible[i];
        }
        AP4_Ordinal from = first;
        while (from != 0 && !sync[from]) --from;
        if (!sync[from]) return Fail("no sync", e);
        for (AP4_Ordinal i=from; i<=last; i++) {
            ExpectedPacket p;
            p.kind              = 0;
            p.edit_index        = (AP4_UI32)e;
            p.sample            = i;
            p.dts               = dts[i];
            p.cts               = cts[i];
            p.duration          = dur[i];
            p.description_index = desc[i];
            p.sync              = sync[i];
            p.movie_begin       = 0;
            p.movie_duration    = 0;
            bool is_visible = false;
            for (size_t v=0; v<visible.size(); v++) {
                if (visible[v] == i) is_visible = true;
            }
            if (is_visible) {
                AP4_SI64 f0 = cts[i]*(AP4_SI64)per_c;
                AP4_SI64 f1 = f0+(AP4_SI64)(dur[i]*per_c);
                AP4_SI64 cs = f0 > begin ? f0 : begin;
                AP4_SI64 ce = f1 < end   ? f1 : end;
                p.movie_begin    = cursor+(AP4_UI64)(cs-begin);
                p.movie_duration = (AP4_UI64)(ce-cs);
            }
            // the expected payload comes from the classic seeking API
            AP4_Sample sample;
            result = track->ReadSample(i, sample, p.payload);
            if (AP4_FAILED(result)) return Fail("ReadSample", i);
            expected.push_back(p);
        }
        cursor += edit.span_c;
    }

    // read the exported stream
    AP4_ByteStream* exported = NULL;
    result = AP4_FileByteStream::Create(argv[2],
                                        AP4_FileByteStream::STREAM_MODE_READ,
                                        exported);
    if (AP4_FAILED(result)) return Fail("cannot open preview", result);

    char magic[4] = {0,0,0,0};
    AP4_UI32 version = 0;
    AP4_UI64 time_scale = 0;
    AP4_UI64 required = 0;
    if (AP4_FAILED(exported->Read(magic, 4))) return Fail("short header", 0);
    if (memcmp(magic, "EPRV", 4))            return Fail("bad magic", 0);
    if (AP4_FAILED(exported->ReadUI32(version)))  return Fail("short header", 1);
    if (version != 1)                            return Fail("bad version", version);
    if (AP4_FAILED(exported->ReadUI64(time_scale))) return Fail("short header", 2);
    if (AP4_FAILED(exported->ReadUI64(required)))   return Fail("short header", 3);
    if (time_scale != common) return Fail("timescale mismatch",
                                          (unsigned long long)time_scale);

    for (size_t i=0; i<expected.size(); i++) {
        const ExpectedPacket& xp = expected[i];
        AP4_UI32 kind = 0, edit_index = 0, sample_index = 0, duration = 0;
        AP4_UI32 description_index = 0, sync_flag = 0, payload_size = 0;
        AP4_UI64 dts_v = 0, cts_v = 0, begin_v = 0, mdur_v = 0;
        if (AP4_FAILED(exported->ReadUI32(kind)))        return Fail("truncated packet", i);
        if (AP4_FAILED(exported->ReadUI32(edit_index)))  return Fail("truncated packet", i);
        if (AP4_FAILED(exported->ReadUI32(sample_index))) return Fail("truncated packet", i);
        if (AP4_FAILED(exported->ReadUI64(dts_v)))       return Fail("truncated packet", i);
        if (AP4_FAILED(exported->ReadUI64(cts_v)))       return Fail("truncated packet", i);
        if (AP4_FAILED(exported->ReadUI32(duration)))    return Fail("truncated packet", i);
        if (AP4_FAILED(exported->ReadUI32(description_index))) return Fail("truncated packet", i);
        if (AP4_FAILED(exported->ReadUI32(sync_flag)))   return Fail("truncated packet", i);
        if (AP4_FAILED(exported->ReadUI64(begin_v)))     return Fail("truncated packet", i);
        if (AP4_FAILED(exported->ReadUI64(mdur_v)))      return Fail("truncated packet", i);
        if (AP4_FAILED(exported->ReadUI32(payload_size))) return Fail("truncated packet", i);

        if ((int)kind != xp.kind)                 return Fail("kind", i);
        if (edit_index != xp.edit_index)          return Fail("edit_index", i);
        if (sample_index != (AP4_UI32)xp.sample)  return Fail("sample_index", i);
        if (dts_v != xp.dts)                      return Fail("dts", i);
        if ((AP4_SI64)cts_v != xp.cts)            return Fail("cts", i);
        if (duration != xp.duration)              return Fail("duration", i);
        if (description_index != xp.description_index) return Fail("description", i);
        if ((sync_flag?true:false) != xp.sync)    return Fail("sync", i);
        if (begin_v != xp.movie_begin)            return Fail("movie_begin", i);
        if (mdur_v != xp.movie_duration)          return Fail("movie_duration", i);
        if (payload_size != xp.payload.GetDataSize()) return Fail("payload size", i);
        if (payload_size) {
            AP4_DataBuffer got(payload_size);
            got.SetDataSize(payload_size);
            if (AP4_FAILED(exported->Read(got.UseData(), payload_size))) {
                return Fail("truncated payload", i);
            }
            if (memcmp(got.GetData(), xp.payload.GetData(), payload_size)) {
                return Fail("payload bytes", i);
            }
        }
    }

    // the stream must end exactly after the last packet
    AP4_UI08 extra = 0;
    AP4_Size  extra_read = 0;
    result = exported->ReadPartial(&extra, 1, extra_read);
    if (AP4_SUCCEEDED(result) && extra_read != 0) {
        return Fail("trailing records", expected.size());
    }

    printf("PREVIEW_VERIFY_OK packets=%llu timescale=%llu\n",
           (unsigned long long)expected.size(),
           (unsigned long long)common);

    exported->Release();
    input->Release();
    return 0;
}
