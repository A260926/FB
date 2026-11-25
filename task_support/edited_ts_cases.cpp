// Self-contained timeline, PCR, failure-injection and compatibility cases.
#include "check.h"

#include "Ap4.h"
#include "Ap4EditedTs.h"
#include "Ap4Mpeg2Ts.h"
#include "ts_check.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

typedef std::vector<unsigned char> Bytes;

static void Put32(Bytes& b, uint32_t x) {
    for (int i=3; i>=0; --i) b.push_back((unsigned char)((x>>(i*8))&255));
}
static void Put16(Bytes& b, uint16_t x) {
    b.push_back((unsigned char)(x>>8));
    b.push_back((unsigned char)(x&255));
}
static void Put64(Bytes& b, uint64_t x) {
    for (int i=7; i>=0; --i) b.push_back((unsigned char)((x>>(i*8))&255));
}
static Bytes Words(std::initializer_list<uint32_t> xs) {
    Bytes b;
    for (uint32_t x : xs) Put32(b, x);
    return b;
}
static void Append(Bytes& b, const Bytes& x) { b.insert(b.end(), x.begin(), x.end()); }
static Bytes Cat(std::initializer_list<Bytes> xs) {
    Bytes b;
    for (const Bytes& x : xs) Append(b, x);
    return b;
}
static Bytes Box(const char* type, const Bytes& payload) {
    Bytes o;
    Put32(o, (uint32_t)(payload.size()+8));
    o.insert(o.end(), type, type+4);
    Append(o, payload);
    return o;
}
static Bytes FullBox(const char* type, const Bytes& payload, uint32_t version_flags=0) {
    Bytes o = Words({version_flags});
    Append(o, payload);
    return Box(type, o);
}
static Bytes MatrixBytes() {
    return Words({65536,0,0,0,65536,0,0,0,0x40000000});
}

struct EditSpec {
    uint64_t duration;
    int64_t  media_time;
    uint16_t rate_integer;
    uint16_t rate_fraction;
    EditSpec(uint64_t d=0, int64_t m=-1, uint16_t ri=1, uint16_t rf=0) :
        duration(d), media_time(m), rate_integer(ri), rate_fraction(rf) {}
};

struct SampleSpec {
    uint32_t duration;
    uint32_t cts_delta;
    bool     sync;
    uint8_t  marker;
    uint32_t description;
    SampleSpec(uint32_t d=1, uint32_t c=0, bool s=false, uint8_t m=0, uint32_t desc=0) :
        duration(d), cts_delta(c), sync(s), marker(m), description(desc) {}
};

struct TrackSpec {
    bool                    video;
    uint32_t                timescale;
    uint64_t                mdhd_duration;
    uint64_t                tkhd_duration;
    std::vector<SampleSpec> samples;
    std::vector<uint8_t>    parameter_markers;
    std::vector<EditSpec>   edits;
    bool                    external_ref;
    uint16_t                channels;
    uint32_t                sample_rate;
    TrackSpec(bool v=true, uint32_t ts=1000) :
        video(v), timescale(ts), mdhd_duration(0), tkhd_duration(1000),
        external_ref(false), channels(2), sample_rate(44100) {}
};

struct MovieSpec {
    uint32_t    timescale;
    uint64_t    duration;
    bool        has_audio;
    bool        duplicate_video;
    TrackSpec   video;
    TrackSpec   audio;
    MovieSpec() : timescale(1000), duration(1000), has_audio(true),
                  duplicate_video(false), video(true,700), audio(false,44100) {}
};

struct BuiltMovie {
    Bytes    bytes;
    uint64_t first_video_offset;
    uint64_t first_audio_offset;
};

static Bytes AvcConfig(uint8_t marker) {
    Bytes sps;
    sps.push_back(0x67); sps.push_back(0x42); sps.push_back(marker); sps.push_back(0x1e);
    Bytes pps;
    pps.push_back(0x68); pps.push_back(marker); pps.push_back(0x22);

    Bytes c;
    c.push_back(1); c.push_back(0x42); c.push_back(0); c.push_back(0x1e);
    c.push_back(0xff); c.push_back(0xe1);
    Put16(c, (uint16_t)sps.size()); Append(c, sps);
    c.push_back(1);
    Put16(c, (uint16_t)pps.size()); Append(c, pps);
    return c;
}

static Bytes VideoEntry(uint8_t parameter_marker) {
    Bytes entry(6, 0);
    Put16(entry, 1); // data reference index
    entry.resize(78, 0);
    entry[7] = 1;
    entry[25] = 16;
    entry[27] = 16;
    entry[29] = 72;
    entry[33] = 72;
    entry[41] = 1;
    entry[75] = 24;
    entry[76] = 255;
    entry[77] = 255;
    Append(entry, Box("avcC", AvcConfig(parameter_marker)));
    return Box("avc1", entry);
}

static Bytes EsdsBox() {
    Bytes dsi;
    dsi.push_back(0x05); dsi.push_back(0x02); dsi.push_back(0x12); dsi.push_back(0x10);

    Bytes decoder;
    decoder.push_back(0x40); // audio object type indication
    decoder.push_back(0x15); // audio stream type
    decoder.push_back(0); decoder.push_back(0); decoder.push_back(0); // buffer size
    Put32(decoder, 0x00017700);
    Put32(decoder, 0x00017700);
    Append(decoder, dsi);

    Bytes decoder_config;
    decoder_config.push_back(0x04);
    decoder_config.push_back((uint8_t)decoder.size());
    Append(decoder_config, decoder);

    Bytes sl;
    sl.push_back(0x06); sl.push_back(0x01); sl.push_back(0x02);

    Bytes es_payload;
    Put16(es_payload, 0); // ES_ID
    es_payload.push_back(0); // flags
    Append(es_payload, decoder_config);
    Append(es_payload, sl);

    Bytes es;
    es.push_back(0x03);
    es.push_back((uint8_t)es_payload.size());
    Append(es, es_payload);
    return FullBox("esds", es);
}

static Bytes AudioEntry(const TrackSpec& track) {
    Bytes entry(6, 0);
    Put16(entry, 1);
    Put16(entry, 0); // QT version
    Put16(entry, 0); // revision
    Put32(entry, 0); // vendor
    Put16(entry, track.channels);
    Put16(entry, 16); // sample size
    Put16(entry, 0); // compression id
    Put16(entry, 0); // packet size
    Put32(entry, track.sample_rate<<16);
    Append(entry, EsdsBox());
    return Box("mp4a", entry);
}

static Bytes EditListBox(const TrackSpec& track) {
    if (track.edits.empty()) return Bytes();
    bool wide = false;
    for (size_t i=0; i<track.edits.size(); ++i) {
        if (track.edits[i].duration > 0xffffffffULL) wide = true;
    }
    Bytes payload = Words({(uint32_t)track.edits.size()});
    for (size_t i=0; i<track.edits.size(); ++i) {
        const EditSpec& e = track.edits[i];
        if (wide) {
            Put64(payload, e.duration);
            Put64(payload, (uint64_t)e.media_time);
        } else {
            Put32(payload, (uint32_t)e.duration);
            Put32(payload, (uint32_t)(int32_t)e.media_time);
        }
        Put16(payload, e.rate_integer);
        Put16(payload, e.rate_fraction);
    }
    Bytes elst = FullBox("elst", payload, wide ? 0x01000000 : 0);
    Bytes edts;
    Append(edts, elst);
    return Box("edts", edts);
}

static Bytes BuildTrack(const TrackSpec& track,
                        uint32_t track_id,
                        uint32_t data_start,
                        Bytes& mdat,
                        uint64_t& first_offset) {
    first_offset = 0;
    std::vector<uint32_t> sizes;
    std::vector<uint32_t> offsets;
    uint64_t sample_duration_total = 0;

    for (size_t i=0; i<track.samples.size(); ++i) {
        const SampleSpec& s = track.samples[i];
        Bytes payload;
        if (track.video) {
            Bytes nalu;
            nalu.push_back(s.sync ? 0x65 : 0x41);
            nalu.push_back(0x88);
            nalu.push_back(s.marker);
            nalu.push_back((uint8_t)(i+1));
            Put32(payload, (uint32_t)nalu.size());
            Append(payload, nalu);
        } else {
            payload.push_back(0xaa);
            payload.push_back(s.marker);
            payload.push_back((uint8_t)(i+1));
            payload.push_back(0x55);
        }

        uint64_t absolute = data_start + mdat.size();
        if (i == 0) first_offset = absolute;
        offsets.push_back((uint32_t)absolute);
        sizes.push_back((uint32_t)payload.size());
        Append(mdat, payload);
        sample_duration_total += s.duration;
    }

    uint32_t description_count = track.video ?
        (uint32_t)std::max<size_t>(1, track.parameter_markers.size()) : 1;
    Bytes stsd = Words({description_count});
    if (track.video) {
        for (uint32_t d=0; d<description_count; ++d) {
            uint8_t marker = track.parameter_markers.empty() ? 0x42 : track.parameter_markers[d];
            Append(stsd, VideoEntry(marker));
        }
    } else {
        Append(stsd, AudioEntry(track));
    }

    Bytes stts = Words({(uint32_t)track.samples.size()});
    for (size_t i=0; i<track.samples.size(); ++i) {
        Put32(stts, 1);
        Put32(stts, track.samples[i].duration);
    }

    Bytes ctts = Words({(uint32_t)track.samples.size()});
    for (size_t i=0; i<track.samples.size(); ++i) {
        Put32(ctts, 1);
        Put32(ctts, track.samples[i].cts_delta);
    }

    Bytes stsc = Words({(uint32_t)track.samples.size()});
    for (size_t i=0; i<track.samples.size(); ++i) {
        Put32(stsc, (uint32_t)(i+1));
        Put32(stsc, 1);
        Put32(stsc, track.samples[i].description+1);
    }

    Bytes stsz = Words({0, (uint32_t)track.samples.size()});
    for (uint32_t size : sizes) Put32(stsz, size);

    Bytes stco = Words({(uint32_t)offsets.size()});
    for (uint32_t offset : offsets) Put32(stco, offset);

    Bytes stbl = Cat({
        FullBox("stsd", stsd),
        FullBox("stts", stts),
        FullBox("ctts", ctts),
        FullBox("stsc", stsc),
        FullBox("stsz", stsz),
        FullBox("stco", stco)
    });

    if (track.video) {
        Bytes stss_entries;
        uint32_t sync_count = 0;
        for (size_t i=0; i<track.samples.size(); ++i) {
            if (track.samples[i].sync) ++sync_count;
        }
        Bytes stss = Words({sync_count});
        for (size_t i=0; i<track.samples.size(); ++i) {
            if (track.samples[i].sync) Put32(stss, (uint32_t)(i+1));
        }
        (void)stss_entries;
        Append(stbl, FullBox("stss", stss));
    }

    Bytes url_payload;
    uint32_t url_flags = 1;
    if (track.external_ref) {
        url_flags = 0;
        const char* url = "http://example.invalid/media";
        url_payload.insert(url_payload.end(), url, url+std::strlen(url));
        url_payload.push_back(0);
    }
    Bytes dref = Words({1});
    Append(dref, FullBox("url ", url_payload, url_flags));
    Bytes dinf = Box("dinf", FullBox("dref", dref));

    Bytes minf;
    if (track.video) {
        Append(minf, FullBox("vmhd", Bytes(8, 0), 1));
    } else {
        Append(minf, FullBox("smhd", Bytes(4, 0), 0));
    }
    Append(minf, dinf);
    Append(minf, Box("stbl", stbl));
    Bytes minf_box = Box("minf", minf);

    uint64_t mdhd_duration = track.mdhd_duration ? track.mdhd_duration : sample_duration_total;
    Bytes mdhd = Words({0, 0, track.timescale, (uint32_t)mdhd_duration});
    Put16(mdhd, 0x55c4);
    Put16(mdhd, 0);

    Bytes hdlr = Words({0});
    const char* handler = track.video ? "vide" : "soun";
    hdlr.insert(hdlr.end(), handler, handler+4);
    hdlr.resize(hdlr.size()+13, 0);

    Bytes tkhd = Words({0, 0, track_id, 0, (uint32_t)track.tkhd_duration});
    Append(tkhd, Words({0, 0})); // reserved
    Put16(tkhd, 0); // layer
    Put16(tkhd, 0); // alternate group
    Put16(tkhd, track.video ? 0 : 0x0100); // volume
    Put16(tkhd, 0); // reserved
    Append(tkhd, MatrixBytes());
    Put32(tkhd, track.video ? (16u<<16) : 0);
    Put32(tkhd, track.video ? (16u<<16) : 0);

    Bytes trak;
    Append(trak, FullBox("tkhd", tkhd, 3));
    Bytes edts = EditListBox(track);
    if (!edts.empty()) Append(trak, edts);
    Append(trak, Box("mdia", Cat({FullBox("mdhd", mdhd),
                                  FullBox("hdlr", hdlr),
                                  minf_box})));
    return Box("trak", trak);
}

static BuiltMovie BuildMovie(const MovieSpec& spec) {
    Bytes ftyp = Box("ftyp", Bytes{'i','s','o','m',0,0,0,0,'i','s','o','m','q','t',' ',' '});
    uint32_t data_start = (uint32_t)ftyp.size()+8;

    Bytes mdat;
    BuiltMovie built;
    built.first_video_offset = 0;
    built.first_audio_offset = 0;

    Bytes video_trak = BuildTrack(spec.video, 1, data_start, mdat, built.first_video_offset);
    Bytes audio_trak;
    if (spec.has_audio) {
        audio_trak = BuildTrack(spec.audio, 2, data_start, mdat, built.first_audio_offset);
    }
    Bytes duplicate_trak;
    if (spec.duplicate_video) {
        uint64_t ignored = 0;
        duplicate_trak = BuildTrack(spec.video, 3, data_start, mdat, ignored);
    }

    Bytes mvhd = Words({0, 0, spec.timescale, (uint32_t)spec.duration,
                        65536, 0x01000000, 0, 0});
    Append(mvhd, MatrixBytes());
    mvhd.resize(mvhd.size()+24, 0);
    Put32(mvhd, 4);

    Bytes moov_payload;
    Append(moov_payload, FullBox("mvhd", mvhd));
    Append(moov_payload, video_trak);
    if (spec.has_audio) Append(moov_payload, audio_trak);
    if (spec.duplicate_video) Append(moov_payload, duplicate_trak);
    Bytes moov = Box("moov", moov_payload);
    Bytes mdat_box = Box("mdat", mdat);

    built.bytes = Cat({ftyp, mdat_box, moov});
    return built;
}

/*----------------------------- TS parsing -----------------------------*/

struct PesInfo {
    unsigned                pid;
    unsigned char           stream_id;
    bool                    has_dts;
    uint64_t                pts;
    uint64_t                dts;
    Bytes                   payload;
};

struct TsEvent {
    bool pcr;
    unsigned pid;
    uint64_t clock;
    size_t pes_index;
};

struct TsInfo {
    bool ok;
    std::string error;
    unsigned pat;
    unsigned pmt;
    unsigned pcr_pid_from_pmt;
    std::vector<uint64_t> pcrs;
    std::vector<PesInfo> pes;
    std::vector<PesInfo> pes_order;
    std::vector<TsEvent> events;
    std::map<unsigned, size_t> current_pes_index;

    TsInfo() : ok(true), pat(0), pmt(0), pcr_pid_from_pmt(0) {}
    void Fail(const char* m) { ok=false; error=m; }
};

static uint64_t ParsePtsField(const unsigned char* p) {
    return (uint64_t(p[0]&14)<<29) | (uint64_t(p[1])<<22) |
           (uint64_t(p[2]&254)<<14) | (uint64_t(p[3])<<7) | (p[4]>>1);
}
static uint64_t ParsePcr27(const unsigned char* p) {
    return ((uint64_t(p[0])<<25)|(uint64_t(p[1])<<17)|(uint64_t(p[2])<<9)|
            (uint64_t(p[3])<<1)|(p[4]>>7))*300 +
           (((p[4]&1)<<8)+p[5]);
}

static TsInfo ParseTs(const unsigned char* data, size_t size) {
    TsInfo ts;
    if (size == 0 || size%188 != 0) { ts.Fail("size"); return ts; }

    std::map<unsigned, int> last_payload_cc;
    std::map<unsigned, bool> seen_payload;

    for (size_t base=0; base<size; base+=188) {
        const unsigned char* p = data+base;
        if (p[0] != 0x47) { ts.Fail("sync"); return ts; }
        unsigned pid = ((p[1]&31)<<8)|p[2];
        bool pusi = (p[1]&64) != 0;
        unsigned afc = (p[3]>>4)&3;
        unsigned cc = p[3]&15;
        if (afc == 0) { ts.Fail("afc0"); return ts; }

        size_t offset = 4;
        bool has_adaptation = (afc&2) != 0;
        bool has_payload = (afc&1) != 0;
        unsigned adaptation_length = 0;
        if (has_adaptation) {
            adaptation_length = p[4];
            if (adaptation_length > 183 || 5u+adaptation_length > 188) {
                ts.Fail("adaptation length"); return ts;
            }
            offset = 5+adaptation_length;
        }
        size_t payload_end = 188;
        size_t payload_length = has_payload ? payload_end-offset : 0;
        if (has_payload && offset >= 188) { ts.Fail("missing payload"); return ts; }

        int expected_cc;
        if (payload_length > 0) {
            expected_cc = seen_payload[pid] ? (last_payload_cc[pid]+1)%16 : 0;
            if ((int)cc != expected_cc) { ts.Fail("payload cc"); return ts; }
            seen_payload[pid] = true;
            last_payload_cc[pid] = cc;
        } else if (afc == 2) {
            expected_cc = seen_payload[pid] ? last_payload_cc[pid] : 0;
            if ((int)cc != expected_cc) { ts.Fail("pcr cc"); return ts; }
        } else {
            ts.Fail("no payload but afc not2"); return ts;
        }

        if (pid == 0) {
            ++ts.pat;
            continue;
        }
        if (pid == 0x100) {
            ++ts.pmt;
            if (has_payload && pusi && payload_length > 12) {
                const unsigned char* section = p+offset+1; // pointer field
                ts.pcr_pid_from_pmt = ((section[8]&0x1f)<<8)|section[9];
            }
            continue;
        }

        if (afc == 2) {
            if (pusi || adaptation_length != 183 || p[5] != 0x10) {
                ts.Fail("pcr packet shape"); return ts;
            }
            for (int i=12; i<188; ++i) {
                if (p[i] != 0xff) { ts.Fail("pcr stuffing"); return ts; }
            }
            uint64_t pcr27 = ParsePcr27(p+6);
            ts.pcrs.push_back(pcr27);
            ts.events.push_back(TsEvent{true, pid, pcr27, 0});
            continue;
        }

        if (!has_payload) { ts.Fail("non-pcr no payload"); return ts; }
        const unsigned char* payload = p+offset;

        if (pusi) {
            if (payload_length < 14 || payload[0] || payload[1] ||
                payload[2] != 1 || (payload[3] != 0xe0 && payload[3] != 0xc0)) {
                ts.Fail("pes header"); return ts;
            }
            unsigned header_length = payload[8];
            size_t es_start = 9+header_length;
            if (es_start > payload_length) { ts.Fail("pes header length"); return ts; }
            unsigned pts_dts_flags = (payload[7]>>6)&3;
            PesInfo pes;
            pes.pid = pid;
            pes.stream_id = payload[3];
            pes.has_dts = (pts_dts_flags == 3);
            pes.pts = ParsePtsField(payload+9);
            pes.dts = pes.pts;
            if (pes.has_dts) pes.dts = ParsePtsField(payload+14);
            pes.payload.assign(payload+es_start, payload+payload_length);
            ts.pes_order.push_back(pes);
            size_t pes_index = ts.pes_order.size()-1;
            ts.current_pes_index[pid] = pes_index;
            ts.events.push_back(TsEvent{false, pid, pes.has_dts ? pes.dts : pes.pts, pes_index});
        } else {
            std::map<unsigned, size_t>::iterator it = ts.current_pes_index.find(pid);
            if (it == ts.current_pes_index.end()) { ts.Fail("pes continuation"); return ts; }
            PesInfo& pes = ts.pes_order[it->second];
            pes.payload.insert(pes.payload.end(), payload, payload+payload_length);
        }
    }
    ts.pes = ts.pes_order;
    return ts;
}

static bool ContainsBytes(const Bytes& haystack, const unsigned char* needle, size_t n) {
    if (n == 0 || haystack.size() < n) return false;
    for (size_t i=0; i+n<=haystack.size(); ++i) {
        if (std::memcmp(haystack.data()+i, needle, n) == 0) return true;
    }
    return false;
}

/*----------------------------- fault streams -----------------------------*/

class FaultSink : public AP4_ByteStream {
public:
    enum Mode {
        NONE, FAIL_PAT, FAIL_PMT, FAIL_PCR, FAIL_VIDEO_MEDIA, FAIL_AUDIO_MEDIA, FAIL_CALL
    };
    Bytes data;
    Mode mode;
    AP4_Result fault_code;
    int fail_call;
    int calls;
    FaultSink(Mode m=NONE, AP4_Result code=-7281, int call=0) :
        mode(m), fault_code(code), fail_call(call), calls(0) {}

    virtual AP4_Result ReadPartial(void*, AP4_Size, AP4_Size&) {
        return AP4_ERROR_NOT_SUPPORTED;
    }
    virtual AP4_Result WritePartial(const void* buffer, AP4_Size bytes, AP4_Size& written) {
        ++calls;
        const unsigned char* p = (const unsigned char*)buffer;
        bool fail = false;
        if (mode == FAIL_CALL && calls == fail_call) fail = true;
        if (bytes >= 4 && p[0] == 0x47) {
            unsigned pid = ((p[1]&31)<<8)|p[2];
            unsigned afc = (p[3]>>4)&3;
            if (mode == FAIL_PAT && pid == 0) fail = true;
            if (mode == FAIL_PMT && pid == 0x100) fail = true;
            if (mode == FAIL_PCR && pid == 0x102 && afc == 2 && bytes == 188) fail = true;
            if (mode == FAIL_VIDEO_MEDIA && pid == 0x102 && (afc&1)) fail = true;
            if (mode == FAIL_AUDIO_MEDIA && pid == 0x101 && (afc&1)) fail = true;
        }
        if (fail) { written = 0; return fault_code; }
        data.insert(data.end(), p, p+bytes);
        written = bytes;
        return AP4_SUCCESS;
    }
    virtual AP4_Result Seek(AP4_Position) { return AP4_SUCCESS; }
    virtual AP4_Result Tell(AP4_Position& position) { position = data.size(); return AP4_SUCCESS; }
    virtual AP4_Result GetSize(AP4_LargeSize& size) { size = data.size(); return AP4_SUCCESS; }
    virtual void AddReference() {}
    virtual void Release() {}
};

class BlobInput : public AP4_ByteStream {
public:
    Bytes data;
    AP4_Position position;
    bool arm_read_fault;
    AP4_Position fault_offset;
    AP4_Result read_code;
    BlobInput(const Bytes& b) : data(b), position(0), arm_read_fault(false),
                                fault_offset(0), read_code(-7282) {}
    virtual AP4_Result ReadPartial(void* buffer, AP4_Size bytes, AP4_Size& read) {
        if (position >= data.size()) { read=0; return AP4_ERROR_EOS; }
        if (arm_read_fault && position == fault_offset) {
            read = 0;
            return read_code;
        }
        AP4_Size available = (AP4_Size)std::min<uint64_t>(bytes, data.size()-position);
        std::memcpy(buffer, data.data()+position, available);
        position += available;
        read = available;
        return AP4_SUCCESS;
    }
    virtual AP4_Result WritePartial(const void*, AP4_Size, AP4_Size& written) {
        written = 0; return AP4_ERROR_NOT_SUPPORTED;
    }
    virtual AP4_Result Seek(AP4_Position p) { position = p; return AP4_SUCCESS; }
    virtual AP4_Result Tell(AP4_Position& p) { p=position; return AP4_SUCCESS; }
    virtual AP4_Result GetSize(AP4_LargeSize& size) { size=data.size(); return AP4_SUCCESS; }
    virtual void AddReference() {}
    virtual void Release() {}
};

/*----------------------------- shared fixtures -----------------------------*/

static MovieSpec ComboSpec() {
    MovieSpec spec;
    spec.timescale = 1000;
    spec.duration = 2620;
    spec.has_audio = true;

    spec.video = TrackSpec(true, 700);
    spec.video.tkhd_duration = 2620;
    spec.video.parameter_markers = Bytes{0xa1, 0xb2};
    // Four decoded frames, then an unused source gap sample, then four more.
    spec.video.samples = {
        SampleSpec(175, 350, true,  0x11, 0),
        SampleSpec(175, 1,   false, 0x12, 0),
        SampleSpec(175, 175, false, 0x13, 0),
        SampleSpec(175, 0,   false, 0x14, 0),
        SampleSpec(700, 0,   false, 0x15, 0), // source gap [700,1400)
        SampleSpec(175, 0,   true,  0x21, 1),
        SampleSpec(175, 0,   false, 0x22, 1),
        SampleSpec(175, 0,   false, 0x23, 1),
        SampleSpec(175, 0,   false, 0x24, 1)
    };
    spec.video.edits = {
        EditSpec(120, -1),
        EditSpec(1000, 0),
        EditSpec(300, -1),
        EditSpec(1000, 1400),
        EditSpec(200, -1)
    };

    spec.audio = TrackSpec(false, 44100);
    spec.audio.tkhd_duration = 2620;
    for (unsigned i=0; i<10; ++i) {
        spec.audio.samples.push_back(SampleSpec(441, 0, true, (uint8_t)(0x40+i), 0));
    }
    spec.audio.edits = {
        EditSpec(50, -1),
        EditSpec(100, 0)
    };
    return spec;
}

static AP4_Result WriteBuiltMovie(const BuiltMovie& built,
                                  AP4_UI64 origin,
                                  AP4_UI32 interval,
                                  AP4_MemoryByteStream*& out,
                                  AP4_File*& file_holder,
                                  AP4_MemoryByteStream*& in_holder) {
    in_holder = new AP4_MemoryByteStream(built.bytes.data(), (AP4_Size)built.bytes.size());
    file_holder = new AP4_File(*in_holder);
    out = new AP4_MemoryByteStream();
    AP4_Movie* movie = file_holder->GetMovie();
    if (movie == NULL) return AP4_ERROR_INVALID_FORMAT;
    return AP4_EditedTsWriter::Write(*movie, *out, origin, interval);
}

/*----------------------------- timeline cases -----------------------------*/

static int case_timeline_combo(void) {
    MovieSpec spec = ComboSpec();
    BuiltMovie built = BuildMovie(spec);
    AP4_MemoryByteStream* in=NULL;
    AP4_MemoryByteStream* out=NULL;
    AP4_File* file=NULL;
    AP4_Result result = WriteBuiltMovie(built, 100000, 5400, out, file, in);
    TASK_CHECK(AP4_SUCCEEDED(result));
    TASK_CHECK(out != NULL);

    TsInfo ts = ParseTs(out->GetData(), out->GetDataSize());
    TASK_CHECK(ts.ok);
    TASK_CHECK(ts.pat == 1);
    TASK_CHECK(ts.pmt == 1);
    TASK_CHECK(ts.pcr_pid_from_pmt == 0x102);
    TASK_CHECK(ts.pes.size() == 18);
    TASK_CHECK(ts.pcrs.size() == 45);

    // PCR grid: 0,5400,...,232200 plus movie end 235800.
    for (size_t i=0; i<44; ++i) {
        TASK_CHECK(ts.pcrs[i] == (100000ULL+(uint64_t)i*5400)*300);
    }
    TASK_CHECK(ts.pcrs[44] == (100000ULL+235800)*300);

    // At movie time 10800 the PCR comes first, then tied-DTS video, then audio.
    size_t tied_event = 0;
    bool found_tied_pcr = false;
    for (size_t i=0; i<ts.events.size(); ++i) {
        if (ts.events[i].pcr && ts.events[i].clock == (100000ULL+10800)*300) {
            tied_event = i;
            found_tied_pcr = true;
            break;
        }
    }
    TASK_CHECK(found_tied_pcr);
    TASK_CHECK(tied_event+2 < ts.events.size());
    TASK_CHECK(!ts.events[tied_event+1].pcr && ts.events[tied_event+1].pid == 0x102);
    TASK_CHECK(!ts.events[tied_event+2].pcr && ts.events[tied_event+2].pid == 0x101);

    uint64_t expected_dts[] = {
        // Audio starts at movie unit 50 => 4500 in 90k.
        109000+4500, 109000+5400, 109000+6300, 109000+7200,
        109000+8100, 109000+9000, 109000+9900, 109000+10800,
        // First video DTS is at 120 movie units => 10800.
        109000+10800,
        109000+11700, 109000+12600,
        109000+33300, 109000+55800, 109000+78300,
        // Remaining audio.
        109000+11700, 109000+12600,
        // Second video segment.
        109000+127800, 109000+150300, 109000+172800, 109000+195300
    };
    // The exact global order is checked through decoded DTS values below;
    // the initializer above documents the interleaving neighborhood.
    (void)expected_dts;

    std::vector<uint64_t> decoded_dts;
    for (size_t i=0; i<ts.pes_order.size(); ++i) {
        const PesInfo& pes = ts.pes_order[i];
        TASK_CHECK((pes.pid == 0x102 && pes.stream_id == 0xe0) ||
                   (pes.pid == 0x101 && pes.stream_id == 0xc0));
        decoded_dts.push_back(pes.has_dts ? pes.dts : pes.pts);
    }
    TASK_CHECK(decoded_dts.size() == 18);
    for (size_t i=1; i<decoded_dts.size(); ++i) {
        TASK_CHECK(decoded_dts[i-1] <= decoded_dts[i]);
    }

    // Exact projected video timestamps, including independent CTS flooring.
    const PesInfo* v0=NULL;
    const PesInfo* v1=NULL;
    const PesInfo* v4=NULL;
    for (size_t i=0; i<ts.pes.size(); ++i) {
        if (ts.pes[i].pid != 0x102) continue;
        if (!v0) v0=&ts.pes[i];
        else if (!v1) v1=&ts.pes[i];
    }
    // pes vector is PID-finish order (audio then video); collect video directly.
    std::vector<PesInfo> video;
    for (size_t i=0; i<ts.pes.size(); ++i) {
        if (ts.pes[i].pid == 0x102) video.push_back(ts.pes[i]);
    }
    TASK_CHECK(video.size() == 8);
    TASK_CHECK(video[0].pts == 109000+55800);
    TASK_CHECK(video[0].has_dts);
    TASK_CHECK(video[0].dts == 109000+10800);
    TASK_CHECK(video[1].pts == 109000+33428);
    TASK_CHECK(video[1].dts == 109000+33300);
    TASK_CHECK(video[2].pts == 109000+78300);
    TASK_CHECK(video[2].dts == 109000+55800);
    TASK_CHECK(video[4].pts == 109000+127800);
    TASK_CHECK(video[4].dts == 109000+127800);

    // Real Annex-B payload and description-specific parameter sets.
    static const unsigned char sps0[] = {0,0,0,1,0x67,0x42,0xa1};
    static const unsigned char pps0[] = {0,0,0,1,0x68,0xa1};
    static const unsigned char sps1[] = {0,0,0,1,0x67,0x42,0xb2};
    static const unsigned char pps1[] = {0,0,0,1,0x68,0xb2};
    TASK_CHECK(ContainsBytes(video[0].payload, sps0, sizeof(sps0)));
    TASK_CHECK(ContainsBytes(video[0].payload, pps0, sizeof(pps0)));
    TASK_CHECK(!ContainsBytes(video[0].payload, sps1, sizeof(sps1)));
    TASK_CHECK(ContainsBytes(video[4].payload, sps1, sizeof(sps1)));
    TASK_CHECK(ContainsBytes(video[4].payload, pps1, sizeof(pps1)));
    TASK_CHECK(!ContainsBytes(video[4].payload, sps0, sizeof(sps0)));
    static const unsigned char sample0_tail[] = {0x65,0x88,0x11,1};
    TASK_CHECK(ContainsBytes(video[0].payload, sample0_tail, sizeof(sample0_tail)));
    static const unsigned char sample5_tail[] = {0x65,0x88,0x21,6};
    TASK_CHECK(ContainsBytes(video[4].payload, sample5_tail, sizeof(sample5_tail)));

    // ADTS identity, length and untouched AAC marker payload.
    unsigned audio_count=0;
    for (size_t i=0; i<ts.pes.size(); ++i) {
        const PesInfo& a = ts.pes[i];
        if (a.pid != 0x101) continue;
        TASK_CHECK(a.stream_id == 0xc0);
        TASK_CHECK(!a.has_dts);
        TASK_CHECK(a.pts == 109000+4500+(uint64_t)audio_count*900);
        ++audio_count;
        TASK_CHECK(a.payload.size() == 11);
        TASK_CHECK(a.payload[0] == 0xff);
        TASK_CHECK((a.payload[1]&0xf6) == 0xf0);
        unsigned frame_length = ((a.payload[3]&3)<<11)|(a.payload[4]<<3)|(a.payload[5]>>5);
        TASK_CHECK(frame_length == 11);
        TASK_CHECK(a.payload[7] == 0xaa);
    }
    TASK_CHECK(audio_count == 10);

    delete file;
    out->Release();
    in->Release();
    return 0;
}

static int case_timeline_media_ends_early(void) {
    MovieSpec spec;
    spec.timescale = 1000;
    spec.duration = 5000; // 450000 in 90k
    spec.has_audio = false;
    spec.video = TrackSpec(true, 1000);
    spec.video.tkhd_duration = 5000;
    spec.video.samples = { SampleSpec(40,0,true,1,0), SampleSpec(40), SampleSpec(40), SampleSpec(40) };
    // Media occupies movie [1000,1160], followed by a long movie tail.
    spec.video.edits = { EditSpec(1000,-1), EditSpec(160,0), EditSpec(3840,-1) };

    BuiltMovie built = BuildMovie(spec);
    AP4_MemoryByteStream* in=NULL,*out=NULL; AP4_File* file=NULL;
    AP4_Result result = WriteBuiltMovie(built, 0, 9000, out, file, in);
    TASK_CHECK(AP4_SUCCEEDED(result));
    TsInfo ts = ParseTs(out->GetData(), out->GetDataSize());
    TASK_CHECK(ts.ok);
    TASK_CHECK(ts.pes.size() == 4);
    TASK_CHECK(ts.pcrs.back() == 450000ULL*300);
    TASK_CHECK(ts.pcrs.size() == 51); // 0..450000 in 9000 steps, endpoint coincides
    uint64_t last_media_dts = ts.pes.back().has_dts ? ts.pes.back().dts : ts.pes.back().pts;
    TASK_CHECK(last_media_dts == 9000+90000+10800);
    delete file; out->Release(); in->Release();
    return 0;
}

/*------------------------------ pcr cases ------------------------------*/

static int case_pcr_all_blank(void) {
    MovieSpec spec = ComboSpec();
    spec.video.samples.clear();
    spec.audio.samples.clear();
    spec.video.edits = { EditSpec(2620,-1) };
    spec.audio.edits = { EditSpec(2620,-1) };
    BuiltMovie built = BuildMovie(spec);

    AP4_MemoryByteStream* in = new AP4_MemoryByteStream(built.bytes.data(), (AP4_Size)built.bytes.size());
    AP4_File file(*in);
    AP4_Movie* movie = file.GetMovie();
    TASK_CHECK(movie != NULL);
    AP4_MemoryByteStream* out = new AP4_MemoryByteStream();
    AP4_Result result = AP4_EditedTsWriter::Write(*movie, *out, 777, 5400);
    TASK_CHECK(AP4_SUCCEEDED(result));

    TsInfo ts = ParseTs(out->GetData(), out->GetDataSize());
    TASK_CHECK(ts.ok);
    TASK_CHECK(ts.pat == 1 && ts.pmt == 1);
    TASK_CHECK(ts.pes.empty());
    TASK_CHECK(ts.pcrs.size() == 45);
    TASK_CHECK(ts.pcrs[0] == 777*300);
    TASK_CHECK(ts.pcrs.back() == (777+235800)*300);
    out->Release(); in->Release();
    return 0;
}

static int case_pcr_33bit_wrap(void) {
    MovieSpec spec;
    spec.timescale = 1000;
    spec.duration = 2000; // end=180000
    spec.has_audio = false;
    spec.video = TrackSpec(true, 1000);
    spec.video.tkhd_duration = 40;
    spec.video.samples = { SampleSpec(40,0,true,9,0) };

    const uint64_t mod33 = 1ULL<<33;
    uint64_t origin = mod33-9000+123;
    BuiltMovie built = BuildMovie(spec);
    AP4_MemoryByteStream* in=NULL,*out=NULL; AP4_File* file=NULL;
    AP4_Result result = WriteBuiltMovie(built, origin, 9000, out, file, in);
    TASK_CHECK(AP4_SUCCEEDED(result));
    TsInfo ts = ParseTs(out->GetData(), out->GetDataSize());
    TASK_CHECK(ts.ok);
    TASK_CHECK(ts.pes.size() == 1);
    TASK_CHECK(ts.pes[0].pts == 123); // wrapped PES PTS
    TASK_CHECK(ts.pcrs.size() == 21);
    for (size_t i=0; i<ts.pcrs.size(); ++i) {
        uint64_t grid = (uint64_t)i*9000;
        uint64_t expected_base = (origin+grid) & (mod33-1);
        TASK_CHECK(ts.pcrs[i] == expected_base*300);
    }
    delete file; out->Release(); in->Release();
    return 0;
}

static int case_pcr_direct_packet(void) {
    AP4_Mpeg2TsWriter::Stream stream(0x123);
    AP4_MemoryByteStream* out = new AP4_MemoryByteStream();
    AP4_Result result = stream.WritePcrPacket(300000, *out);
    TASK_CHECK(AP4_SUCCEEDED(result));
    TASK_CHECK(out->GetDataSize() == 188);
    const unsigned char* d = out->GetData();
    TASK_CHECK(d[0] == 0x47);
    TASK_CHECK(((d[1]&31)<<8|d[2]) == 0x123);
    TASK_CHECK((d[1]&64) == 0);
    TASK_CHECK(((d[3]>>4)&3) == 2);
    TASK_CHECK((d[3]&15) == 0);
    TASK_CHECK(d[4] == 183 && d[5] == 0x10);
    TASK_CHECK(ParsePcr27(d+6) == 300000);
    out->Release();
    return 0;
}

/*----------------------------- failure cases -----------------------------*/

static int ExpectPreOutputFailure(const MovieSpec& spec,
                                  uint64_t origin=0,
                                  uint32_t interval=3600) {
    BuiltMovie built = BuildMovie(spec);
    AP4_MemoryByteStream* in = new AP4_MemoryByteStream(built.bytes.data(), (AP4_Size)built.bytes.size());
    AP4_File file(*in);
    AP4_Movie* movie = file.GetMovie();
    TASK_CHECK(movie != NULL);
    FaultSink sink;
    AP4_Result result = AP4_EditedTsWriter::Write(*movie, sink, origin, interval);
    in->Release();
    return AP4_FAILED(result) && sink.data.empty() ? 0 : 1;
}

static int case_failure_bad_edits(void) {
    {
        MovieSpec s = ComboSpec(); s.timescale = 0;
        TASK_CHECK(ExpectPreOutputFailure(s) == 0);
    }
    {
        MovieSpec s = ComboSpec(); s.video.timescale = 0;
        TASK_CHECK(ExpectPreOutputFailure(s) == 0);
    }
    {
        MovieSpec s = ComboSpec(); s.duplicate_video = true;
        TASK_CHECK(ExpectPreOutputFailure(s) == 0);
    }
    {
        MovieSpec s = ComboSpec();
        s.video.edits[1].rate_integer = 2;
        TASK_CHECK(ExpectPreOutputFailure(s) == 0);
    }
    {
        MovieSpec s = ComboSpec();
        s.video.edits[1].rate_integer = 1;
        s.video.edits[1].rate_fraction = 1;
        TASK_CHECK(ExpectPreOutputFailure(s) == 0);
    }
    {
        MovieSpec s = ComboSpec();
        s.video.edits[1].media_time = 7; // not a DTS boundary
        TASK_CHECK(ExpectPreOutputFailure(s) == 0);
    }
    {
        MovieSpec s = ComboSpec();
        s.video.samples[0].sync = false;
        TASK_CHECK(ExpectPreOutputFailure(s) == 0);
    }
    {
        MovieSpec s = ComboSpec();
        // First CTS was 350 media units; push it beyond the 700-unit window.
        s.video.samples[0].cts_delta = 701;
        TASK_CHECK(ExpectPreOutputFailure(s) == 0);
    }
    {
        MovieSpec s = ComboSpec();
        // Make the second source interval start before the first ends.
        s.video.edits[3].media_time = 500;
        TASK_CHECK(ExpectPreOutputFailure(s) == 0);
    }
    {
        MovieSpec s = ComboSpec();
        s.video.external_ref = true;
        TASK_CHECK(ExpectPreOutputFailure(s) == 0);
    }
    {
        MovieSpec s = ComboSpec();
        TASK_CHECK(ExpectPreOutputFailure(s, ~0ULL, 1) == 0);
    }
    {
        MovieSpec s = ComboSpec();
        BuiltMovie built = BuildMovie(s);
        AP4_MemoryByteStream* in = new AP4_MemoryByteStream(built.bytes.data(), (AP4_Size)built.bytes.size());
        AP4_File file(*in);
        FaultSink sink;
        TASK_CHECK(file.GetMovie() != NULL);
        TASK_CHECK(AP4_FAILED(AP4_EditedTsWriter::Write(*file.GetMovie(), sink, 0, 0)));
        TASK_CHECK(sink.data.empty());
        TASK_CHECK(AP4_FAILED(AP4_EditedTsWriter::Write(*file.GetMovie(), sink, 0, 9001)));
        TASK_CHECK(sink.data.empty());
        in->Release();
    }
    return 0;
}

static int RunFaultMode(FaultSink::Mode mode) {
    MovieSpec spec = ComboSpec();
    BuiltMovie built = BuildMovie(spec);
    BlobInput input(built.bytes);
    AP4_File file(input);
    AP4_Movie* movie = file.GetMovie();
    TASK_CHECK(movie != NULL);
    FaultSink sink(mode, -7281);
    AP4_Result result = AP4_EditedTsWriter::Write(*movie, sink, 100000, 5400);
    TASK_CHECK(result == -7281);
    return 0;
}

static int case_failure_output_writes(void) {
    TASK_CHECK(RunFaultMode(FaultSink::FAIL_PAT) == 0);
    TASK_CHECK(RunFaultMode(FaultSink::FAIL_PMT) == 0);
    TASK_CHECK(RunFaultMode(FaultSink::FAIL_PCR) == 0);
    TASK_CHECK(RunFaultMode(FaultSink::FAIL_VIDEO_MEDIA) == 0);
    TASK_CHECK(RunFaultMode(FaultSink::FAIL_AUDIO_MEDIA) == 0);
    return 0;
}

static int case_failure_short_read(void) {
    MovieSpec spec = ComboSpec();
    BuiltMovie built = BuildMovie(spec);
    BlobInput input(built.bytes);
    AP4_File file(input);
    AP4_Movie* movie = file.GetMovie();
    TASK_CHECK(movie != NULL);

    input.arm_read_fault = true;
    input.fault_offset = built.first_video_offset;
    FaultSink sink;
    AP4_Result result = AP4_EditedTsWriter::Write(*movie, sink, 100000, 5400);
    TASK_CHECK(result == input.read_code);
    return 0;
}

/*---------------------------- compatibility ----------------------------*/

static int case_compat_legacy_path(void) {
    MovieSpec spec;
    spec.timescale = 1000;
    spec.duration = 160;
    spec.has_audio = false;
    spec.video = TrackSpec(true, 1000);
    spec.video.parameter_markers = Bytes{0x66};
    for (unsigned i=0; i<4; ++i) {
        spec.video.samples.push_back(SampleSpec(40, 0, i==0, (uint8_t)(i+1), 0));
    }
    BuiltMovie built = BuildMovie(spec);
    AP4_MemoryByteStream* in = new AP4_MemoryByteStream(built.bytes.data(), (AP4_Size)built.bytes.size());
    AP4_File file(*in);
    AP4_Movie* movie = file.GetMovie();
    TASK_CHECK(movie != NULL);
    AP4_Track* track = movie->GetTrack(AP4_Track::TYPE_VIDEO);
    TASK_CHECK(track != NULL);

    AP4_Mpeg2TsWriter writer;
    AP4_Mpeg2TsWriter::SampleStream* stream = NULL;
    TASK_CHECK(AP4_SUCCEEDED(writer.SetVideoStream(1000, AP4_MPEG2_STREAM_TYPE_AVC,
                                                   0xe0, stream, 0x102,
                                                   NULL, 0, 9000)));
    AP4_MemoryByteStream* legacy = new AP4_MemoryByteStream();
    TASK_CHECK(AP4_SUCCEEDED(writer.WritePAT(*legacy)));
    TASK_CHECK(AP4_SUCCEEDED(writer.WritePMT(*legacy)));
    for (unsigned i=0; i<4; ++i) {
        AP4_Sample sample;
        AP4_DataBuffer data;
        TASK_CHECK(AP4_SUCCEEDED(track->ReadSample(i, sample, data)));
        AP4_SampleDescription* desc = track->GetSampleDescription(sample.GetDescriptionIndex());
        TASK_CHECK(AP4_SUCCEEDED(stream->WriteSample(sample, data, desc, true, *legacy)));
    }
    TASK_CHECK(inspect_simple(legacy->GetData(), legacy->GetDataSize(), false) == 0);

    AP4_MemoryByteStream* edited = new AP4_MemoryByteStream();
    TASK_CHECK(AP4_SUCCEEDED(AP4_EditedTsWriter::Write(*movie, *edited, 100000, 3600)));
    TASK_CHECK(inspect_simple(edited->GetData(), edited->GetDataSize(), true) == 0);

    legacy->Release();
    edited->Release();
    in->Release();
    return 0;
}

/*----------------------------- registration -----------------------------*/

const struct task_case task_cases[] = {
    {"timeline", "combo_edits_bframes_descriptions_and_silence", case_timeline_combo},
    {"timeline", "media_ends_before_movie", case_timeline_media_ends_early},
    {"pcr", "all_blank_grid_and_cc", case_pcr_all_blank},
    {"pcr", "33bit_wrap", case_pcr_33bit_wrap},
    {"pcr", "direct_pcr_packet", case_pcr_direct_packet},
    {"failure", "bad_edit_and_preflight", case_failure_bad_edits},
    {"failure", "pat_pmt_pcr_media_writes", case_failure_output_writes},
    {"failure", "short_read", case_failure_short_read},
    {"compat", "legacy_avc_chain_and_edited_inspector", case_compat_legacy_path}
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
