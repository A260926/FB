package org.jcodec.movtool;

import static org.jcodec.common.io.NIOUtils.readableChannel;
import static org.jcodec.common.io.NIOUtils.writableChannel;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;

import org.jcodec.common.io.FileChannelWrapper;
import org.jcodec.common.io.NIOUtils;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.containers.mp4.Chunk;
import org.jcodec.containers.mp4.ChunkReader;
import org.jcodec.containers.mp4.ChunkWriter;
import org.jcodec.containers.mp4.MP4Util;
import org.jcodec.containers.mp4.MP4Util.Atom;
import org.jcodec.containers.mp4.MP4Util.Movie;
import org.jcodec.containers.mp4.boxes.AliasBox;
import org.jcodec.containers.mp4.boxes.Box;
import org.jcodec.containers.mp4.boxes.ChunkOffsets64Box;
import org.jcodec.containers.mp4.boxes.ChunkOffsetsBox;
import org.jcodec.containers.mp4.boxes.DataRefBox;
import org.jcodec.containers.mp4.boxes.Header;
import org.jcodec.containers.mp4.boxes.MovieBox;
import org.jcodec.containers.mp4.boxes.NodeBox;
import org.jcodec.containers.mp4.boxes.SampleDescriptionBox;
import org.jcodec.containers.mp4.boxes.SampleEntry;
import org.jcodec.containers.mp4.boxes.SampleSizesBox;
import org.jcodec.containers.mp4.boxes.SampleToChunkBox;
import org.jcodec.containers.mp4.boxes.SampleToChunkBox.SampleToChunkEntry;
import org.jcodec.containers.mp4.boxes.TimeToSampleBox;
import org.jcodec.containers.mp4.boxes.TimeToSampleBox.TimeToSampleEntry;
import org.jcodec.containers.mp4.boxes.TrakBox;
import org.jcodec.containers.mp4.boxes.UrlBox;
import org.jcodec.platform.Platform;

/**
 * This class is part of JCodec ( www.jcodec.org ) This software is distributed
 * under FreeBSD License
 *
 * Self contained movie creator.
 *
 * <p>
 * A track may spread its chunks over more than one data reference. Every chunk
 * selects a sample description through <code>stsc</code>, and the description
 * selects a data reference through its
 * <code>data_reference_index</code>. Both indices are one based. All
 * references are resolved before anything is written; the resulting plan is
 * used while the chunks are streamed across, and only once every byte has been
 * read are the data references collapsed to a single self reference.
 * </p>
 *
 * @author The JCodec project
 *
 */
public class Flatten {
    public static interface SampleProcessor {
        ByteBuffer processSample(ByteBuffer src, double pts, double duration) throws IOException;
    }

    public static void main1(String[] args) throws Exception {
        if (args.length < 2) {
            System.out.println("Syntax: self <ref movie> <out movie>");
            System.exit(-1);
        }
        File outFile = new File(args[1]);
        Platform.deleteFile(outFile);
        SeekableByteChannel input = null;
        try {
            input = readableChannel(new File(args[0]));
            Movie movie = MP4Util.parseFullMovieChannel(input);
            new Flatten().flatten(movie, outFile);
        } finally {
            if (input != null)
                input.close();
        }
    }

    public List<ProgressListener> listeners;
    private Map<TrakBox, SampleProcessor> sampleProcessors = new HashMap<TrakBox, SampleProcessor>();

    public Flatten() {
        this.listeners = new ArrayList<Flatten.ProgressListener>();
    }

    public interface ProgressListener {
        public void trigger(int progress);
    }

    public void addProgressListener(ProgressListener listener) {
        this.listeners.add(listener);
    }

    public boolean setSampleProcessor(TrakBox trak, SampleProcessor processor) {
        this.sampleProcessors.put(trak, processor);
        return true;
    }

    /**
     * Resolved, validated description of where every chunk of every track
     * lives. This is computed before the movie or the output file is touched,
     * so that malformed input is rejected without leaving a half written
     * result behind.
     */
    private static class Plan {
        TrackPlan[] tracks;
    }

    private static class TrackPlan {
        /** Resolved file for every entry of the dref table, index 0 == entry 1. */
        File[] drefFiles;
        /** True when the reference points at the file we are flattening on top of. */
        boolean[] local;
        /** Channel to read every dref from; null means "leave in place". */
        SeekableByteChannel[] channels;
        boolean hasProcessor;
    }

    /**
     * Per-run registry of the source channels. Sources may be shared between
     * tracks; one channel is opened per canonical file and every one of them is
     * closed when the operation completes, successfully or not.
     */
    private static class Sources {
        private Map<String, SeekableByteChannel> channels = new HashMap<String, SeekableByteChannel>();

        SeekableByteChannel channelFor(File file) throws IOException {
            String key = file.getCanonicalPath();
            SeekableByteChannel ch = channels.get(key);
            if (ch == null) {
                ch = NIOUtils.readableChannel(file);
                channels.put(key, ch);
            }
            return ch;
        }

        void closeAll() {
            for (SeekableByteChannel ch : channels.values()) {
                NIOUtils.closeQuietly(ch);
            }
            channels.clear();
        }
    }

    public void flattenChannel(Movie movie, SeekableByteChannel out) throws IOException {
        MovieBox moov = movie.getMoov();
        if (!moov.isPureRefMovie())
            throw new IllegalArgumentException("movie should be reference");

        Sources sources = new Sources();
        try {
            Plan plan = buildPlan(moov, null, sources);
            flattenIntoSelfContained(movie, out, plan);
        } finally {
            sources.closeAll();
        }
    }

    public void flattenOnTop(Movie movie, File outF) throws IOException {
        MovieBox moov = movie.getMoov();
        if (!moov.isPureRefMovie())
            throw new IllegalArgumentException("movie should be reference");

        File localOut = outF.getCanonicalFile();
        Sources sources = new Sources();
        FileChannelWrapper out = null;
        try {
            // Validate everything before renaming the old moov or appending a
            // single byte.
            Plan plan = buildPlan(moov, localOut, sources);

            out = NIOUtils.rwChannel(outF);
            List<Atom> atoms = MP4Util.getRootAtoms(out);
            Atom moovAtom = MP4Util.getMoov(atoms);
            if (moovAtom == null)
                throw new IOException("Could not find moov box");
            if (moovAtom.getHeader().getBodySize() < 0)
                throw new IOException("Broken moov box header");

            // Only the type of the old moov changes; every other byte of the
            // existing prefix is left untouched and the media data stays where
            // it is.
            out.setPosition(moovAtom.getOffset() + 4);
            out.write(ByteBuffer.wrap(new byte[] { 'f', 'r', 'e', 'e' }));

            out.setPosition(out.size());
            long mdatOff = out.position();
            flattenInt(out, moov, plan, localOut, true);
            long mdatSize = out.position() - mdatOff;

            MP4Util.writeMovie(out, movie.getMoov());

            out.setPosition(mdatOff);
            writeMdatHeader(mdatSize, out);
        } finally {
            sources.closeAll();
            NIOUtils.closeQuietly(out);
        }
    }

    private void flattenIntoSelfContained(Movie movie, SeekableByteChannel out, Plan plan) throws IOException {
        MovieBox moov = movie.getMoov();
        out.setPosition(0);
        MP4Util.writeFullMovie(out, movie);

        int extraSpace = calcSpaceReq(moov);
        ByteBuffer buf = ByteBuffer.allocate(extraSpace);
        out.write(buf);

        long mdatOff = out.position();
        flattenInt(out, moov, plan, null, false);
        long mdatSize = out.position() - mdatOff;

        out.setPosition(0);
        MP4Util.writeFullMovie(out, movie);

        long extra = mdatOff - out.position();
        if (extra < 0)
            throw new IOException("Not enough space to write the header");
        if (extra > 0 && extra < 8)
            throw new IOException("Not enough space to write the header");
        if (extra >= 8)
            writeHeader(Header.createHeader("free", extra), out);

        out.setPosition(mdatOff);
        writeMdatHeader(mdatSize, out);
    }

    /**
     * The mdat placeholder is written with a 16 byte long form header before
     * the data is streamed, so the final size is patched in the same long form
     * and the media data starts exactly where the header says it does.
     */
    private void writeMdatHeader(long mdatSize, SeekableByteChannel out) throws IOException {
        ByteBuffer bb = ByteBuffer.allocate(16);
        bb.putInt(1);
        bb.put(Platform.getBytes("mdat"));
        bb.putLong(mdatSize);
        ((java.nio.Buffer) bb).flip();
        out.write(bb);
    }

    private void flattenInt(SeekableByteChannel out, MovieBox moov, Plan plan, File localOut, boolean onTop)
            throws IOException {
        writeHeader(Header.createHeader("mdat", 0x100000001L), out);

        TrakBox[] tracks = moov.getTracks();
        ChunkReader[] readers = new ChunkReader[tracks.length];
        ChunkWriter[] writers = new ChunkWriter[tracks.length];
        Chunk[] head = new Chunk[tracks.length];
        int totalChunks = 0, writtenChunks = 0, lastProgress = 0;
        long[] off = new long[tracks.length];
        for (int i = 0; i < tracks.length; i++) {
            TrackPlan tp = plan.tracks[i];
            readers[i] = ChunkReader.createMultiSource(tracks[i], tp.channels);
            totalChunks += readers[i].size();

            writers[i] = new ChunkWriter(tracks[i], null, out);
            head[i] = readers[i].next();
            if (tracks[i].isVideo())
                off[i] = 2L * moov.getTimescale();
        }

        while (true) {
            int min = -1;
            for (int i = 0; i < readers.length; i++) {
                if (head[i] == null)
                    continue;

                if (min == -1)
                    min = i;
                else {
                    long iTv = moov.rescale(head[i].getStartTv(), tracks[i].getTimescale()) + off[i];
                    long minTv = moov.rescale(head[min].getStartTv(), tracks[min].getTimescale()) + off[min];
                    if (iTv < minTv)
                        min = i;
                }
            }
            if (min == -1)
                break;
            SampleProcessor processor = sampleProcessors.get(tracks[min]);
            TrackPlan tp = plan.tracks[min];
            Chunk chunk = head[min];
            boolean inPlace = onTop && processor == null && tp.local[chunk.getDrefIndex() - 1];
            if (inPlace) {
                writers[min].writeInPlace(chunk);
                writtenChunks++;
            } else {
                if (processor != null) {
                    chunk.unpackSampleSizes();
                    chunk = processChunk(processor, chunk, tracks[min]);
                }
                writers[min].write(chunk);
                writtenChunks++;
            }
            head[min] = readers[min].next();

            lastProgress = calcProgress(totalChunks, writtenChunks, lastProgress);
        }

        for (int i = 0; i < tracks.length; i++) {
            writers[i].apply();
        }
    }

    private static Chunk processChunk(SampleProcessor processor, Chunk orig, TrakBox track) throws IOException {
        ByteBuffer src = NIOUtils.duplicate(orig.getData());
        int[] sampleSizes = orig.getSampleSizes();
        int[] sampleDurs = orig.getSampleDurs();
        boolean uneqDur = orig.getSampleDur() == Chunk.UNEQUAL_DUR;
        List<ByteBuffer> modSamples = new LinkedList<ByteBuffer>();
        int totalSize = 0;
        int totalDur = 0;
        for (int ss = 0; ss < sampleSizes.length; ss++) {
            ByteBuffer sample = NIOUtils.read(src, sampleSizes[ss]);
            int sampleDur = uneqDur ? sampleDurs[ss] : orig.getSampleDur();

            ByteBuffer modSample = processor.processSample(sample,
                    ((double) (totalDur + orig.getStartTv())) / track.getTimescale(),
                    ((double) sampleDur) / track.getTimescale());
            totalDur += sampleDur;
            modSamples.add(modSample);
            totalSize += modSample.remaining();
        }
        byte[] result = new byte[totalSize];
        int[] modSizes = new int[modSamples.size()];
        int ss = 0;
        ByteBuffer tmp = ByteBuffer.wrap(result);
        for (ByteBuffer byteBuffer : modSamples) {
            modSizes[ss++] = byteBuffer.remaining();
            tmp.put(byteBuffer);
        }

        Chunk mod = Chunk.createFrom(orig);
        mod.setSampleSizes(modSizes);
        mod.setData(ByteBuffer.wrap(result));
        return mod;
    }

    /**
     * Resolves and validates every data reference used by the movie. No part
     * of the movie box is modified and no output byte is written from here; the
     * only side effect is that source channels may be opened (and are tracked
     * for closing by the caller).
     */
    private Plan buildPlan(MovieBox moov, File localOut, Sources sources) throws IOException {
        TrakBox[] tracks = moov.getTracks();
        Plan plan = new Plan();
        plan.tracks = new TrackPlan[tracks.length];
        for (int i = 0; i < tracks.length; i++) {
            plan.tracks[i] = buildTrackPlan(tracks[i], localOut, sources);
        }
        return plan;
    }

    private TrackPlan buildTrackPlan(TrakBox trak, File localOut, Sources sources) throws IOException {
        NodeBox stbl = trak.getStbl();
        if (stbl == null)
            throw new IOException("Missing sample table");
        TimeToSampleBox stts = trak.getStts();
        if (stts == null)
            throw new IOException("Missing time to sample table");
        SampleToChunkBox stsc = trak.getStsc();
        if (stsc == null)
            throw new IOException("Missing sample to chunk table");
        SampleSizesBox stsz = trak.getStsz();
        if (stsz == null)
            throw new IOException("Missing sample sizes table");
        ChunkOffsetsBox stco = trak.getStco();
        ChunkOffsets64Box co64 = trak.getCo64();
        if (stco == null && co64 == null)
            throw new IOException("Missing chunk offsets table");
        SampleDescriptionBox stsd = trak.getStsd();
        if (stsd == null)
            throw new IOException("Missing sample description table");

        long[] chunkOffsets = stco != null ? stco.getChunkOffsets() : co64.getChunkOffsets();
        int chunkCount = chunkOffsets.length;

        SampleEntry[] entries = trak.getSampleEntries();
        if (entries.length == 0 || entries.length != stsd.getBoxes().size())
            throw new IOException("Unsupported sample description");

        DataRefBox drefs = NodeBox.findFirstPath(trak, DataRefBox.class, Box.path("mdia.minf.dinf.dref"));
        if (drefs == null)
            throw new IOException("No data references");
        List<Box> refs = drefs.getBoxes();
        if (refs.isEmpty())
            throw new IOException("No data references");

        File[] drefFiles = new File[refs.size()];
        for (int i = 0; i < refs.size(); i++) {
            drefFiles[i] = resolveDataRef(refs.get(i));
        }

        for (int i = 0; i < entries.length; i++) {
            int ind = entries[i].getDrefInd();
            if (ind < 1 || ind > drefFiles.length)
                throw new IOException("Sample description references missing data reference " + ind);
        }

        SampleToChunkEntry[] s2c = stsc.getSampleToChunk();
        if (s2c == null)
            throw new IOException("Missing sample to chunk table");
        long prevFirst = -1;
        for (int i = 0; i < s2c.length; i++) {
            SampleToChunkEntry e = s2c[i];
            if (e.getCount() <= 0)
                throw new IOException("Invalid sample to chunk entry");
            if (e.getFirst() < 1 || e.getFirst() <= prevFirst)
                throw new IOException("Invalid sample to chunk entry order");
            if (chunkCount > 0 && e.getFirst() > chunkCount)
                throw new IOException("Sample to chunk entry points past the last chunk");
            if (e.getEntry() < 1 || e.getEntry() > entries.length)
                throw new IOException("Sample description index out of range: " + e.getEntry());
            prevFirst = e.getFirst();
        }
        if (chunkCount > 0 && (s2c.length == 0 || s2c[0].getFirst() != 1))
            throw new IOException("Sample to chunk table does not start at chunk 1");

        int defaultSize = stsz.getDefaultSize();
        int sampleCount = stsz.getCount();
        int[] sizes = stsz.getSizes();
        if (defaultSize < 0 || sampleCount < 0)
            throw new IOException("Invalid sample sizes table");
        if (defaultSize == 0 && (sizes == null || sizes.length != sampleCount))
            throw new IOException("Sample count mismatch in sample sizes table");

        TimeToSampleEntry[] tts = stts.getEntries();
        if (tts == null)
            throw new IOException("Missing time to sample table");
        long ttsSamples = 0;
        for (int i = 0; i < tts.length; i++) {
            if (tts[i].getSampleCount() < 0 || tts[i].getSampleDuration() < 0)
                throw new IOException("Invalid time to sample entry");
            ttsSamples += tts[i].getSampleCount();
        }
        if (ttsSamples != sampleCount)
            throw new IOException("Sample count mismatch between stts and stsz");

        int[] chunkSamples = new int[chunkCount];
        int[] chunkEntries = new int[chunkCount];
        int si = 0;
        long totalSamples = 0;
        for (int c = 1; c <= chunkCount; c++) {
            while (si + 1 < s2c.length && s2c[si + 1].getFirst() <= c)
                si++;
            if (s2c.length == 0 || s2c[si].getFirst() > c)
                throw new IOException("Chunk " + c + " is not covered by the sample to chunk table");
            chunkSamples[c - 1] = s2c[si].getCount();
            chunkEntries[c - 1] = s2c[si].getEntry();
            totalSamples += s2c[si].getCount();
        }
        if (totalSamples != sampleCount)
            throw new IOException("Sample count mismatch between stsc and stsz");

        boolean[] used = new boolean[drefFiles.length];
        int sampleNo = 0;
        for (int c = 0; c < chunkCount; c++) {
            int cnt = chunkSamples[c];
            long size = 0;
            if (defaultSize != 0) {
                size = (long) defaultSize * cnt;
            } else {
                for (int i = 0; i < cnt; i++) {
                    size += sizes[sampleNo + i];
                }
            }
            sampleNo += cnt;

            int di = entries[chunkEntries[c] - 1].getDrefInd() - 1;
            used[di] = true;
            File file = drefFiles[di];
            long off = chunkOffsets[c];
            if (off < 0 || size > Integer.MAX_VALUE)
                throw new IOException("Chunk " + (c + 1) + " has an unsupported size or offset");
            if (!file.isFile())
                throw new IOException("Source file not found: " + file);
            long len = file.length();
            if (off > len || size > len - off)
                throw new IOException("Source range exceeds file " + file + " for chunk " + (c + 1));
        }

        TrackPlan tp = new TrackPlan();
        tp.drefFiles = drefFiles;
        tp.local = new boolean[drefFiles.length];
        tp.hasProcessor = sampleProcessors.get(trak) != null;
        tp.channels = new SeekableByteChannel[drefFiles.length];
        for (int i = 0; i < drefFiles.length; i++) {
            if (!used[i])
                continue;
            if (localOut != null && sameFile(drefFiles[i], localOut)) {
                tp.local[i] = true;
                // Chunks that stay in place are not read at all unless a
                // processor has to see their bytes.
                if (!tp.hasProcessor)
                    continue;
            }
            tp.channels[i] = sources.channelFor(drefFiles[i]);
        }
        return tp;
    }

    private static boolean sameFile(File a, File b) throws IOException {
        return a.getCanonicalPath().equals(b.getCanonicalPath());
    }

    private void writeHeader(Header header, SeekableByteChannel out) throws IOException {
        ByteBuffer bb = ByteBuffer.allocate(16);
        header.write(bb);
        ((java.nio.Buffer) bb).flip();
        out.write(bb);
    }

    private int calcProgress(int totalChunks, int writtenChunks, int lastProgress) {
        if (totalChunks == 0)
            return lastProgress;
        int curProgress = 100 * writtenChunks / totalChunks;
        if (lastProgress < curProgress) {
            lastProgress = curProgress;
            for (ProgressListener pl : this.listeners)
                pl.trigger(lastProgress);
        }
        return lastProgress;
    }

    /**
     * Legacy helper: returns the single data reference file of every track.
     * Multi reference tracks are not representable this way; use the planner
     * through {@link #flattenChannel(Movie, SeekableByteChannel)} instead.
     */
    protected File[] getInputs(MovieBox movie) throws IOException {
        TrakBox[] tracks = movie.getTracks();
        File[] result = new File[tracks.length];
        for (int i = 0; i < tracks.length; i++) {
            DataRefBox drefs = NodeBox.findFirstPath(tracks[i], DataRefBox.class, Box.path("mdia.minf.dinf.dref"));
            if (drefs == null) {
                throw new IOException("No data references");
            }
            List<Box> entries = drefs.getBoxes();
            if (entries.size() != 1)
                throw new IOException("Concat tracks not supported");
            result[i] = resolveDataRef(entries.get(0));
        }
        return result;
    }

    private int calcSpaceReq(MovieBox movie) {
        int sum = 0;
        TrakBox[] tracks = movie.getTracks();
        for (TrakBox trakBox : tracks) {
            ChunkOffsetsBox stco = trakBox.getStco();
            if (stco != null)
                sum += stco.getChunkOffsets().length * 4; // stco -> co64
            // A default sized sample table may have to be expanded if a
            // processor changes individual sample lengths.
            SampleSizesBox stsz = trakBox.getStsz();
            if (stsz != null && stsz.getDefaultSize() != 0)
                sum += stsz.getCount() * 4 + 8;
            // Collapsing the data information box to one self reference may
            // grow it; budget the exact positive difference plus a margin for
            // the rewritten header.
            DataRefBox dref = NodeBox.findFirstPath(trakBox, DataRefBox.class, Box.path("mdia.minf.dinf.dref"));
            if (dref != null) {
                int current = 0;
                for (Box ref : dref.getBoxes())
                    current += ref.estimateSize();
                int selfRef = AliasBox.createSelfRef().estimateSize();
                if (selfRef > current)
                    sum += selfRef - current;
            }
            sum += 64;
        }
        return sum;
    }

    /**
     * Resolves one data reference to the file it points to. Only local
     * <code>file://</code> references and classic aliases are accepted; network
     * protocols are explicitly out of scope.
     */
    public File resolveDataRef(Box box) throws IOException {
        if (box instanceof UrlBox) {
            if (((UrlBox) box).getFlags() != 0)
                throw new IOException("Self data references are not supported");
            String url = ((UrlBox) box).getUrl();
            if (url == null || !url.startsWith("file://"))
                throw new IOException("Only file:// urls are supported in data reference");
            File file = new File(url.substring(7));
            if (!file.isAbsolute())
                throw new IOException("Only absolute file:// urls are supported in data reference");
            return file;
        } else if (box instanceof AliasBox) {
            if (((AliasBox) box).isSelfRef())
                throw new IOException("Self data references are not supported");
            String uxPath = ((AliasBox) box).getUnixPath();
            if (uxPath == null)
                throw new IOException("Could not resolve alias");
            return new File(uxPath);
        } else {
            throw new IOException(box.getHeader().getFourcc() + " dataref type is not supported");
        }
    }

    public void flatten(Movie movie, File video) throws IOException {
        MovieBox moov = movie.getMoov();
        if (!moov.isPureRefMovie())
            throw new IllegalArgumentException("movie should be reference");

        Sources sources = new Sources();
        SeekableByteChannel out = null;
        try {
            // Reject broken input before the target file is created or the
            // source plan is invalidated.
            Plan plan = buildPlan(moov, null, sources);

            Platform.deleteFile(video);
            out = writableChannel(video);
            flattenIntoSelfContained(movie, out, plan);
        } finally {
            sources.closeAll();
            if (out != null)
                out.close();
        }
    }
}
