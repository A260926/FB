/*
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is MP4v2.
 */

// Task tests for MP4GetVideoTimeCode. Every container here is synthesized
// in a temp file from raw box bytes so that edit lists, composition offsets
// and timecode descriptions can be driven into exact corner cases.

#include "task_support/check.h"

#include <mp4v2/mp4v2.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

typedef std::vector<uint8_t> Bytes;

void putU32(Bytes& b, uint32_t x)
{
    for (int i = 3; i >= 0; --i) b.push_back((x >> (i * 8)) & 255u);
}
void putU16(Bytes& b, uint16_t x)
{
    b.push_back((x >> 8) & 255u);
    b.push_back(x & 255u);
}
void putBytes(Bytes& b, const Bytes& x)
{
    b.insert(b.end(), x.begin(), x.end());
}
Bytes words(std::vector<uint32_t> xs)
{
    Bytes b;
    for (size_t i = 0; i < xs.size(); ++i) putU32(b, xs[i]);
    return b;
}
Bytes box(const char* t, Bytes b)
{
    Bytes o;
    putU32(o, (uint32_t)(b.size() + 8));
    o.insert(o.end(), t, t + 4);
    putBytes(o, b);
    return o;
}
Bytes fullBox(const char* t, Bytes b, uint32_t vf = 0)
{
    Bytes o = words(std::vector<uint32_t>(1, vf));
    putBytes(o, b);
    return box(t, o);
}
Bytes cat(std::vector<Bytes> xs)
{
    Bytes b;
    for (size_t i = 0; i < xs.size(); ++i) putBytes(b, xs[i]);
    return b;
}
Bytes matrix()
{
    return words(std::vector<uint32_t>{
        65536, 0, 0, 0, 65536, 0, 0, 0, 0x40000000u });
}

struct SttsEntry { uint32_t count; uint32_t delta; };
struct CttsEntry { uint32_t count; int32_t offset; };
struct ElstEntry {
    uint32_t duration;   // movie timeline units
    int64_t  mediaTime;  // -1 => empty edit
    uint16_t rate;       // 1.0 fixed point high half
};
struct TcDescription {
    uint32_t flags;
    uint32_t timeScale;
    uint32_t frameDuration;
    uint8_t  numFrames;
};
struct TcSample {
    uint32_t value;      // big-endian counter payload
    uint32_t duration;   // tmcd track timescale units
    uint32_t descIndex;  // 1-based stsd index
};

// Builds a self-contained, non-fragmented MOV with one video track and one
// tmcd track. Chunk offsets are patched after the media section is laid out.
class MovBuilder {
public:
    enum RefMode { REF_NORMAL, REF_NONE, REF_EXTRA, REF_CUSTOM };

    MovBuilder(uint32_t movieTimeScale)
        : m_movieTs(movieTimeScale)
        , m_refMode(REF_NORMAL), m_extraRefId(0), m_customRefId(0) {}

    void setVideo(
        uint32_t trackId,
        uint32_t timeScale,
        const std::vector<SttsEntry>& stts,
        const std::vector<CttsEntry>& ctts,
        const std::vector<ElstEntry>& edits,
        uint32_t sampleCount)
    {
        m_videoId = trackId;
        m_videoTs = timeScale;
        m_videoStts = stts;
        m_videoCtts = ctts;
        m_videoEdits = edits;
        m_videoSamples = sampleCount;
    }

    void setTimecode(
        uint32_t trackId,
        uint32_t timeScale,
        const std::vector<TcDescription>& descriptions,
        const std::vector<TcSample>& samples,
        const std::vector<ElstEntry>& edits)
    {
        m_tcId = trackId;
        m_tcTs = timeScale;
        m_tcDesc = descriptions;
        m_tcSamples = samples;
        m_tcEdits = edits;
    }

    // suppress the video track's tref box entirely
    void setNoReference() { m_refMode = REF_NONE; }

    // add a second tmcd target so the association is ambiguous
    void setExtraReference(uint32_t extraTrackId)
    {
        m_refMode = REF_EXTRA;
        m_extraRefId = extraTrackId;
    }

    // point the single tmcd reference at a non-existent track
    void setReferenceTarget(uint32_t targetTrackId)
    {
        m_refMode = REF_CUSTOM;
        m_customRefId = targetTrackId;
    }

    bool write(const std::string& path) const
    {
        // Lay out the media section first, one chunk per video sample group
        // and one chunk per timecode sample, recording absolute offsets.
        Bytes media;
        std::vector<uint32_t> videoChunks;
        std::vector<uint32_t> tcChunks;

        // A single video chunk holds all video payload markers.
        videoChunks.push_back((uint32_t)media.size());
        for (uint32_t i = 0; i < m_videoSamples; ++i) {
            char mark[4] = {
                'V', (char)('0' + (i / 10) % 10),
                (char)('0' + i % 10), 0 };
            mark[3] = (char)('A' + (i % 26));
            media.insert(media.end(), mark, mark + 4);
        }

        // Every timecode sample is its own chunk so stsc can switch
        // descriptions at arbitrary points.
        for (size_t i = 0; i < m_tcSamples.size(); ++i) {
            tcChunks.push_back((uint32_t)media.size());
            putU32(media, m_tcSamples[i].value);
        }

        const Bytes ftyp = box("ftyp",
            Bytes{ 'i','s','o','m',0,0,0,0,'i','s','o','m','q','t',' ',' ' });
        const uint32_t mediaBase = (uint32_t)ftyp.size() + 8;
        const Bytes mdat = box("mdat", media);

        Bytes mvhd = words(std::vector<uint32_t>{
            0, 0, m_movieTs, 0, 65536, 0x01000000, 0, 0 });
        putBytes(mvhd, matrix());
        mvhd.resize(mvhd.size() + 24, 0);
        putU32(mvhd, 2); // next track id

        Bytes moovContent;
        putBytes(moovContent, fullBox("mvhd", mvhd));
        putBytes(moovContent, buildVideoTrack(mediaBase, videoChunks));
        putBytes(moovContent, buildTcTrack(mediaBase, tcChunks));
        const Bytes moov = box("moov", moovContent);

        Bytes file;
        putBytes(file, ftyp);
        putBytes(file, mdat);
        putBytes(file, moov);

        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
        if (std::fclose(f)) ok = false;
        return ok;
    }

private:
    static Bytes elstBox(const std::vector<ElstEntry>& edits)
    {
        Bytes body = words(std::vector<uint32_t>{
            (uint32_t)edits.size() });
        for (size_t i = 0; i < edits.size(); ++i) {
            putU32(body, edits[i].duration);
            if (edits[i].mediaTime < 0) {
                putU32(body, 0xFFFFFFFFu);
            } else {
                putU32(body, (uint32_t)edits[i].mediaTime);
            }
            putU16(body, edits[i].rate);
            putU16(body, 0);
        }
        return box("edts", fullBox("elst", body));
    }

    Bytes buildVideoTrack(
        uint32_t mediaBase,
        const std::vector<uint32_t>& chunks) const
    {
        Bytes tkhd = words(std::vector<uint32_t>{
            0, 0, m_videoId, 0, 0, 0, 0 });
        putBytes(tkhd, words(std::vector<uint32_t>{ 0, 0 }));
        putBytes(tkhd, matrix());
        putBytes(tkhd, words(std::vector<uint32_t>{
            16u << 16, 16u << 16 }));

        Bytes mdhd = words(std::vector<uint32_t>{
            0, 0, m_videoTs, 0 });
        putU16(mdhd, 0x55c4);
        putU16(mdhd, 0);

        Bytes hdr = words(std::vector<uint32_t>{ 0 });
        const char* h = "vide";
        hdr.insert(hdr.end(), h, h + 4);
        hdr.resize(hdr.size() + 13, 0);

        // avc1 sample entry, 78 fixed bytes plus a tiny avcC child. Field
        // positions follow the QuickTime video sample entry layout.
        Bytes avc1(78, 0);
        avc1[7] = 1; // data reference index (offset 6)
        avc1[25] = 16; // height
        avc1[27] = 16;
        avc1[29] = 72; // width 72
        avc1[33] = 72;
        avc1[41] = 1;
        avc1[75] = 24; // depth
        avc1[76] = 255;
        avc1[77] = 255; // color table id
        putBytes(avc1, box("avcC",
            Bytes{ 1, 66, 0, 30, 255, 224, 0 }));
        Bytes stsd = words(std::vector<uint32_t>{ 1 });
        putBytes(stsd, box("avc1", avc1));

        Bytes stbl = cat(std::vector<Bytes>{
            fullBox("stsd", stsd),
            fullBox("stts", sttsTable(m_videoStts)),
            fullBox("ctts", cttsTable(m_videoCtts)),
            fullBox("stsc", words(std::vector<uint32_t>{
                1, 1, (uint32_t)m_videoSamples, 1 })),
            fullBox("stsz", words(std::vector<uint32_t>{
                4, m_videoSamples })),
            fullBox("stco", offsetTable(mediaBase, chunks)),
            fullBox("stss", words(std::vector<uint32_t>{ 1, 1 }))
        });

        Bytes dref = words(std::vector<uint32_t>{ 1 });
        putBytes(dref, fullBox("url ", Bytes{}, 1));
        Bytes minf = cat(std::vector<Bytes>{
            fullBox("vmhd", Bytes(8, 0), 1),
            box("dinf", fullBox("dref", dref)),
            box("stbl", stbl)
        });

        Bytes trak = cat(std::vector<Bytes>{
            fullBox("tkhd", tkhd, 3),
            box("mdia", cat(std::vector<Bytes>{
                fullBox("mdhd", mdhd),
                fullBox("hdlr", hdr),
                box("minf", minf) }))
        });
        if (!m_videoEdits.empty()) {
            putBytes(trak, elstBox(m_videoEdits));
        }
        if (m_refMode != REF_NONE) {
            Bytes refIds;
            putU32(refIds,
                m_refMode == REF_CUSTOM ? m_customRefId : m_tcId);
            if (m_refMode == REF_EXTRA) {
                putU32(refIds, m_extraRefId);
            }
            putBytes(trak, box("tref", box("tmcd", refIds)));
        }
        return box("trak", trak);
    }

    Bytes buildTcTrack(
        uint32_t mediaBase,
        const std::vector<uint32_t>& chunks) const
    {
        Bytes tkhd = words(std::vector<uint32_t>{
            0, 0, m_tcId, 0, 0, 0, 0 });
        putBytes(tkhd, words(std::vector<uint32_t>{ 0, 0 }));
        putBytes(tkhd, matrix());
        putBytes(tkhd, words(std::vector<uint32_t>{ 0, 0 }));

        Bytes mdhd = words(std::vector<uint32_t>{
            0, 0, m_tcTs, 0 });
        putU16(mdhd, 0);
        putU16(mdhd, 0);

        Bytes hdr = words(std::vector<uint32_t>{ 0 });
        const char* h = "tmcd";
        hdr.insert(hdr.end(), h, h + 4);
        hdr.resize(hdr.size() + 13, 0);

        Bytes stsd = words(std::vector<uint32_t>{
            (uint32_t)m_tcDesc.size() });
        for (size_t d = 0; d < m_tcDesc.size(); ++d) {
            Bytes entry(34, 0);
            entry[7] = 1; // data reference index at payload offset 6
            // flags live at bytes 8..11 of the entry payload
            putU32At(entry, 8, m_tcDesc[d].flags);
            // bytes 12..15 reserved, timeScale at 16
            putU32At(entry, 16, m_tcDesc[d].timeScale);
            putU32At(entry, 20, m_tcDesc[d].frameDuration);
            entry[24] = m_tcDesc[d].numFrames;
            putBytes(stsd, box("tmcd", entry));
        }

        // stts: one run per sample (durations may differ)
        Bytes sttsBody = words(std::vector<uint32_t>{
            (uint32_t)m_tcSamples.size() });
        for (size_t i = 0; i < m_tcSamples.size(); ++i) {
            putU32(sttsBody, 1);
            putU32(sttsBody, m_tcSamples[i].duration);
        }

        // stsc: run-length encode the per-chunk description indices;
        // every timecode sample occupies its own chunk (one sample each)
        Bytes stscBody;
        std::vector<uint32_t> runs;
        for (size_t i = 0; i < m_tcSamples.size();) {
            size_t j = i;
            while (j < m_tcSamples.size() &&
                   m_tcSamples[j].descIndex ==
                       m_tcSamples[i].descIndex) {
                ++j;
            }
            runs.push_back((uint32_t)(i + 1)); // first chunk, 1-based
            runs.push_back(1);                 // samples per chunk
            runs.push_back(m_tcSamples[i].descIndex);
            i = j;
        }
        putU32(stscBody, (uint32_t)(runs.size() / 3));
        putBytes(stscBody, words(runs));

        Bytes stbl = cat(std::vector<Bytes>{
            fullBox("stsd", stsd),
            fullBox("stts", sttsBody),
            fullBox("stsc", stscBody),
            fullBox("stsz", words(std::vector<uint32_t>{
                4, (uint32_t)m_tcSamples.size() })),
            fullBox("stco", offsetTable(mediaBase, chunks))
        });

        Bytes dref = words(std::vector<uint32_t>{ 1 });
        putBytes(dref, fullBox("url ", Bytes{}, 1));
        Bytes gmin = words(std::vector<uint32_t>{ 0 });
        putBytes(gmin, words(std::vector<uint32_t>{
            0x0040, 0x8000, 0x8000, 0x8000, 0, 0 }));
        Bytes gmhd = box("gmhd", cat(std::vector<Bytes>{
            fullBox("gmin", gmin), fullBox("tmcd", Bytes{}) }));
        Bytes minf = cat(std::vector<Bytes>{
            gmhd,
            box("dinf", fullBox("dref", dref)),
            box("stbl", stbl)
        });

        Bytes trak = cat(std::vector<Bytes>{
            fullBox("tkhd", tkhd, 3),
            box("mdia", cat(std::vector<Bytes>{
                fullBox("mdhd", mdhd),
                fullBox("hdlr", hdr),
                box("minf", minf) }))
        });
        if (!m_tcEdits.empty()) {
            putBytes(trak, elstBox(m_tcEdits));
        }
        return box("trak", trak);
    }

    static void putU32At(Bytes& b, size_t at, uint32_t x)
    {
        b[at] = (x >> 24) & 255u;
        b[at + 1] = (x >> 16) & 255u;
        b[at + 2] = (x >> 8) & 255u;
        b[at + 3] = x & 255u;
    }

    static Bytes sttsTable(const std::vector<SttsEntry>& entries)
    {
        Bytes b = words(std::vector<uint32_t>{
            (uint32_t)entries.size() });
        for (size_t i = 0; i < entries.size(); ++i) {
            putU32(b, entries[i].count);
            putU32(b, entries[i].delta);
        }
        return b;
    }

    static Bytes cttsTable(const std::vector<CttsEntry>& entries)
    {
        Bytes b = words(std::vector<uint32_t>{
            (uint32_t)entries.size() });
        for (size_t i = 0; i < entries.size(); ++i) {
            putU32(b, entries[i].count);
            putU32(b, (uint32_t)entries[i].offset);
        }
        return b;
    }

    static Bytes offsetTable(
        uint32_t mediaBase, const std::vector<uint32_t>& chunks)
    {
        Bytes b = words(std::vector<uint32_t>{
            (uint32_t)chunks.size() });
        for (size_t i = 0; i < chunks.size(); ++i) {
            putU32(b, mediaBase + chunks[i]);
        }
        return b;
    }

    uint32_t m_movieTs;
    uint32_t m_videoId;
    uint32_t m_videoTs;
    std::vector<SttsEntry> m_videoStts;
    std::vector<CttsEntry> m_videoCtts;
    std::vector<ElstEntry> m_videoEdits;
    uint32_t m_videoSamples;
    uint32_t m_tcId;
    uint32_t m_tcTs;
    std::vector<TcDescription> m_tcDesc;
    std::vector<TcSample> m_tcSamples;
    std::vector<ElstEntry> m_tcEdits;
    RefMode m_refMode;
    uint32_t m_extraRefId;
    uint32_t m_customRefId;
};

// Scratch file lifetime for one test case.
struct TempFile {
    std::string path;
    explicit TempFile(const char* tag)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s/mp4tc-%s-%d.mov",
            std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp",
            tag, (int)reinterpret_cast<intptr_t>(this));
        path = buf;
    }
    ~TempFile() { std::remove(path.c_str()); }
};

const TcDescription TC_25 = { 0x00000000u, 25, 1, 25 };
const TcDescription TC_30 = { 0x00000000u, 30, 1, 30 };
const TcDescription TC_DF30 = { 0x80000000u, 30000, 1001, 30 };
const TcDescription TC_DF60 = { 0x80000000u, 60000, 1001, 60 };
const TcDescription TC_25_WRAP = { 0x40000000u, 25, 1, 25 };

// ---------------------------------------------------------------------------
// association
// ---------------------------------------------------------------------------

static int case_unique_reference()
{
    TempFile tmp("assoc");
    MovBuilder b(1000);
    b.setVideo(1, 1000,
        { { 4, 40 } }, { { 4, 0 } }, {}, 4);
    b.setTimecode(2, 1000, { TC_25 },
        { { 90000u, 160, 1 } }, {});
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);

    MP4VideoTimeCode a{};
    MP4VideoTimeCode c{};
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 80, &a));
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 119, &c));
    // both ticks fall inside visible frame 3, so results must be identical
    TASK_CHECK(a.videoSampleId == 3 && c.videoSampleId == 3);
    TASK_CHECK(a.timeCodeTrackId == 2 && c.timeCodeTrackId == 2);
    TASK_CHECK(a.timeCodeSampleId == 1 && c.timeCodeSampleId == 1);
    TASK_CHECK(a.frameNumber == 90002 && c.frameNumber == 90002);
    TASK_CHECK(std::strcmp(a.text, "01:00:00:02") == 0);
    TASK_CHECK(std::strcmp(c.text, a.text) == 0);

    // ordinary sample read now works on the tmcd track and yields the raw
    // big-endian counter payload plus its table timing
    uint8_t* p = NULL;
    uint32_t n = 0;
    MP4Timestamp st = 0;
    MP4Duration dur = 0, ro = 0;
    bool sync = false;
    TASK_CHECK(MP4ReadSample(f, 2, 1, &p, &n, &st, &dur, &ro, &sync));
    TASK_CHECK(n == 4 && p != NULL);
    uint32_t counter = 0;
    for (uint32_t i = 0; i < 4; ++i) counter = (counter << 8) | p[i];
    TASK_CHECK(counter == 90000u && st == 0 && dur == 160);
    MP4Free(p);

    // the typed description is exposed through the regular property path
    {
        uint64_t v = 0;
        TASK_CHECK(MP4GetTrackIntegerProperty(
            f, 2, "mdia.minf.stbl.stsd.*[0].timeScale", &v));
        TASK_CHECK(v == 25);
    }
    {
        uint64_t v = 0;
        TASK_CHECK(MP4GetTrackIntegerProperty(
            f, 2, "mdia.minf.stbl.stsd.*[0].frameDuration", &v));
        TASK_CHECK(v == 1);
    }
    {
        uint64_t v = 0;
        TASK_CHECK(MP4GetTrackIntegerProperty(
            f, 2, "mdia.minf.stbl.stsd.*[0].numFrames", &v));
        TASK_CHECK(v == 25);
    }
    uint8_t* flags = NULL;
    uint32_t flagsSize = 0;
    TASK_CHECK(MP4GetTrackBytesProperty(
        f, 2, "mdia.minf.stbl.stsd.*[0].flags", &flags, &flagsSize));
    TASK_CHECK(flagsSize == 4 && flags[0] == 0 && flags[3] == 0);
    MP4Free(flags);

    MP4Close(f);
    return 0;
}

static int case_missing_reference()
{
    TempFile tmp("assoc");
    MovBuilder b(1000);
    b.setVideo(1, 1000, { { 2, 40 } }, { { 2, 0 } }, {}, 2);
    b.setTimecode(2, 1000, { TC_25 }, { { 0u, 160, 1 } }, {});
    b.setNoReference();
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);

    MP4VideoTimeCode sentinel;
    std::memset(&sentinel, 0xA5, sizeof(sentinel));
    MP4VideoTimeCode out = sentinel;
    TASK_CHECK(!MP4GetVideoTimeCode(f, 1, 10, &out));
    TASK_CHECK(std::memcmp(&out, &sentinel, sizeof(out)) == 0);

    MP4Close(f);
    return 0;
}

static int case_ambiguous_reference()
{
    TempFile tmp("assoc");
    MovBuilder b(1000);
    b.setVideo(1, 1000, { { 2, 40 } }, { { 2, 0 } }, {}, 2);
    b.setTimecode(2, 1000, { TC_25 }, { { 0u, 160, 1 } }, {});
    b.setExtraReference(3);
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);

    MP4VideoTimeCode sentinel;
    std::memset(&sentinel, 0x5A, sizeof(sentinel));
    MP4VideoTimeCode out = sentinel;
    TASK_CHECK(!MP4GetVideoTimeCode(f, 1, 10, &out));
    TASK_CHECK(std::memcmp(&out, &sentinel, sizeof(out)) == 0);

    MP4Close(f);
    return 0;
}

// ---------------------------------------------------------------------------
// projection
// ---------------------------------------------------------------------------

static int case_bframe_display_order()
{
    // DTS order 1,2,3,4 but display order is 2,1,3,4: selection must follow
    // PTS = DTS + composition offset, never DTS alone. All offsets are
    // non-negative.
    TempFile tmp("proj");
    MovBuilder b(1000);
    b.setVideo(1, 90000,
        { { 4, 1000 } },
        { { 1, 2000 }, { 1, 0 }, { 2, 2000 } },
        {}, 4);
    b.setTimecode(2, 90000, { TC_25 },
        { { 0u, 36000, 1 } }, {});
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);

    // display intervals (media units): s2 [1000,2000), s1 [2000,3000),
    // s3 [4000,5000), s4 [5000,6000); media = movie tick * 90
    const MP4Timestamp ticks[] = { 16, 27, 50, 61 };
    const MP4SampleId expected[] = { 2, 1, 3, 4 };
    for (uint32_t i = 0; i < 4; ++i) {
        MP4VideoTimeCode out{};
        TASK_CHECK(MP4GetVideoTimeCode(f, 1, ticks[i], &out));
        TASK_CHECK(out.videoSampleId == expected[i]);
    }

    // a DTS-only lookup would pick sample 1 at media time 1440, while the
    // presentation rule requires sample 2
    MP4VideoTimeCode check{};
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 16, &check));
    TASK_CHECK(check.videoSampleId == 2);

    MP4Close(f);
    return 0;
}

static int case_empty_edits_both_tracks()
{
    TempFile tmp("proj");
    MovBuilder b(1000);
    // video: empty 20 ticks, then media starting at zero for 160 ticks
    b.setVideo(1, 1000,
        { { 4, 40 } }, { { 4, 0 } },
        { { 20, -1, 1 }, { 160, 0, 1 } }, 4);
    // timecode: empty 20 ticks, then media zero for the rest
    b.setTimecode(2, 1000, { TC_25 },
        { { 0u, 1000, 1 } },
        { { 20, -1, 1 }, { 1000, 0, 1 } });
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);

    MP4VideoTimeCode sentinel;
    std::memset(&sentinel, 0x33, sizeof(sentinel));
    MP4VideoTimeCode rejected = sentinel;
    TASK_CHECK(!MP4GetVideoTimeCode(f, 1, 10, &rejected));
    TASK_CHECK(std::memcmp(&rejected, &sentinel, sizeof(rejected)) == 0);

    // movie tick 60 is 40 ticks into the live edit -> media time 40,
    // visible video frame 2; anchor maps to tc media time 40
    MP4VideoTimeCode out{};
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 60, &out));
    TASK_CHECK(out.videoSampleId == 2);
    TASK_CHECK(out.timeCodeSampleId == 1);
    TASK_CHECK(out.frameNumber == 1); // floor(40 * 25 / 1000)

    MP4Close(f);
    return 0;
}

static int case_repeated_reversed_edits()
{
    TempFile tmp("proj");
    MovBuilder b(1000);
    // media segment [40,160) is shown twice, then [120,160) again
    b.setVideo(1, 1000,
        { { 4, 40 } }, { { 4, 0 } },
        { { 20, -1, 1 },
          { 80, 40, 1 },
          { 80, 40, 1 },
          { 40, 120, 1 } }, 4);
    b.setTimecode(2, 1000, { TC_25 },
        { { 100u, 1000, 1 } },
        { { 20, -1, 1 }, { 1000, 0, 1 } });
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);

    MP4VideoTimeCode first{};
    MP4VideoTimeCode second{};
    MP4VideoTimeCode repeated{};
    MP4VideoTimeCode reversed{};
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 30, &first));    // media 50 -> sid2
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 59, &second));   // media 79 -> sid2
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 100, &repeated));// 2nd copy, media 40 -> sid2
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 190, &reversed));// reversed copy, media 130 -> sid4
    TASK_CHECK(first.videoSampleId == 2);
    TASK_CHECK(second.videoSampleId == 2);
    TASK_CHECK(repeated.videoSampleId == 2);
    TASK_CHECK(reversed.videoSampleId == 4);
    // the label persists within one counter sample even across copies
    TASK_CHECK(first.frameNumber == 100);
    TASK_CHECK(repeated.frameNumber == 102);
    TASK_CHECK(std::strcmp(first.text, "00:00:04:00") == 0);

    MP4Close(f);
    return 0;
}

static int case_rational_frame_boundaries()
{
    // movie ts 1000 vs video ts 30000, 1001-tick frames: movie ticks never
    // align exactly with frame boundaries, so all math stays rational.
    TempFile tmp("proj");
    MovBuilder b(1000);
    b.setVideo(1, 30000,
        { { 4, 1001 } }, { { 4, 0 } }, {}, 4);
    b.setTimecode(2, 24000, { TC_25 },
        { { 0u, 96000, 1 } }, {});
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);

    struct Probe { MP4Timestamp when; MP4SampleId sid; };
    const Probe probes[] = {
        { 0, 1 }, { 33, 1 }, { 34, 2 }, { 66, 2 }, { 67, 3 }
    };
    MP4VideoTimeCode prev{};
    bool havePrev = false;
    for (uint32_t i = 0; i < 5; ++i) {
        MP4VideoTimeCode out{};
        TASK_CHECK(MP4GetVideoTimeCode(f, 1, probes[i].when, &out));
        TASK_CHECK(out.videoSampleId == probes[i].sid);
        if (havePrev && probes[i].sid == probes[i - 1].sid) {
            TASK_CHECK(out.frameNumber == prev.frameNumber);
            TASK_CHECK(std::strcmp(out.text, prev.text) == 0);
        }
        prev = out;
        havePrev = true;
    }

    MP4Close(f);
    return 0;
}

// ---------------------------------------------------------------------------
// counter
// ---------------------------------------------------------------------------

static int case_counter_covers_many_frames()
{
    TempFile tmp("cnt");
    MovBuilder b(1000);
    b.setVideo(1, 1000, { { 8, 40 } }, { { 8, 0 } }, {}, 8);
    // one counter sample spans the whole clip, recorded value is frame 7
    b.setTimecode(2, 1000, { TC_25 },
        { { 7u, 1000, 1 } }, {});
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);

    const uint64_t expectedFrame[] = { 7, 8, 9, 10 };
    for (uint32_t i = 0; i < 4; ++i) {
        MP4VideoTimeCode out{};
        TASK_CHECK(MP4GetVideoTimeCode(f, 1, 40 * i + 1, &out));
        TASK_CHECK(out.videoSampleId == i + 1);
        TASK_CHECK(out.timeCodeSampleId == 1);
        TASK_CHECK(out.frameNumber == expectedFrame[i]);
    }

    MP4Close(f);
    return 0;
}

static int case_description_switch_and_jump()
{
    TempFile tmp("cnt");
    MovBuilder b(1000);
    // 18 video frames cover the full 720-tick timecode span
    b.setVideo(1, 1000, { { 18, 40 } }, { { 18, 0 } }, {}, 18);
    // description 1: 25fps for counter samples 1..3; description 2: 30fps
    // for samples 4..6, with a deliberate counter jump between the sections
    b.setTimecode(2, 1000,
        { TC_25, TC_30 },
        { { 100u, 120, 1 }, { 101u, 120, 1 }, { 102u, 120, 1 },
          { 500u, 120, 2 }, { 501u, 120, 2 }, { 502u, 120, 2 } },
        {});
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);

    // first section interpreted at 25fps; the label persists across the
    // whole counter sample even though video frames advance underneath
    MP4VideoTimeCode a{};
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 40, &a)); // anchor 40 -> counter 1
    TASK_CHECK(a.timeCodeSampleId == 1);
    TASK_CHECK(a.frameNumber == 101); // 100 + floor(40*25/1000)
    TASK_CHECK(std::strcmp(a.text, "00:00:04:01") == 0);

    MP4VideoTimeCode b2{};
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 150, &b2)); // anchor 150 -> counter 2
    TASK_CHECK(b2.timeCodeSampleId == 2);
    TASK_CHECK(b2.frameNumber == 101); // 101 + floor(30*25/1000)
    TASK_CHECK(std::strcmp(b2.text, "00:00:04:01") == 0);

    // second section must use the 30fps description, not carry over the 25fps
    MP4VideoTimeCode z{};
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 400, &z)); // anchor 400 -> counter 4
    TASK_CHECK(z.timeCodeSampleId == 4);
    TASK_CHECK(z.frameNumber == 501); // 500 + floor(40*30/1000)
    TASK_CHECK(std::strcmp(z.text, "00:00:16:21") == 0);

    MP4VideoTimeCode q{};
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 560, &q)); // anchor 560 -> counter 5
    TASK_CHECK(q.timeCodeSampleId == 5);
    TASK_CHECK(q.frameNumber == 503); // 501 + floor(80*30/1000)
    TASK_CHECK(std::strcmp(q.text, "00:00:16:23") == 0);

    // both descriptions are present and readable through the property path
    {
        uint64_t v = 0;
        TASK_CHECK(MP4GetTrackIntegerProperty(
            f, 2, "mdia.minf.stbl.stsd.*[0].numFrames", &v));
        TASK_CHECK(v == 25);
    }
    {
        uint64_t v = 0;
        TASK_CHECK(MP4GetTrackIntegerProperty(
            f, 2, "mdia.minf.stbl.stsd.*[1].numFrames", &v));
        TASK_CHECK(v == 30);
    }

    // raw counter payloads read back with ordinary MP4ReadSample
    for (MP4SampleId sid = 1; sid <= 6; ++sid) {
        uint8_t* p = NULL;
        uint32_t n = 0;
        TASK_CHECK(MP4ReadSample(f, 2, sid, &p, &n));
        TASK_CHECK(n == 4);
        uint32_t v = 0;
        for (uint32_t k = 0; k < 4; ++k) v = (v << 8) | p[k];
        TASK_CHECK(v == (sid <= 3 ? 99u + sid : 496u + sid));
        MP4Free(p);
    }

    MP4Close(f);
    return 0;
}

static int case_drop_frame_boundaries()
{
    TempFile tmp("cnt");
    MovBuilder b(30000);
    // one video frame per 1001 movie ticks mirrors the 29.97 timecode grid
    b.setVideo(1, 30000, { { 4, 1001 } }, { { 4, 0 } }, {}, 4);
    b.setTimecode(2, 30000, { TC_DF30 },
        { { 0u, 3600000, 1 } }, {});
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);

    // frame 0 and frame 1 use ordinary labels
    MP4VideoTimeCode f0{}, f1{};
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 0, &f0));
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 1001, &f1));
    TASK_CHECK(std::strcmp(f0.text, "00:00:00;00") == 0);
    TASK_CHECK(std::strcmp(f1.text, "00:00:00;01") == 0);
    MP4Close(f);

    // dedicated file with counter samples whose recorded values sit exactly
    // on the first label of minute 1 and of minute 10. Both tracks jump to
    // the minute-1 media point through identical unit-rate edits, and video
    // frames are one 1001-tick grid step each, so each frame anchor lands
    // exactly on a counter sample start.
    TempFile tmp2("cnt2");
    MovBuilder b2(30000);
    const uint64_t m1 = 1800ull * 1001ull;    // first label of minute 1
    const uint64_t m10 = 17982ull * 1001ull;  // first label of minute 10
    const uint32_t liveFrames2 = 17982u - 1800u + 10u;
    const uint32_t totalFrames2 = 1800u + liveFrames2;
    b2.setVideo(1, 30000,
        { { totalFrames2, 1001 } }, { { totalFrames2, 0 } },
        { { liveFrames2 * 1001u, (int64_t)m1, 1 } }, totalFrames2);
    b2.setTimecode(2, 30000, { TC_DF30 },
        { { 0u, (uint32_t)m1, 1 },
          { 1800u, (uint32_t)(m10 - m1), 1 },
          { 17982u, 100000, 1 } },
        { { liveFrames2 * 1001u, (int64_t)m1, 1 } });
    TASK_CHECK(b2.write(tmp2.path));
    MP4FileHandle g = MP4Read(tmp2.path.c_str());
    TASK_CHECK(g != MP4_INVALID_FILE_HANDLE);

    MP4VideoTimeCode minute{};
    TASK_CHECK(MP4GetVideoTimeCode(g, 1, 0, &minute));
    TASK_CHECK(minute.timeCodeSampleId == 2);
    TASK_CHECK(minute.videoSampleId == 1801);
    TASK_CHECK(minute.frameNumber == 1800);
    TASK_CHECK(std::strcmp(minute.text, "00:01:00;02") == 0);

    MP4VideoTimeCode ten{};
    TASK_CHECK(MP4GetVideoTimeCode(g, 1,
        (MP4Timestamp)(m10 - m1), &ten));
    TASK_CHECK(ten.timeCodeSampleId == 3);
    TASK_CHECK(ten.frameNumber == 17982);
    TASK_CHECK(std::strcmp(ten.text, "00:10:00;00") == 0);
    MP4Close(g);

    // 59.94 grid: 4 frames dropped per non-ten-minute minute
    TempFile tmp3("cnt3");
    MovBuilder b3(60000);
    const uint64_t s1 = 3600ull * 1001ull;   // first label of minute 1
    const uint64_t s10 = 35964ull * 1001ull; // first label of minute 10
    const uint32_t liveFrames3 = 35964u - 3600u + 10u;
    const uint32_t totalFrames3 = 3600u + liveFrames3;
    b3.setVideo(1, 60000,
        { { totalFrames3, 1001 } }, { { totalFrames3, 0 } },
        { { liveFrames3 * 1001u, (int64_t)s1, 1 } }, totalFrames3);
    b3.setTimecode(2, 60000, { TC_DF60 },
        { { 0u, (uint32_t)s1, 1 },
          { 3600u, (uint32_t)(s10 - s1), 1 },
          { 35964u, 100000, 1 } },
        { { liveFrames3 * 1001u, (int64_t)s1, 1 } });
    TASK_CHECK(b3.write(tmp3.path));
    MP4FileHandle h = MP4Read(tmp3.path.c_str());
    TASK_CHECK(h != MP4_INVALID_FILE_HANDLE);
    MP4VideoTimeCode m60{}, t60{};
    TASK_CHECK(MP4GetVideoTimeCode(h, 1, 0, &m60));
    TASK_CHECK(m60.timeCodeSampleId == 2);
    TASK_CHECK(m60.frameNumber == 3600);
    TASK_CHECK(std::strcmp(m60.text, "00:01:00;04") == 0);
    TASK_CHECK(MP4GetVideoTimeCode(h, 1,
        (MP4Timestamp)(s10 - s1), &t60));
    TASK_CHECK(t60.timeCodeSampleId == 3);
    TASK_CHECK(t60.frameNumber == 35964);
    TASK_CHECK(std::strcmp(t60.text, "00:10:00;00") == 0);
    MP4Close(h);
    return 0;
}

static int case_twenty_four_hour_wrap()
{
    TempFile tmp("cnt");
    const uint64_t dayFrames = 24u * 3600u * 25u;

    MovBuilder b(1000);
    b.setVideo(1, 1000, { { 2, 40 } }, { { 2, 0 } }, {}, 2);
    b.setTimecode(2, 1000, { TC_25_WRAP },
        { { (uint32_t)dayFrames, 100000, 1 } }, {});
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);
    MP4VideoTimeCode wrap{};
    TASK_CHECK(MP4GetVideoTimeCode(f, 1, 0, &wrap));
    TASK_CHECK(wrap.frameNumber == dayFrames);
    TASK_CHECK(std::strcmp(wrap.text, "00:00:00:00") == 0);
    MP4Close(f);

    // without the wrap flag the same frame number is still representable
    TempFile tmp2("cnt2");
    MovBuilder b2(1000);
    b2.setVideo(1, 1000, { { 2, 40 } }, { { 2, 0 } }, {}, 2);
    b2.setTimecode(2, 1000, { TC_25 },
        { { (uint32_t)dayFrames, 100000, 1 } }, {});
    TASK_CHECK(b2.write(tmp2.path));
    MP4FileHandle g = MP4Read(tmp2.path.c_str());
    TASK_CHECK(g != MP4_INVALID_FILE_HANDLE);
    MP4VideoTimeCode plain{};
    TASK_CHECK(MP4GetVideoTimeCode(g, 1, 0, &plain));
    TASK_CHECK(plain.frameNumber == dayFrames);
    TASK_CHECK(std::strcmp(plain.text, "24:00:00:00") == 0);
    MP4Close(g);
    return 0;
}

// ---------------------------------------------------------------------------
// reject
// ---------------------------------------------------------------------------

static int case_no_coverage_and_empty_edit()
{
    TempFile tmp("rej");
    MovBuilder b(1000);
    b.setVideo(1, 1000,
        { { 2, 40 } }, { { 2, 0 } },
        { { 20, -1, 1 }, { 80, 0, 1 } }, 2);
    b.setTimecode(2, 1000, { TC_25 },
        { { 0u, 1000, 1 } }, {});
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);

    MP4VideoTimeCode sentinel;
    std::memset(&sentinel, 0x77, sizeof(sentinel));

    MP4VideoTimeCode inEmpty = sentinel;
    TASK_CHECK(!MP4GetVideoTimeCode(f, 1, 5, &inEmpty));
    TASK_CHECK(std::memcmp(&inEmpty, &sentinel, sizeof(inEmpty)) == 0);

    MP4VideoTimeCode pastEnd = sentinel;
    TASK_CHECK(!MP4GetVideoTimeCode(f, 1, 500, &pastEnd));
    TASK_CHECK(std::memcmp(&pastEnd, &sentinel, sizeof(pastEnd)) == 0);

    MP4Close(f);
    return 0;
}

static int case_negative_ctts()
{
    TempFile tmp("rej");
    MovBuilder b(90000);
    b.setVideo(1, 90000, { { 3, 3000 } },
        { { 1, 0 }, { 1, (int32_t)0xFFFFFFFEu }, { 1, 0 } }, {}, 3);
    b.setTimecode(2, 90000, { TC_25 }, { { 0u, 100000, 1 } }, {});
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);
    MP4VideoTimeCode sentinel;
    std::memset(&sentinel, 0x66, sizeof(sentinel));
    MP4VideoTimeCode out = sentinel;
    // movie and video time scales are both 90000; media time 4500 is inside
    // sample 2 which carries the negative composition offset and must reject
    TASK_CHECK(!MP4GetVideoTimeCode(f, 1, 4500, &out));
    TASK_CHECK(std::memcmp(&out, &sentinel, sizeof(out)) == 0);
    MP4Close(f);
    return 0;
}

static int case_unsupported_descriptions()
{
    struct BadDesc { const char* tag; TcDescription desc; };
    const BadDesc bad[] = {
        { "counter",  { 0x20000000u, 25, 1, 25 } },
        { "baddf",    { 0x80000000u, 25000, 1001, 25 } },
        { "badndf",   { 0x00000000u, 24000, 1001, 24 } },
        { "zerots",   { 0x00000000u, 0, 1, 25 } },
    };

    for (uint32_t i = 0; i < 4; ++i) {
        TempFile tmp(bad[i].tag);
        MovBuilder b(1000);
        b.setVideo(1, 1000, { { 2, 40 } }, { { 2, 0 } }, {}, 2);
        b.setTimecode(2, 1000, { bad[i].desc },
            { { 0u, 1000, 1 } }, {});
        TASK_CHECK(b.write(tmp.path));

        MP4FileHandle f = MP4Read(tmp.path.c_str());
        TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);
        MP4VideoTimeCode sentinel;
        std::memset(&sentinel, 0x44 + i, sizeof(sentinel));
        MP4VideoTimeCode out = sentinel;
        TASK_CHECK(!MP4GetVideoTimeCode(f, 1, 10, &out));
        TASK_CHECK(std::memcmp(&out, &sentinel, sizeof(out)) == 0);
        MP4Close(f);
    }
    return 0;
}

static int case_bad_rate_bad_ref_and_requery()
{
    TempFile tmp("rej");
    MovBuilder b(1000);
    // a dwell edit (media rate 0.0) is explicitly out of scope
    b.setVideo(1, 1000, { { 4, 40 } }, { { 4, 0 } },
        { { 40, 0, 0 }, { 120, 0, 1 } }, 4);
    b.setTimecode(2, 1000, { TC_25 }, { { 0u, 1000, 1 } }, {});
    TASK_CHECK(b.write(tmp.path));

    MP4FileHandle f = MP4Read(tmp.path.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);
    MP4VideoTimeCode sentinel;
    std::memset(&sentinel, 0x22, sizeof(sentinel));
    MP4VideoTimeCode out = sentinel;
    TASK_CHECK(!MP4GetVideoTimeCode(f, 1, 10, &out));
    TASK_CHECK(std::memcmp(&out, &sentinel, sizeof(out)) == 0);
    MP4Close(f);

    // a dangling tref target must fail without disturbing later queries
    TempFile tmp2("rej2");
    MovBuilder b2(1000);
    b2.setVideo(1, 1000, { { 4, 40 } }, { { 4, 0 } }, {}, 4);
    b2.setTimecode(2, 1000, { TC_25 }, { { 90000u, 160, 1 } }, {});
    b2.setReferenceTarget(99);
    TASK_CHECK(b2.write(tmp2.path));
    MP4FileHandle g = MP4Read(tmp2.path.c_str());
    TASK_CHECK(g != MP4_INVALID_FILE_HANDLE);
    MP4VideoTimeCode bad = sentinel;
    TASK_CHECK(!MP4GetVideoTimeCode(g, 1, 80, &bad));
    TASK_CHECK(std::memcmp(&bad, &sentinel, sizeof(bad)) == 0);
    MP4Close(g);

    // the same handle style still works on a valid file afterwards
    MP4FileHandle h = MP4Read(tmp.path.c_str());
    TASK_CHECK(h != MP4_INVALID_FILE_HANDLE);
    MP4VideoTimeCode good{};
    TASK_CHECK(!MP4GetVideoTimeCode(h, 1, 10, &good)); // dwell still bad
    MP4Close(h);
    return 0;
}

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------

} // namespace

const struct task_case task_cases[] = {
    { "association", "unique_reference",   case_unique_reference },
    { "association", "missing_reference",  case_missing_reference },
    { "association", "ambiguous_reference", case_ambiguous_reference },

    { "projection", "bframe_display_order", case_bframe_display_order },
    { "projection", "empty_edits_both_tracks", case_empty_edits_both_tracks },
    { "projection", "repeated_reversed_edits", case_repeated_reversed_edits },
    { "projection", "rational_frame_boundaries",
      case_rational_frame_boundaries },

    { "counter", "counter_covers_many_frames",
      case_counter_covers_many_frames },
    { "counter", "description_switch_and_jump",
      case_description_switch_and_jump },
    { "counter", "drop_frame_boundaries", case_drop_frame_boundaries },
    { "counter", "twenty_four_hour_wrap", case_twenty_four_hour_wrap },

    { "reject", "no_coverage_and_empty_edit",
      case_no_coverage_and_empty_edit },
    { "reject", "negative_ctts", case_negative_ctts },
    { "reject", "unsupported_descriptions", case_unsupported_descriptions },
    { "reject", "bad_rate_bad_ref_and_requery",
      case_bad_rate_bad_ref_and_requery },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);

