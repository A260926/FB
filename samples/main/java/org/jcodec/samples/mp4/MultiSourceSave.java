package org.jcodec.samples.mp4;

import java.io.File;
import java.io.IOException;
import java.util.Arrays;
import java.util.List;

import org.jcodec.common.io.NIOUtils;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.common.tools.MainUtils;
import org.jcodec.common.tools.MainUtils.Cmd;
import org.jcodec.common.tools.MainUtils.Flag;
import org.jcodec.containers.mp4.MP4Util;
import org.jcodec.containers.mp4.MP4Util.Movie;
import org.jcodec.containers.mp4.boxes.Box;
import org.jcodec.containers.mp4.boxes.DataRefBox;
import org.jcodec.containers.mp4.boxes.NodeBox;
import org.jcodec.containers.mp4.boxes.SampleEntry;
import org.jcodec.containers.mp4.boxes.UrlBox;
import org.jcodec.containers.mp4.boxes.TrakBox;
import org.jcodec.movtool.Flatten;

/**
 * Shows the two ways of saving a movie whose tracks span several local MP4
 * files.
 *
 * <p>
 * A track selects a sample description through <code>stsc</code> and the
 * description selects a data reference through its
 * <code>data_reference_index</code>; both are one based. Every referenced
 * file must be a plain non fragmented MP4 and every data reference used must
 * be an absolute <code>file://</code> URL (classic aliases keep working).
 * </p>
 *
 * <p>
 * <b>Self contained</b>: <code>flatten</code> streams every chunk of every
 * track, in interleaved track order, into a new MP4 whose <code>moov</code>
 * precedes the <code>mdat</code> and whose data references are collapsed to a
 * single self reference. The sources are opened read only for the duration of
 * the call, then closed; they are never modified.
 * </p>
 *
 * <p>
 * <b>On top</b>: <code>flattenOnTop</code> appends only the chunks that do not
 * already live in the target file and leaves in place the ones that do. The
 * old <code>moov</code> is renamed to <code>free</code> and a fresh
 * <code>moov</code> is written after the appended data, so the original bytes
 * (including the media payload) stay where they were. This is how a movie is
 * assembled from a base file plus one or more external media files without
 * copying the base payload.
 * </p>
 */
public class MultiSourceSave {
    private static final Flag FLAG_ON_TOP = Flag.flag("on-top", "t",
            "Append external chunks to the base file instead of writing a new self contained file");

    public static void main(String[] args) throws Exception {
        Cmd cmd = MainUtils.parseArguments(args, new Flag[] { FLAG_ON_TOP });
        if (cmd.argsLength() < 3) {
            MainUtils.printHelpCmd("multisource-save", new Flag[] { FLAG_ON_TOP },
                    Arrays.asList(new String[] { "reference movie", "output", "external source", "[external source...]" }));
            System.exit(-1);
            return;
        }

        File reference = new File(cmd.getArg(0));
        File output = new File(cmd.getArg(1));
        List<String> externals = new java.util.ArrayList<String>();
        for (int i = 2; i < cmd.argsLength(); i++)
            externals.add(cmd.getArg(i));

        Movie movie = MP4Util.createRefFullMovieFromFile(reference);
        attachExternals(movie, externals);

        Flatten flatten = new Flatten();
        if (cmd.getBooleanFlag(FLAG_ON_TOP)) {
            // The output file is the base file; chunks that already live there
            // are kept in place and only the external ones are appended.
            flatten.flattenOnTop(movie, output);
        } else {
            // A brand new, self contained file: every chunk is copied.
            flatten.flatten(movie, output);
        }
    }

    /**
     * Registers the external files in every track's data reference table and
     * points the second sample description of each track at the first external
     * file. Build your reference movie (or edit each track's
     * <code>stsc</code>) so the chunks that should come from an external file
     * select that description.
     */
    static void attachExternals(Movie movie, List<String> externals) throws IOException {
        if (externals.isEmpty())
            return;
        for (TrakBox trak : movie.getMoov().getTracks()) {
            DataRefBox dref = NodeBox.findFirstPath(trak, DataRefBox.class, Box.path("mdia.minf.dinf.dref"));
            if (dref == null)
                continue;
            for (String path : externals) {
                dref.add(UrlBox.createUrlBox("file://" + new File(path).getCanonicalPath()));
            }
            SampleEntry[] entries = trak.getSampleEntries();
            if (entries.length > 1) {
                short ref = (short) dref.getBoxes().size();
                entries[entries.length - 1].setDrefInd(ref);
            }
        }
    }

    /**
     * Lower level variant: stream straight into a caller owned channel. The
     * channel is not closed by flatten; the caller keeps ownership.
     */
    static void saveToChannel(Movie movie, File outFile) throws IOException {
        SeekableByteChannel out = NIOUtils.writableChannel(outFile);
        try {
            new Flatten().flattenChannel(movie, out);
        } finally {
            out.close();
        }
    }
}
