package org.jcodec.movtool;

import static task_support.Check.check;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.Charset;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

import org.jcodec.common.io.NIOUtils;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.common.model.Packet;
import org.jcodec.common.model.Packet.FrameType;
import org.jcodec.containers.mp4.BoxFactory;
import org.jcodec.containers.mp4.BoxUtil;
import org.jcodec.containers.mp4.Brand;
import org.jcodec.containers.mp4.MP4Util;
import org.jcodec.containers.mp4.MP4Util.Atom;
import org.jcodec.containers.mp4.MP4Util.Movie;
import org.jcodec.containers.mp4.boxes.AliasBox;
import org.jcodec.containers.mp4.boxes.AudioSampleEntry;
import org.jcodec.containers.mp4.boxes.Box;
import org.jcodec.containers.mp4.boxes.ChunkOffsets64Box;
import org.jcodec.containers.mp4.boxes.ChunkOffsetsBox;
import org.jcodec.containers.mp4.boxes.CompositionOffsetsBox;
import org.jcodec.containers.mp4.boxes.DataInfoBox;
import org.jcodec.containers.mp4.boxes.DataRefBox;
import org.jcodec.containers.mp4.boxes.Edit;
import org.jcodec.containers.mp4.boxes.EditListBox;
import org.jcodec.containers.mp4.boxes.FileTypeBox;
import org.jcodec.containers.mp4.boxes.HandlerBox;
import org.jcodec.containers.mp4.boxes.Header;
import org.jcodec.containers.mp4.boxes.MediaBox;
import org.jcodec.containers.mp4.boxes.MediaHeaderBox;
import org.jcodec.containers.mp4.boxes.MediaInfoBox;
import org.jcodec.containers.mp4.boxes.MovieBox;
import org.jcodec.containers.mp4.boxes.MovieHeaderBox;
import org.jcodec.containers.mp4.boxes.NodeBox;
import org.jcodec.containers.mp4.boxes.SampleDescriptionBox;
import org.jcodec.containers.mp4.boxes.SampleEntry;
import org.jcodec.containers.mp4.boxes.SampleSizesBox;
import org.jcodec.containers.mp4.boxes.SampleToChunkBox;
import org.jcodec.containers.mp4.boxes.SampleToChunkBox.SampleToChunkEntry;
import org.jcodec.containers.mp4.boxes.SoundMediaHeaderBox;
import org.jcodec.containers.mp4.boxes.SyncSamplesBox;
import org.jcodec.containers.mp4.boxes.TimeToSampleBox;
import org.jcodec.containers.mp4.boxes.TimeToSampleBox.TimeToSampleEntry;
import org.jcodec.containers.mp4.boxes.TrackHeaderBox;
import org.jcodec.containers.mp4.boxes.TrakBox;
import org.jcodec.containers.mp4.boxes.UrlBox;
import org.jcodec.containers.mp4.demuxer.MP4Demuxer;
import org.jcodec.platform.Platform;

/**
 * Exercises multi data reference flattening.
 *
 * Two source files share an identical physical layout but carry different
 * bytes at the same offsets. A reference movie points one track at both files
 * and switches sample description (and therefore data reference) between
 * chunks, while a second track reuses one of the sources.
 */
public final class MultiSourceFlattenTest {

    // Track 0: 5 samples over 3 chunks, alternating between two descriptions.
    private static final int[] T0_SIZES = { 7, 9, 11, 8, 10 };
    private static final int[] T0_DURS = { 1024, 1024, 2048, 1024, 1024 };
    private static final int[] T0_CHUNK_SAMPLES = { 2, 2, 1 };
    private static final int[] T0_CHUNK_ENTRY = { 1, 2, 1 };
    private static final int T0_TIMESCALE = 44100;

    // Track 1: 4 samples over 2 chunks, always the first description.
    private static final int[] T1_SIZES = { 6, 6, 12, 7 };
    private static final int[] T1_DURS = { 2048, 2048, 2048, 2048 };
    private static final int[] T1_CHUNK_SAMPLES = { 2, 2 };
    private static final int[] T1_CHUNK_ENTRY = { 1, 1 };
    private static final int T1_TIMESCALE = 48000;

    private static final int MOVIE_TIMESCALE = 1000;
    private static final int SRC_A = 1;
    private static final int SRC_B = 2;

    /** Byte filling a sample payload. Different sources produce different bytes. */
    private static byte payloadByte(int track, int sample, int source) {
        return (byte) (0x11 + track * 0x20 + sample * 3 + source * 0x40);
    }

    private static byte[] sampleBytes(int track, int sample, int source) {
        int[] sizes = track == 0 ? T0_SIZES : T1_SIZES;
        byte[] b = new byte[sizes[sample]];
        Arrays.fill(b, payloadByte(track, sample, source));
        return b;
    }

    private static byte[] chunkBytes(int track, int chunk, int source) {
        int[] samples = track == 0 ? T0_CHUNK_SAMPLES : T1_CHUNK_SAMPLES;
        int first = 0;
        for (int i = 0; i < chunk; i++)
            first += samples[i];
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        for (int i = 0; i < samples[chunk]; i++) {
            byte[] b = sampleBytes(track, first + i, source);
            out.write(b, 0, b.length);
        }
        return out.toByteArray();
    }

    private static int chunkCount(int track) {
        return track == 0 ? T0_CHUNK_SAMPLES.length : T1_CHUNK_SAMPLES.length;
    }

    /**
     * Source of a track0 chunk when both data references are in play. Track1
     * always reads the first reference.
     */
    private static int chunkSource(int track) {
        if (track == 1)
            return SRC_A;
        throw new IllegalArgumentException();
    }

    private static int track0ChunkSource(int chunk) {
        // Chunks 0 and 2 use description 1 (source A), chunk 1 uses
        // description 2 (source B).
        return T0_CHUNK_ENTRY[chunk] == 2 ? SRC_B : SRC_A;
    }

    // Physical mdat layout: track0 chunk0, track0 chunk1, track0 chunk2,
    // track1 chunk0, track1 chunk1.
    private static final int[] REGION_TRACK = { 0, 0, 0, 1, 1 };
    private static final int[] REGION_CHUNK = { 0, 1, 2, 0, 1 };

    private static int payloadLength() {
        int len = 0;
        for (int t = 0; t < 2; t++)
            for (int c = 0; c < chunkCount(t); c++)
                len += chunkBytes(t, c, SRC_A).length;
        return len;
    }

    private static byte[] payload(int source) throws IOException {
        byte[] out = new byte[payloadLength()];
        int pos = 0;
        for (int r = 0; r < REGION_TRACK.length; r++) {
            byte[] c = chunkBytes(REGION_TRACK[r], REGION_CHUNK[r], source);
            System.arraycopy(c, 0, out, pos, c.length);
            pos += c.length;
        }
        return out;
    }

    /** Absolute offsets of every chunk, identical in both source files. */
    private static long[][] chunkOffsets(int ftypLen) {
        long base = ftypLen + 8;
        long[] t0 = new long[T0_CHUNK_SAMPLES.length];
        long[] t1 = new long[T1_CHUNK_SAMPLES.length];
        long rel = 0;
        for (int r = 0; r < REGION_TRACK.length; r++) {
            long[] dst = REGION_TRACK[r] == 0 ? t0 : t1;
            dst[REGION_CHUNK[r]] = base + rel;
            rel += chunkBytes(REGION_TRACK[r], REGION_CHUNK[r], SRC_A).length;
        }
        return new long[][] { t0, t1 };
    }

    // ------------------------------------------------------------------
    // Fixture construction
    // ------------------------------------------------------------------

    private static final byte[] MATRIX = { 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 };

    private static int[] matrix() {
        return new int[] { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 };
    }

    private static byte[] boxBytes(Box box) {
        ByteBuffer b = ByteBuffer.allocate(Math.max(box.estimateSize() * 2 + 1024, 4096));
        box.write(b);
        b.flip();
        byte[] out = new byte[b.remaining()];
        b.get(out);
        return out;
    }

    private static byte[] ftypBytes() {
        FileTypeBox ftyp = Brand.MP4.getFileTypeBox();
        return boxBytes(ftyp);
    }

    private static TrakBox buildTrack(int track, File refA, File refB, long[] chunkOffsets) throws IOException {
        int timescale = track == 0 ? T0_TIMESCALE : T1_TIMESCALE;
        int[] sizes = track == 0 ? T0_SIZES : T1_SIZES;
        int[] durs = track == 0 ? T0_DURS : T1_DURS;
        int[] chunkSamples = track == 0 ? T0_CHUNK_SAMPLES : T1_CHUNK_SAMPLES;
        int[] chunkEntry = track == 0 ? T0_CHUNK_ENTRY : T1_CHUNK_ENTRY;

        long totalDur = 0;
        for (int d : durs)
            totalDur += d;

        TrakBox trak = TrakBox.createTrakBox();
        long movieDur = Math.round(totalDur * (double) MOVIE_TIMESCALE / timescale);
        TrackHeaderBox tkhd = TrackHeaderBox.createTrackHeaderBox(track + 1, movieDur, 0, 0, 0, 0, 1f, (short) 0, 0,
                matrix());
        tkhd.setFlags(0xf);
        trak.add(tkhd);

        NodeBox edts = new NodeBox(new Header("edts"));
        List<Edit> edits = new ArrayList<Edit>();
        edits.add(new Edit(MOVIE_TIMESCALE * 60, 0, 1f));
        edts.add(EditListBox.createEditListBox(edits));
        trak.add(edts);

        MediaBox mdia = MediaBox.createMediaBox();
        trak.add(mdia);
        mdia.add(MediaHeaderBox.createMediaHeaderBox(timescale, totalDur, 0, 0, 0, 0));
        mdia.add(HandlerBox.createHandlerBox("mhlr", "soun", "appl", 0, 0));

        MediaInfoBox minf = MediaInfoBox.createMediaInfoBox();
        mdia.add(minf);
        minf.add(SoundMediaHeaderBox.createSoundMediaHeaderBox());
        minf.add(HandlerBox.createHandlerBox("dhlr", "url ", "appl", 0, 0));

        DataInfoBox dinf = DataInfoBox.createDataInfoBox();
        minf.add(dinf);
        DataRefBox dref = DataRefBox.createDataRefBox();
        dinf.add(dref);
        dref.add(UrlBox.createUrlBox("file://" + refA.getCanonicalPath()));
        if (track == 0)
            dref.add(UrlBox.createUrlBox("file://" + refB.getCanonicalPath()));

        NodeBox stbl = new NodeBox(new Header("stbl"));
        minf.add(stbl);

        // A description per data reference used by this track.
        int descCount = track == 0 ? 2 : 1;
        SampleEntry[] entries = new SampleEntry[descCount];
        for (int i = 0; i < descCount; i++) {
            entries[i] = AudioSampleEntry.compressedAudioSampleEntry("mp4a", i + 1, 16, 1, timescale, 0, 0, 0);
        }
        stbl.add(SampleDescriptionBox.createSampleDescriptionBox(entries));

        TimeToSampleEntry[] tts = new TimeToSampleEntry[durs.length];
        for (int i = 0; i < durs.length; i++)
            tts[i] = new TimeToSampleEntry(1, durs[i]);
        stbl.add(TimeToSampleBox.createTimeToSampleBox(tts));

        SampleToChunkEntry[] stsc = new SampleToChunkEntry[chunkSamples.length];
        int first = 1;
        for (int c = 0; c < chunkSamples.length; c++) {
            stsc[c] = new SampleToChunkEntry(first, chunkSamples[c], chunkEntry[c]);
            first += 1;
        }
        stbl.add(SampleToChunkBox.createSampleToChunkBox(stsc));

        stbl.add(SampleSizesBox.createSampleSizesBox2(sizes));

        if (track == 0) {
            stbl.add(ChunkOffsetsBox.createChunkOffsetsBox(chunkOffsets));
            stbl.add(CompositionOffsetsBox
                    .createCompositionOffsetsBox(new CompositionOffsetsBox.Entry[] {
                            new CompositionOffsetsBox.Entry(3, 0), new CompositionOffsetsBox.Entry(2, 500) }));
            stbl.add(SyncSamplesBox.createSyncSamplesBox(new int[] { 1, 4 }));
        } else {
            stbl.add(ChunkOffsets64Box.createChunkOffsets64Box(chunkOffsets));
        }
        return trak;
    }

    private static MovieBox buildMoov(File a, File b) throws IOException {
        byte[] ftyp = ftypBytes();
        long[][] offs = chunkOffsets(ftyp.length);
        MovieBox moov = MovieBox.createMovieBox();
        moov.addFirst(MovieHeaderBox.createMovieHeaderBox(MOVIE_TIMESCALE, MOVIE_TIMESCALE * 60, 1f, 1f, 0, 0,
                matrix(), 3));
        moov.add(buildTrack(0, a, b, offs[0]));
        moov.add(buildTrack(1, a, null, offs[1]));
        return moov;
    }

    private static void writeSource(File f, int source, MovieBox moov) throws IOException {
        byte[] ftyp = ftypBytes();
        byte[] payload = payload(source);
        SeekableByteChannel ch = NIOUtils.writableChannel(f);
        try {
            ch.write(ByteBuffer.wrap(ftyp));
            ByteBuffer mdat = ByteBuffer.allocate(8);
            Header.createHeader("mdat", payload.length + 8).write(mdat);
            mdat.flip();
            ch.write(mdat);
            ch.write(ByteBuffer.wrap(payload));
            ch.write(ByteBuffer.wrap(boxBytes(moov)));
        } finally {
            ch.close();
        }
    }

    private static class Fixture {
        File dir;
        File a;
        File b;
        Movie movie() throws IOException {
            return new Movie(Brand.MP4.getFileTypeBox(), buildMoov(a, b));
        }
    }

    private static Fixture fixture() throws IOException {
        Path dir = Files.createTempDirectory("multi-flatten-");
        Fixture f = new Fixture();
        f.dir = dir.toFile();
        f.a = dir.resolve("sourceA.mp4").toFile();
        f.b = dir.resolve("sourceB.mp4").toFile();
        writeSource(f.a, SRC_A, buildMoov(f.a, f.b));
        writeSource(f.b, SRC_B, buildMoov(f.a, f.b));
        return f;
    }

    private static void cleanup(File dir) {
        if (dir == null)
            return;
        try {
            Files.walk(dir.toPath()).sorted(Comparator.reverseOrder()).forEach(p -> {
                try {
                    Files.deleteIfExists(p);
                } catch (IOException e) {
                    // best effort
                }
            });
        } catch (IOException e) {
            // best effort
        }
    }

    // ------------------------------------------------------------------
    // Expectations
    // ------------------------------------------------------------------

    private static int[] sampleBasePts(int track) {
        int[] durs = track == 0 ? T0_DURS : T1_DURS;
        int[] pts = new int[durs.length];
        int acc = 0;
        for (int i = 0; i < durs.length; i++) {
            pts[i] = acc;
            acc += durs[i];
        }
        return pts;
    }

    private static int cttsOffset(int sample) {
        return sample < 3 ? 0 : 500;
    }

    private static byte[] expectedSample(int track, int sample, int source) {
        return sampleBytes(track, sample, source);
    }

    /** Expected bytes of a chunk in a self-contained output. */
    private static byte[] expectedChunk(int track, int chunk) {
        int source;
        if (track == 0)
            source = track0ChunkSource(chunk);
        else
            source = SRC_A;
        return chunkBytes(track, chunk, source);
    }

    private static long[] offsetsOf(TrakBox trak) {
        ChunkOffsetsBox stco = NodeBox.findFirstPath(trak, ChunkOffsetsBox.class,
                Box.path("mdia.minf.stbl.stco"));
        if (stco != null)
            return stco.getChunkOffsets();
        return NodeBox.findFirstPath(trak, ChunkOffsets64Box.class, Box.path("mdia.minf.stbl.co64")).getChunkOffsets();
    }

    private static SampleSizesBox stszOf(TrakBox trak) {
        return NodeBox.findFirstPath(trak, SampleSizesBox.class, Box.path("mdia.minf.stbl.stsz"));
    }

    private static int[] sizesOf(SampleSizesBox stsz) {
        if (stsz.getDefaultSize() != 0) {
            int[] out = new int[stsz.getCount()];
            Arrays.fill(out, stsz.getDefaultSize());
            return out;
        }
        return stsz.getSizes();
    }

    private static DataRefBox drefOf(TrakBox trak) {
        return NodeBox.findFirstPath(trak, DataRefBox.class, Box.path("mdia.minf.dinf.dref"));
    }

    private static void checkSelfReference(TrakBox trak, String what) {
        DataRefBox dref = drefOf(trak);
        check(dref != null && dref.getBoxes().size() == 1, what + ": expected exactly one data reference");
        Box ref = dref.getBoxes().get(0);
        check(ref instanceof AliasBox && ((AliasBox) ref).isSelfRef(), what + ": expected a self reference");
        for (SampleEntry se : trak.getSampleEntries())
            check(se.getDrefInd() == 1, what + ": sample entry not normalised to self reference");
    }

    private static void readAt(SeekableByteChannel ch, long off, byte[] dst) throws IOException {
        ch.setPosition(off);
        ByteBuffer b = NIOUtils.fetchFromChannel(ch, dst.length);
        check(b.remaining() == dst.length, "short read at " + off);
        b.get(dst);
    }

    private static List<Atom> atoms(SeekableByteChannel ch) throws IOException {
        return MP4Util.getRootAtoms(ch);
    }

    private static Atom find(List<Atom> atoms, String fourcc) {
        for (Atom a : atoms)
            if (fourcc.equals(a.getHeader().getFourcc()))
                return a;
        return null;
    }

    private static Atom findLast(List<Atom> atoms, String fourcc) {
        Atom result = null;
        for (Atom a : atoms)
            if (fourcc.equals(a.getHeader().getFourcc()))
                result = a;
        return result;
    }

    private static void checkSelfContained(File out, String what) throws IOException {
        checkSelfContained(out, what, false);
    }

    private static void checkSelfContained(File out, String what, boolean singleSource) throws IOException {
        SeekableByteChannel ch = NIOUtils.readableChannel(out);
        try {
            List<Atom> atoms = atoms(ch);
            Atom moovAtom = find(atoms, "moov");
            Atom mdatAtom = find(atoms, "mdat");
            check(moovAtom != null, what + ": missing moov");
            check(mdatAtom != null, what + ": missing mdat");
            check(moovAtom.getOffset() < mdatAtom.getOffset(), what + ": moov must precede mdat");
            check(find(atoms, "ftyp") != null, what + ": missing ftyp");

            MovieBox moov = (MovieBox) moovAtom.parseBox(ch);
            TrakBox[] tracks = moov.getTracks();
            check(tracks.length == 2, what + ": expected two tracks");
            long mdatStart = mdatAtom.getOffset() + mdatAtom.getHeader().headerSize();
            long mdatEnd = mdatAtom.getOffset() + mdatAtom.getHeader().getSize();

            for (int t = 0; t < 2; t++) {
                TrakBox trak = tracks[t];
                checkSelfReference(trak, what + " track" + t);
                SampleSizesBox stsz = stszOf(trak);
                int[] sizes = sizesOf(stsz);
                int[] expSizes = expSizes(t);
                check(sizes.length == expSizes.length, what + " track" + t + ": sample count");
                for (int i = 0; i < expSizes.length; i++)
                    check(sizes[i] == expSizes[i], what + " track" + t + ": sample size " + i);

                long[] offs = offsetsOf(trak);
                check(offs.length == chunkCount(t), what + " track" + t + ": chunk count");
                int sampleIdx = 0;
                for (int c = 0; c < offs.length; c++) {
                    byte[] expected = singleSource ? chunkBytes(t, c, SRC_A) : expChunk(t, c);
                    check(offs[c] >= mdatStart && offs[c] + expected.length <= mdatEnd,
                            what + " track" + t + " chunk" + c + ": chunk outside mdat");
                    byte[] actual = new byte[expected.length];
                    readAt(ch, offs[c], actual);
                    check(Arrays.equals(expected, actual), what + " track" + t + " chunk" + c + ": payload");
                    sampleIdx += chunkSamples(t, c);
                }
                check(sampleIdx == expSizes.length, what + " track" + t + ": chunk/sample coverage");

                // Time table, composition offsets, sync samples and edits are preserved.
                TimeToSampleBox stts = NodeBox.findFirstPath(trak, TimeToSampleBox.class,
                        Box.path("mdia.minf.stbl.stts"));
                int[] durs = t == 0 ? T0_DURS : T1_DURS;
                check(stts.getEntries().length == durs.length, what + " track" + t + ": stts entry count");
                for (int i = 0; i < durs.length; i++)
                    check(stts.getEntries()[i].getSampleDuration() == durs[i], what + " track" + t + ": duration " + i);

                CompositionOffsetsBox ctts = NodeBox.findFirstPath(trak, CompositionOffsetsBox.class,
                        Box.path("mdia.minf.stbl.ctts"));
                if (t == 0) {
                    check(ctts != null && ctts.getEntries().length == 2, what + ": ctts missing");
                    check(ctts.getEntries()[0].getCount() == 3 && ctts.getEntries()[0].getOffset() == 0,
                            what + ": ctts entry 0");
                    check(ctts.getEntries()[1].getCount() == 2 && ctts.getEntries()[1].getOffset() == 500,
                            what + ": ctts entry 1");
                } else {
                    check(ctts == null, what + ": unexpected ctts");
                }

                SyncSamplesBox stss = NodeBox.findFirstPath(trak, SyncSamplesBox.class,
                        Box.path("mdia.minf.stbl.stss"));
                if (t == 0) {
                    check(stss != null && Arrays.equals(stss.getSyncSamples(), new int[] { 1, 4 }),
                            what + ": stss not preserved");
                } else {
                    check(stss == null, what + ": unexpected stss");
                }

                List<Edit> edits = trak.getEdits();
                check(edits != null && edits.size() == 1, what + " track" + t + ": edit list");
                check(edits.get(0).getMediaTime() == 0 && edits.get(0).getDuration() == MOVIE_TIMESCALE * 60,
                        what + " track" + t + ": edit list content");

                SampleToChunkBox stsc = NodeBox.findFirstPath(trak, SampleToChunkBox.class,
                        Box.path("mdia.minf.stbl.stsc"));
                check(stsc != null && stsc.getSampleToChunk().length == chunkCount(t),
                        what + " track" + t + ": stsc entry count");
                for (int c = 0; c < chunkCount(t); c++) {
                    check(stsc.getSampleToChunk()[c].getCount() == chunkSamples(t, c),
                            what + " track" + t + ": stsc sample count " + c);
                    int expectedEntry = singleSource ? 1 : chunkEntry(t, c);
                    check(stsc.getSampleToChunk()[c].getEntry() == expectedEntry,
                            what + " track" + t + ": stsc description " + c);
                }
            }
        } finally {
            ch.close();
        }
    }

    private static int[] expSizes(int track) {
        return track == 0 ? T0_SIZES : T1_SIZES;
    }

    private static int chunkSamples(int track, int chunk) {
        return (track == 0 ? T0_CHUNK_SAMPLES : T1_CHUNK_SAMPLES)[chunk];
    }

    private static int chunkEntry(int track, int chunk) {
        return (track == 0 ? T0_CHUNK_ENTRY : T1_CHUNK_ENTRY)[chunk];
    }

    private static byte[] expChunk(int track, int chunk) {
        int source = track == 0 ? track0ChunkSource(chunk) : SRC_A;
        return chunkBytes(track, chunk, source);
    }

    /** Source file a given sample of a track comes from in the fixture. */
    private static int sampleSource(int track, int sample) {
        if (track == 1)
            return SRC_A;
        // Track0: samples 0,1 bind to description 1 (source A), samples 2,3 to
        // description 2 (source B), sample 4 back to description 1.
        return sample <= 1 || sample == 4 ? SRC_A : SRC_B;
    }

    /** Verifies the demuxed samples of a flattened file. */
    private static void checkDemux(File f) throws IOException {
        checkDemux(f, false);
    }

    private static void checkDemux(File f, boolean singleSource) throws IOException {
        SeekableByteChannel ch = NIOUtils.readableChannel(f);
        MP4Demuxer demux = MP4Demuxer.createRawMP4Demuxer(ch);
        try {
            List<org.jcodec.common.SeekableDemuxerTrack> tracks = demux.getTracks();
            check(tracks.size() == 2, "demux track count");
            for (int t = 0; t < 2; t++) {
                int[] sizes = t == 0 ? T0_SIZES : T1_SIZES;
                int[] durs = t == 0 ? T0_DURS : T1_DURS;
                int[] base = sampleBasePts(t);
                for (int i = 0; i < sizes.length; i++) {
                    Packet p = tracks.get(t).nextFrame();
                    check(p != null, "missing sample " + t + "/" + i);
                    byte[] actual = new byte[p.getData().remaining()];
                    p.getData().get(actual);
                    byte[] expected = expectedSample(t, i, singleSource ? SRC_A : sampleSource(t, i));
                    check(Arrays.equals(expected, actual), "demux payload track" + t + " sample" + i);
                    long expPts = base[i] + (t == 0 ? cttsOffset(i) : 0);
                    check(p.getPts() == expPts, "demux pts track" + t + " sample" + i);
                    check(p.getDuration() == durs[i], "demux duration track" + t + " sample" + i);
                    boolean key = t == 0 ? (i == 0 || i == 3) : true;
                    check((p.getFrameType() == FrameType.KEY) == key,
                            "demux sync flag track" + t + " sample" + i);
                }
                check(tracks.get(t).nextFrame() == null, "extra sample track" + t);
            }
        } finally {
            ch.close();
        }
    }

    private static byte[] fileBytes(File f) throws IOException {
        return Files.readAllBytes(f.toPath());
    }

    private static long atomOffset(byte[] file, String fourcc) throws IOException {
        long off = 0;
        while (off + 8 <= file.length) {
            long size = ((long) (file[(int) off] & 0xff) << 24) | ((file[(int) off + 1] & 0xff) << 16)
                    | ((file[(int) off + 2] & 0xff) << 8) | (file[(int) off + 3] & 0xff);
            String fc = new String(file, (int) off + 4, 4, Charset.forName("US-ASCII"));
            int hdr = 8;
            if (size == 1) {
                size = ByteBuffer.wrap(file, (int) off + 8, 8).getLong();
                hdr = 16;
            }
            if (fourcc.equals(fc))
                return off;
            if (size <= 0)
                break;
            off += size;
        }
        return -1;
    }

    // ------------------------------------------------------------------
    // Tests
    // ------------------------------------------------------------------

    public static void route() throws Exception {
        Fixture f = fixture();
        File out = new File(f.dir, "self.mp4");
        try {
            byte[] sourceA = fileBytes(f.a);
            byte[] sourceB = fileBytes(f.b);

            new Flatten().flatten(f.movie(), out);

            check(out.isFile() && out.length() > 0, "route: output not produced");
            checkSelfContained(out, "route/flatten");
            checkDemux(out);

            check(Arrays.equals(sourceA, fileBytes(f.a)), "route: source A modified");
            check(Arrays.equals(sourceB, fileBytes(f.b)), "route: source B modified");

            // The self contained output must be readable with every source
            // gone.
            check(f.a.delete(), "route: could not delete source A");
            check(f.b.delete(), "route: could not delete source B");
            checkDemux(out);
            checkSelfContained(out, "route/flatten after source removal");
        } finally {
            cleanup(f.dir);
        }

        // flattenChannel keeps the caller's channel open, writes a front moov
        // and interleaves tracks.
        Fixture g = fixture();
        try {
            MemChannel mem = new MemChannel();
            new Flatten().flattenChannel(g.movie(), mem);
            check(mem.isOpen(), "route: caller channel was closed");
            File out2 = new File(g.dir, "channel.mp4");
            Files.write(out2.toPath(), mem.toByteArray());
            checkSelfContained(out2, "route/flattenChannel");
            checkDemux(out2);
        } finally {
            cleanup(g.dir);
        }
    }

    public static void onTop() throws Exception {
        Fixture f = fixture();
        try {
            byte[] before = fileBytes(f.a);
            long oldMoov = atomOffset(before, "moov");
            check(oldMoov >= 0, "onTop: fixture has no moov");

            new Flatten().flattenOnTop(f.movie(), f.a);

            byte[] after = fileBytes(f.a);
            check(after.length > before.length, "onTop: external chunks were not appended");

            // Only the old moov's type field may change in the original prefix.
            for (int i = 0; i < before.length; i++) {
                if (i >= oldMoov + 4 && i < oldMoov + 8)
                    continue;
                check(after[i] == before[i], "onTop: original byte " + i + " changed");
            }
            check(new String(after, (int) oldMoov + 4, 4, Charset.forName("US-ASCII")).equals("free"),
                    "onTop: old moov type not turned into free");

            // Re-read the appended file; external source can now vanish.
            SeekableByteChannel ch = NIOUtils.readableChannel(f.a);
            List<Atom> atoms = atoms(ch);
            Atom moovAtom = findLast(atoms, "moov");
            check(moovAtom != null, "onTop: new moov missing");
            check(moovAtom.getOffset() > before.length, "onTop: new moov not appended");
            MovieBox moov = (MovieBox) moovAtom.parseBox(ch);
            TrakBox[] tracks = moov.getTracks();
            check(tracks.length == 2, "onTop: track count");

            long oldChunk0 = offsetsOf(T0ChunkHolder(moov))[0];
            check(oldChunk0 < before.length, "onTop: local chunk moved");

            Atom mdatAtom = findLast(atoms, "mdat");
            check(mdatAtom != null, "onTop: new mdat missing");
            check(mdatAtom.getOffset() >= before.length, "onTop: new mdat not appended");
            long newMdatBody = new File(f.a.getPath()).length();
            long newMdatSize = mdatAtom.getHeader().getSize();
            long externalBytes = chunkBytes(0, 1, SRC_B).length;
            check(newMdatSize == externalBytes + 16,
                    "onTop: new mdat should hold only external payload (was " + newMdatSize + ")");
            check(newMdatBody > 0, "onTop: empty output");

            // Local chunks keep their original offsets, external chunks point
            // at the appended mdat.
            long[] t0Offs = offsetsOf(tracks[0]);
            long base = f.dir.listFiles() == null ? 0 : chunkOffsets(ftypBytes().length)[0][0];
            check(t0Offs[0] == base, "onTop: local chunk 0 offset");
            check(t0Offs[2] == chunkOffsets(ftypBytes().length)[0][2], "onTop: local chunk 2 offset");
            check(t0Offs[1] >= before.length, "onTop: external chunk not appended");
            ch.close();

            // External source removed: the file must still demux completely.
            check(f.b.delete(), "onTop: could not delete external source");
            checkDemux(f.a);
        } finally {
            cleanup(f.dir);
        }
    }

    private static TrakBox T0ChunkHolder(MovieBox moov) {
        return moov.getTracks()[0];
    }

    public static void processor() throws Exception {
        Fixture f = fixture();
        try {
            // Self contained flatten with a length changing processor on track0.
            final RecordingProcessor proc = new RecordingProcessor();
            Flatten flatten = new Flatten();
            Movie movie = f.movie();
            flatten.setSampleProcessor(movie.getMoov().getTracks()[0], proc);
            File out = new File(f.dir, "proc.mp4");
            flatten.flatten(movie, out);

            check(proc.calls == T0_SIZES.length, "processor: called " + proc.calls + " times, expected "
                    + T0_SIZES.length);
            check(proc.ptsMonotonic, "processor: samples not delivered in original order");
            checkSelfContainedProcessed(out);
            checkProcessedDemux(out, null);

            // On top: local chunks must be processed and appended too.
            Fixture g = fixture();
            try {
                RecordingProcessor proc2 = new RecordingProcessor();
                Flatten flatten2 = new Flatten();
                Movie movie2 = g.movie();
                flatten2.setSampleProcessor(movie2.getMoov().getTracks()[0], proc2);
                byte[] before = fileBytes(g.a);
                flatten2.flattenOnTop(movie2, g.a);
                byte[] after = fileBytes(g.a);

                check(proc2.calls == T0_SIZES.length, "processor/onTop: called " + proc2.calls + " times");
                check(proc2.ptsMonotonic, "processor/onTop: out of order");

                SeekableByteChannel ch = NIOUtils.readableChannel(g.a);
                List<Atom> atoms = atoms(ch);
                Atom mdatAtom = findLast(atoms, "mdat");
                check(mdatAtom != null, "processor/onTop: missing mdat");
                check(mdatAtom.getOffset() >= before.length, "processor/onTop: external mdat not appended");
                long mdatStart = mdatAtom.getOffset() + mdatAtom.getHeader().headerSize();
                long mdatEnd = mdatAtom.getOffset() + mdatAtom.getHeader().getSize();

                MovieBox moov = (MovieBox) findLast(atoms, "moov").parseBox(ch);
                TrakBox[] tracks = moov.getTracks();
                long[] t0Offs = offsetsOf(tracks[0]);
                // All track0 chunks were processed and appended.
                for (int c = 0; c < t0Offs.length; c++)
                    check(t0Offs[c] >= before.length && t0Offs[c] >= mdatStart && t0Offs[c] < mdatEnd,
                            "processor/onTop: track0 chunk " + c + " not appended");
                // Track1 has no processor and stays in place.
                long[] t1Offs = offsetsOf(tracks[1]);
                long[][] orig = chunkOffsets(ftypBytes().length);
                for (int c = 0; c < t1Offs.length; c++)
                    check(t1Offs[c] == orig[1][c], "processor/onTop: unprocessed track moved");

                long t0Bytes = 0;
                for (int i = 0; i < T0_SIZES.length; i++)
                    t0Bytes += T0_SIZES[i] + 1 + i;
                // The mdat placeholder uses an exact 16 byte long form header,
                // so the body is exactly the processed track0 samples.
                check(mdatEnd - mdatStart == t0Bytes,
                        "processor/onTop: appended region holds exactly the processed track0 samples (" +
                                (mdatEnd - mdatStart) + " vs " + t0Bytes + ")");
                ch.close();

                checkProcessedDemux(g.a, before);

                // Both local and external source payloads must appear once,
                // after processing.
                byte[] all = fileBytes(g.a);
                for (int i = 0; i < T0_SIZES.length; i++) {
                    int source = sampleSource(0, i);
                    check(countOf(all, processedSample(i, source)) == 1,
                            "processor/onTop: processed sample " + i + " not present exactly once");
                }
                // Unprocessed payloads are not appended: the raw local bytes
                // survive only inside the untouched original prefix (which is
                // not part of any live track any more), and external bytes
                // were never copied unprocessed.
                byte[] localPrefix = Arrays.copyOf(all, before.length);
                byte[] appended = Arrays.copyOfRange(all, before.length, all.length);
                check(countOf(localPrefix, sampleBytes(0, 0, SRC_A)) == 1,
                        "processor/onTop: original local prefix lost its payload");
                // Every track0 chunk in the appended region is the processed
                // form only. Asserting that the appended bytes are exactly the
                // concatenation of the processed chunks proves that local
                // chunks were not skipped wholesale and external bytes were
                // not appended raw.
                java.io.ByteArrayOutputStream expectedAppended = new java.io.ByteArrayOutputStream();
                for (int c = 0; c < T0_CHUNK_SAMPLES.length; c++) {
                    byte[] processed = processedChunk(c);
                    expectedAppended.write(processed, 0, processed.length);
                }
                byte[] expected = expectedAppended.toByteArray();
                byte[] mdatBody = Arrays.copyOfRange(all, (int) mdatStart, (int) mdatEnd);
                check(mdatBody.length == expected.length,
                        "processor/onTop: appended length " + mdatBody.length + " vs " + expected.length);
                check(Arrays.equals(mdatBody, expected),
                        "processor/onTop: appended data is not exactly the processed samples");
            } finally {
                cleanup(g.dir);
            }
        } finally {
            cleanup(f.dir);
        }
    }

    private static final class RecordingProcessor implements Flatten.SampleProcessor {
        int calls;
        double lastPts = -1;
        boolean ptsMonotonic = true;
        final List<Double> pts = new ArrayList<Double>();

        @Override
        public ByteBuffer processSample(ByteBuffer src, double pts, double duration) throws IOException {
            if (pts < lastPts)
                ptsMonotonic = false;
            lastPts = pts;
            this.pts.add(pts);
            byte[] orig = new byte[src.remaining()];
            src.duplicate().get(orig);
            byte[] result = new byte[orig.length + 1 + calls];
            result[0] = 0x5A;
            System.arraycopy(orig, 0, result, 1, orig.length);
            for (int i = 0; i < calls; i++)
                result[1 + orig.length + i] = (byte) 0xA5;
            calls++;
            return ByteBuffer.wrap(result);
        }
    }

    private static boolean containsProcessed(byte[] file, int sample, int source) {
        byte[] payload = processedSample(sample, source);
        return countOf(file, payload) >= 1;
    }

    private static byte[] processedSample(int sample, int source) {
        byte[] orig = sampleBytes(0, sample, source);
        byte[] result = new byte[orig.length + 1 + sample];
        result[0] = 0x5A;
        System.arraycopy(orig, 0, result, 1, orig.length);
        for (int i = 0; i < sample; i++)
            result[1 + orig.length + i] = (byte) 0xA5;
        return result;
    }

    /** Concatenation of the processed samples that make up track0 chunk c. */
    private static byte[] processedChunk(int chunk) {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        int first = 0;
        for (int c = 0; c < chunk; c++)
            first += T0_CHUNK_SAMPLES[c];
        for (int i = 0; i < T0_CHUNK_SAMPLES[chunk]; i++) {
            int source = sampleSource(0, first + i);
            byte[] b = processedSample(first + i, source);
            out.write(b, 0, b.length);
        }
        return out.toByteArray();
    }

    private static int countOf(byte[] haystack, byte[] needle) {
        int count = 0;
        outer: for (int i = 0; i + needle.length <= haystack.length; i++) {
            for (int j = 0; j < needle.length; j++) {
                if (haystack[i + j] != needle[j])
                    continue outer;
            }
            count++;
        }
        return count;
    }

    /** Self-contained output from the processor test: check stsz matches the transformed sizes. */
    private static void checkSelfContainedProcessed(File out) throws IOException {
        SeekableByteChannel ch = NIOUtils.readableChannel(out);
        try {
            List<Atom> atoms = atoms(ch);
            MovieBox moov = (MovieBox) find(atoms, "moov").parseBox(ch);
            TrakBox trak = moov.getTracks()[0];
            SampleSizesBox stsz = stszOf(trak);
            int[] sizes = sizesOf(stsz);
            check(sizes.length == T0_SIZES.length, "processor: sample count");
            for (int i = 0; i < sizes.length; i++)
                check(sizes[i] == T0_SIZES[i] + 1 + i, "processor: output size " + i + " = " + sizes[i]);
            checkSelfReference(trak, "processor");
            // Track1 is untouched and still has its original sizes.
            TrakBox t1 = moov.getTracks()[1];
            check(Arrays.equals(sizesOf(stszOf(t1)), T1_SIZES), "processor: untouched track sizes");
        } finally {
            ch.close();
        }
    }

    private static void checkProcessedDemux(File f, byte[] onTopBefore) throws IOException {
        SeekableByteChannel ch = NIOUtils.readableChannel(f);
        MP4Demuxer demux = MP4Demuxer.createRawMP4Demuxer(ch);
        try {
            List<org.jcodec.common.SeekableDemuxerTrack> tracks = demux.getTracks();
            check(tracks.size() == 2, "processor: demux track count");
            // Track0: every sample processed once.
            for (int i = 0; i < T0_SIZES.length; i++) {
                Packet p = tracks.get(0).nextFrame();
                check(p != null, "processor: missing processed sample " + i);
                byte[] actual = new byte[p.getData().remaining()];
                p.getData().get(actual);
                int source = sampleSource(0, i);
                check(Arrays.equals(processedSample(i, source), actual),
                        "processor: processed payload " + i);
                check(p.getPts() == sampleBasePts(0)[i] + cttsOffset(i), "processor: pts " + i);
                check(p.getDuration() == T0_DURS[i], "processor: duration " + i);
            }
            check(tracks.get(0).nextFrame() == null, "processor: track0 extra sample");
            for (int i = 0; i < T1_SIZES.length; i++) {
                Packet p = tracks.get(1).nextFrame();
                check(p != null, "processor: missing track1 sample " + i);
                byte[] actual = new byte[p.getData().remaining()];
                p.getData().get(actual);
                check(Arrays.equals(sampleBytes(1, i, SRC_A), actual), "processor: track1 payload " + i);
                check(p.getPts() == sampleBasePts(1)[i], "processor: track1 pts " + i);
            }
            check(tracks.get(1).nextFrame() == null, "processor: track1 extra sample");
        } finally {
            ch.close();
        }
    }

    public static void rejectCompat() throws Exception {
        // ---- Positive: single source and AliasBox still work. ----
        Fixture single = fixture();
        try {
            Movie movie = single.movie();
            MovieBox moov = movie.getMoov();
            TrakBox t0 = moov.getTracks()[0];
            DataRefBox dref = drefOf(t0);
            dref.getBoxes().remove(1); // keep only source A
            for (SampleEntry se : t0.getSampleEntries())
                se.setDrefInd((short) 1);
            SampleToChunkBox stsc = NodeBox.findFirstPath(t0, SampleToChunkBox.class,
                    Box.path("mdia.minf.stbl.stsc"));
            for (SampleToChunkEntry e : stsc.getSampleToChunk())
                e.setEntry(1);
            File out = new File(single.dir, "single.mp4");
            new Flatten().flatten(movie, out);
            checkSelfContained(out, "rejectCompat/single", true);
            checkDemux(out, true);

            // AliasBox data reference resolving to source A.
            AliasBox alias = aliasFor(single.a);
            check(new Flatten().resolveDataRef(alias).getCanonicalPath().equals(single.a.getCanonicalPath()),
                    "rejectCompat: alias did not resolve");
            Movie aliasMovie = single.movie();
            TrakBox aliasTrak = aliasMovie.getMoov().getTracks()[1];
            drefOf(aliasTrak).getBoxes().set(0, alias);
            File aliasOut = new File(single.dir, "alias.mp4");
            new Flatten().flatten(aliasMovie, aliasOut);
            check(aliasOut.isFile() && aliasOut.length() > 0, "rejectCompat: alias flatten failed");
        } finally {
            cleanup(single.dir);
        }

        // ---- Rejections happen before the output is touched. ----
        reject("missing stts", new Mutation() {
            public void apply(MovieBox moov) {
                stbl(moov).removeChildren(new String[] { "stts" });
            }
        });
        reject("missing stsc", new Mutation() {
            public void apply(MovieBox moov) {
                stbl(moov).removeChildren(new String[] { "stsc" });
            }
        });
        reject("missing stsz", new Mutation() {
            public void apply(MovieBox moov) {
                stbl(moov).removeChildren(new String[] { "stsz" });
            }
        });
        reject("missing chunk offsets", new Mutation() {
            public void apply(MovieBox moov) {
                stbl(moov).removeChildren(new String[] { "stco", "co64" });
            }
        });
        reject("missing sample descriptions", new Mutation() {
            public void apply(MovieBox moov) {
                stbl(moov).removeChildren(new String[] { "stsd" });
            }
        });
        reject("missing data references", new Mutation() {
            public void apply(MovieBox moov) {
                NodeBox.findFirstPath(moov.getTracks()[0], DataInfoBox.class, Box.path("mdia.minf.dinf"))
                        .removeChildren(new String[] { "dref" });
            }
        });
        reject("description index out of range", new Mutation() {
            public void apply(MovieBox moov) {
                SampleToChunkBox stsc = NodeBox.findFirstPath(moov.getTracks()[0], SampleToChunkBox.class,
                        Box.path("mdia.minf.stbl.stsc"));
                for (SampleToChunkEntry e : stsc.getSampleToChunk())
                    e.setEntry(9);
            }
        });
        reject("data reference index out of range", new Mutation() {
            public void apply(MovieBox moov) {
                moov.getTracks()[0].getSampleEntries()[1].setDrefInd((short) 7);
            }
        });
        reject("sample count mismatch stsz/stsc", new Mutation() {
            public void apply(MovieBox moov) {
                stszOf(moov.getTracks()[0]).setCount(T0_SIZES.length + 1);
            }
        });
        reject("sample count mismatch stts/stsz", new Mutation() {
            public void apply(MovieBox moov) {
                TrakBox t = moov.getTracks()[0];
                NodeBox stbl = stbl(moov);
                stbl.removeChildren(new String[] { "stts" });
                stbl.add(TimeToSampleBox.createTimeToSampleBox(
                        new TimeToSampleEntry[] { new TimeToSampleEntry(2, 1024) }));
            }
        });
        reject("stsc first chunk past end", new Mutation() {
            public void apply(MovieBox moov) {
                SampleToChunkBox stsc = NodeBox.findFirstPath(moov.getTracks()[0], SampleToChunkBox.class,
                        Box.path("mdia.minf.stbl.stsc"));
                stsc.getSampleToChunk()[0].setFirst(99);
            }
        });
        reject("network data reference", new Mutation() {
            public void apply(MovieBox moov) throws IOException {
                drefOf(moov.getTracks()[0]).getBoxes().set(1,
                        UrlBox.createUrlBox("http://example.com/remote.mp4"));
            }
        });
        reject("relative file data reference", new Mutation() {
            public void apply(MovieBox moov) throws IOException {
                drefOf(moov.getTracks()[0]).getBoxes().set(1, UrlBox.createUrlBox("file://relative.mp4"));
            }
        });
        reject("self data reference", new Mutation() {
            public void apply(MovieBox moov) {
                UrlBox url = UrlBox.createUrlBox("file:///tmp/x.mp4");
                url.setFlags(1);
                drefOf(moov.getTracks()[0]).getBoxes().set(1, url);
            }
        });
        reject("unsupported data reference type", new Mutation() {
            public void apply(MovieBox moov) {
                drefOf(moov.getTracks()[0]).getBoxes().set(1, new OtherRefBox());
            }
        });
        reject("chunk range past end of source", new Mutation() {
            public void apply(MovieBox moov) {
                ChunkOffsetsBox stco = (ChunkOffsetsBox) NodeBox.findFirstPath(moov.getTracks()[0],
                        ChunkOffsetsBox.class, Box.path("mdia.minf.stbl.stco"));
                long[] offs = stco.getChunkOffsets();
                offs[0] = 10_000_000L;
                stco.setChunkOffsets(offs);
            }
        });
        reject("truncated source", new Mutation() {
            public void apply(MovieBox moov) {
                // no-op: the special step truncates source B
            }
        }, new Special() {
            public void run(File a, File b) throws IOException {
                Files.write(b.toPath(), new byte[10]);
            }
        });

        // The same validation applies when flattening on top: the target must
        // be byte identical after a rejection.
        Fixture f = fixture();
        try {
            byte[] before = fileBytes(f.a);
            Movie movie = f.movie();
            stbl(movie.getMoov()).removeChildren(new String[] { "stsz" });
            try {
                new Flatten().flattenOnTop(movie, f.a);
                check(false, "rejectCompat/onTop: broken movie accepted");
            } catch (IOException expected) {
                check(true, "rejectCompat/onTop: rejected");
            }
            check(Arrays.equals(before, fileBytes(f.a)), "rejectCompat/onTop: target modified on rejection");
            long moovOff = atomOffset(before, "moov");
            byte[] now = fileBytes(f.a);
            check(new String(now, (int) moovOff + 4, 4, Charset.forName("US-ASCII")).equals("moov"),
                    "rejectCompat/onTop: old moov was freed before validation");
        } finally {
            cleanup(f.dir);
        }
    }

    private interface Mutation {
        void apply(MovieBox moov) throws IOException;
    }

    /** A well formed but unsupported data reference (a non-leaf, non-url box). */
    private static final class OtherRefBox extends org.jcodec.containers.mp4.boxes.FullBox {
        OtherRefBox() {
            super(Header.createHeader("cios", 0));
        }

        @Override
        public void parse(ByteBuffer input) {
            super.parse(input);
        }

        @Override
        protected void doWrite(ByteBuffer out) {
            super.doWrite(out);
        }

        @Override
        public int estimateSize() {
            return 12;
        }
    }

    private interface Special {
        void run(File a, File b) throws IOException;
    }

    private static NodeBox stbl(MovieBox moov) {
        return NodeBox.findFirstPath(moov.getTracks()[0], NodeBox.class, Box.path("mdia.minf.stbl"));
    }

    private static void reject(String name, Mutation mutation) throws IOException {
        reject(name, mutation, null);
    }

    /**
     * Applies a malformed mutation and asserts the flatten call is rejected
     * before the output file is created.
     */
    private static void reject(String name, Mutation mutation, Special special) throws IOException {
        Fixture f = fixture();
        try {
            if (special != null)
                special.run(f.a, f.b);
            Movie movie = f.movie();
            mutation.apply(movie.getMoov());
            File out = new File(f.dir, "rejected.mp4");
            try {
                new Flatten().flatten(movie, out);
                check(false, "rejectCompat: " + name + " accepted");
            } catch (IOException e) {
                check(true, "rejectCompat: " + name + " rejected (" + e.getMessage() + ")");
            } catch (IllegalArgumentException e) {
                // e.g. removing dref makes the movie no longer a pure reference
                check(true, "rejectCompat: " + name + " rejected (" + e.getMessage() + ")");
            }
            check(!out.exists(), "rejectCompat: " + name + " produced output");
        } finally {
            cleanup(f.dir);
        }
    }

    /** Builds an alias box with a UNIX absolute path extra field. */
    private static AliasBox aliasFor(File target) throws IOException {
        String path = target.getCanonicalPath();
        byte[] data = path.substring(1).getBytes(Charset.forName("UTF-8"));
        ByteBuffer b = ByteBuffer.allocate(512);
        b.putInt(0); // version + flags
        b.put("alis".getBytes(Charset.forName("US-ASCII")));
        b.putShort((short) 0); // record size
        b.putShort((short) 0); // version
        b.putShort((short) 0); // kind
        b.put((byte) 0);
        b.put(new byte[27]); // volume name
        b.putInt(0);
        b.putShort((short) 0);
        b.putShort((short) 0);
        b.putInt(0);
        b.put((byte) 0);
        b.put(new byte[63]); // file name
        b.putInt(0);
        b.putInt(0);
        b.put("    ".getBytes(Charset.forName("US-ASCII")));
        b.put("    ".getBytes(Charset.forName("US-ASCII")));
        b.putShort((short) 0);
        b.putShort((short) 0);
        b.putInt(0);
        b.putShort((short) 0);
        b.put(new byte[10]);
        b.putShort((short) AliasBox.UNIXAbsolutePath);
        b.putShort((short) data.length);
        b.put(data);
        if ((data.length & 1) != 0)
            b.put((byte) 0);
        b.putShort((short) -1);
        b.putShort((short) 0);
        b.flip();
        AliasBox box = new AliasBox(Header.createHeader("alis", 0));
        box.parse(b);
        return box;
    }

    /** Minimal in-memory seekable channel used to exercise flattenChannel ownership. */
    private static final class MemChannel implements SeekableByteChannel {
        private byte[] data = new byte[512];
        private int size;
        private int pos;
        private boolean open = true;

        @Override
        public int read(ByteBuffer dst) {
            if (pos >= size)
                return -1;
            int n = Math.min(dst.remaining(), size - pos);
            dst.put(data, pos, n);
            pos += n;
            return n;
        }

        @Override
        public int write(ByteBuffer src) {
            int n = src.remaining();
            ensure(size + n);
            src.get(data, pos, n);
            pos += n;
            size = Math.max(size, pos);
            return n;
        }

        private void ensure(int cap) {
            if (cap <= data.length)
                return;
            int len = data.length;
            while (len < cap)
                len *= 2;
            data = Arrays.copyOf(data, len);
        }

        @Override
        public long position() {
            return pos;
        }

        @Override
        public SeekableByteChannel setPosition(long newPosition) {
            pos = (int) newPosition;
            return this;
        }

        @Override
        public long size() {
            return size;
        }

        @Override
        public SeekableByteChannel truncate(long s) {
            size = (int) s;
            if (pos > size)
                pos = size;
            return this;
        }

        @Override
        public boolean isOpen() {
            return open;
        }

        @Override
        public void close() {
            open = false;
        }

        byte[] toByteArray() {
            return Arrays.copyOf(data, size);
        }
    }
}
