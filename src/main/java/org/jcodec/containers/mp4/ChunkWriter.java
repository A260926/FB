package org.jcodec.containers.mp4;
import java.io.IOException;
import java.nio.ByteBuffer;

import org.jcodec.common.IntArrayList;
import org.jcodec.common.io.NIOUtils;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.containers.mp4.boxes.AliasBox;
import org.jcodec.containers.mp4.boxes.Box;
import org.jcodec.containers.mp4.boxes.ChunkOffsets64Box;
import org.jcodec.containers.mp4.boxes.ChunkOffsetsBox;
import org.jcodec.containers.mp4.boxes.DataInfoBox;
import org.jcodec.containers.mp4.boxes.DataRefBox;
import org.jcodec.containers.mp4.boxes.MediaInfoBox;
import org.jcodec.containers.mp4.boxes.NodeBox;
import org.jcodec.containers.mp4.boxes.SampleEntry;
import org.jcodec.containers.mp4.boxes.SampleSizesBox;
import org.jcodec.containers.mp4.boxes.TrakBox;

/**
 * This class is part of JCodec ( www.jcodec.org ) This software is distributed
 * under FreeBSD License
 *
 * @author The JCodec project
 *
 */
public class ChunkWriter {
    private long[] offsets;
    private final SeekableByteChannel input;
    private int curChunk;
    private SeekableByteChannel out;
    private TrakBox trak;
    private IntArrayList sampleSizes;
    private int sampleSize;
    private int sampleCount;
    private boolean explicitSizes;

    public ChunkWriter(TrakBox trak, SeekableByteChannel input, SeekableByteChannel out) {
        ChunkOffsetsBox stco = trak.getStco();
        ChunkOffsets64Box co64 = trak.getCo64();
        int size;
        if (stco != null)
            size = stco.getChunkOffsets().length;
        else
            size = co64.getChunkOffsets().length;
        this.input = input;

        offsets = new long[size];
        this.out = out;
        this.trak = trak;
        this.sampleSizes = IntArrayList.createIntArrayList();
    }

    public void apply() {
        NodeBox stbl = NodeBox.findFirstPath(trak, NodeBox.class, Box.path("mdia.minf.stbl"));
        stbl.removeChildren(new String[]{"stco", "co64"});

        stbl.add(ChunkOffsets64Box.createChunkOffsets64Box(offsets));
        cleanDrefs(trak);

        SampleSizesBox stsz;
        if (explicitSizes || sampleCount == 0 || sampleSize == 0) {
            // Zero-sized samples cannot be expressed with a default size (a
            // default of 0 means "explicit table"), and an empty track needs
            // an empty explicit table as well.
            int[] sizes = explicitSizes ? sampleSizes.toArray() : new int[sampleCount];
            stsz = SampleSizesBox.createSampleSizesBox2(sizes);
        } else {
            stsz = SampleSizesBox.createSampleSizesBox(sampleSize, sampleCount);
        }
        stbl.replaceBox(stsz);
    }

    public static void cleanDrefs(TrakBox trak) {
        MediaInfoBox minf = trak.getMdia().getMinf();
        DataInfoBox dinf = trak.getMdia().getMinf().getDinf();
        if (dinf == null) {
            dinf = DataInfoBox.createDataInfoBox();
            minf.add(dinf);
        }

        DataRefBox dref = dinf.getDref();
        if (dref == null) {
            dref = DataRefBox.createDataRefBox();
            dinf.add(dref);
        }

        dref.getBoxes().clear();
        dref.add(AliasBox.createSelfRef());

        SampleEntry[] sampleEntries = trak.getSampleEntries();
        for (int i = 0; i < sampleEntries.length; i++) {
            SampleEntry entry = sampleEntries[i];
            entry.setDrefInd((short) 1);
        }
    }

    /**
     * Appends the chunk's data at the current output position and records that
     * position as the chunk's new offset.
     */
    public void write(Chunk chunk) throws IOException {
        long pos = out.position();

        ByteBuffer chunkData = chunk.getData();
        if (chunkData == null) {
            input.setPosition(chunk.getOffset());
            chunkData = NIOUtils.fetchFromChannel(input, (int) chunk.getSize());
        }

        out.write(chunkData);
        offsets[curChunk++] = pos;

        collectSampleSizes(chunk);
    }

    /**
     * Keeps the chunk where it already lives: no data is copied, the existing
     * offset is preserved. Used when flattening on top of the source file and
     * the chunk does not need rewriting.
     */
    public void writeInPlace(Chunk chunk) throws IOException {
        offsets[curChunk++] = chunk.getOffset();
        collectSampleSizes(chunk);
    }

    private void collectSampleSizes(Chunk chunk) {
        if (chunk.getSampleSize() == Chunk.UNEQUAL_SIZES) {
            ensureExplicitSizes();
            sampleSizes.addAll(chunk.getSampleSizes());
        } else {
            if (explicitSizes) {
                for (int i = 0; i < chunk.getSampleCount(); i++) {
                    sampleSizes.add(chunk.getSampleSize());
                }
            } else {
                if (sampleCount == 0) {
                    sampleSize = chunk.getSampleSize();
                } else if (sampleSize != chunk.getSampleSize()) {
                    ensureExplicitSizes();
                    for (int i = 0; i < chunk.getSampleCount(); i++) {
                        sampleSizes.add(chunk.getSampleSize());
                    }
                    return;
                }
                sampleCount += chunk.getSampleCount();
            }
        }
    }

    private void ensureExplicitSizes() {
        if (explicitSizes)
            return;
        sampleSizes = new IntArrayList(128);
        sampleSizes.fill(0, sampleCount, sampleSize);
        sampleCount = 0;
        explicitSizes = true;
    }
}
