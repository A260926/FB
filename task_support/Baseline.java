package task_support;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import org.jcodec.common.*;
import org.jcodec.common.io.*;
import org.jcodec.common.model.*;
import org.jcodec.common.model.Packet.FrameType;
import org.jcodec.containers.mp4.*;
import org.jcodec.containers.mp4.MP4Util.Movie;
import org.jcodec.containers.mp4.boxes.*;
import org.jcodec.containers.mp4.boxes.SampleToChunkBox.SampleToChunkEntry;
import org.jcodec.containers.mp4.demuxer.*;
import org.jcodec.containers.mp4.muxer.*;
import org.jcodec.movtool.Flatten;
public final class Baseline {
  private static byte[] bytes(int track,int frame,int source) {
    byte[] b=new byte[7+frame];Arrays.fill(b,(byte)(source+track*10+frame));return b;
  }
  private static File make(Path dir,int source) throws Exception {
    File f=dir.resolve("source"+source+".m4a").toFile();
    try(SeekableByteChannel ch=NIOUtils.writableChannel(f)) {
      MP4Muxer mux=MP4Muxer.createMP4MuxerToChannel(ch);
      for(int t=0;t<2;t++) {
        CodecMP4MuxerTrack track=mux.addCompressedAudioTrack(Codec.AAC,AudioFormat.MONO_44K_S16_BE);
        track.setTgtChunkDuration(Rational.R(2,1),Unit.FRAME);
        for(int i=0;i<4;i++)track.addFrame(MP4Packet.createPacket(ByteBuffer.wrap(bytes(t,i,source)),i*1024,44100,1024,i,FrameType.KEY,TapeTimecode.ZERO_TAPE_TIMECODE));
      }
      mux.finish();
    }
    return f;
  }
  private static void verify(File f,int source) throws Exception {
    try(SeekableByteChannel ch=NIOUtils.readableChannel(f)) {
      MP4Demuxer demux=MP4Demuxer.createRawMP4Demuxer(ch);
      if(demux.getTracks().size()!=2)throw new AssertionError("track count");
      int t=0;
      for(DemuxerTrack track:demux.getTracks()) {
        for(int i=0;i<4;i++) {
          Packet p=track.nextFrame();if(p==null)throw new AssertionError("missing frame");
          byte[] b=new byte[p.getData().remaining()];p.getData().get(b);
          if(!Arrays.equals(b,bytes(t,i,source))||p.getPts()!=i*1024||p.getDuration()!=1024)throw new AssertionError("payload/time");
        }
        if(track.nextFrame()!=null)throw new AssertionError("extra frame");t++;
      }
    }
  }
  public static void main(String[] args) throws Exception {
    Path dir=Files.createTempDirectory("jcodec-flatten-");
    try {
      File a=make(dir,40), b=make(dir,80), out=dir.resolve("flat.mp4").toFile();
      Movie movie=MP4Util.createRefFullMovieFromFile(a);
      new Flatten().flatten(movie,out);verify(out,40);
      System.out.println("BASELINE_FLATTEN tracks=2 samples=8 payloads=exact times=exact");
      if(args.length>0&&args[0].equals("--observe-gap")) {
        Movie multi=MP4Util.createRefFullMovieFromFile(a);
        TrakBox t=multi.getMoov().getTracks()[0];
        DataRefBox refs=t.getMdia().getMinf().getDinf().getDref();
        refs.add(UrlBox.createUrlBox("file://"+b.getCanonicalPath()));
        SampleDescriptionBox stsd=t.getStsd();
        SampleDescriptionBox copy=(SampleDescriptionBox)NodeBox.cloneBox(stsd,4096,BoxFactory.getDefault());
        SampleEntry second=(SampleEntry)copy.getBoxes().get(0);second.setDrefInd((short)2);stsd.add(second);
        NodeBox table=NodeBox.findFirstPath(t,NodeBox.class,Box.path("mdia.minf.stbl"));
        table.replaceBox(SampleToChunkBox.createSampleToChunkBox(new SampleToChunkEntry[]{new SampleToChunkEntry(1,2,1),new SampleToChunkEntry(2,2,2)}));
        try {
          new Flatten().flatten(multi,dir.resolve("multi.mp4").toFile());
          System.out.println("MULTI_REFERENCE_START accepted");
        } catch(IOException e) {
          System.out.println("MULTI_REFERENCE_START rejected="+e.getMessage());
        }
      }
      System.out.println("BASELINE_COMPLETE");
    } finally {
      try(java.util.stream.Stream<Path> paths=Files.walk(dir)) {
        for(Path p:(Iterable<Path>)paths.sorted(Comparator.reverseOrder())::iterator)Files.deleteIfExists(p);
      }
    }
  }
}
