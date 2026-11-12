package org.netpreserve.jwarc;
import org.junit.Test;
import java.io.ByteArrayInputStream;
import java.nio.charset.StandardCharsets;
import static org.junit.Assert.*;
public class BaselineTest {
 private static String rec(String type,String id,String extra,String body) {
  return "WARC/1.1\r\nWARC-Type: "+type+"\r\nWARC-Record-ID: <urn:uuid:"+id+">\r\nWARC-Date: 2025-01-01T00:00:00Z\r\nContent-Length: "+body.length()+"\r\n"+extra+"\r\n"+body+"\r\n\r\n";
 }
 private static String body(WarcRecord r) throws Exception { java.io.ByteArrayOutputStream b=new java.io.ByteArrayOutputStream();byte[] tmp=new byte[17];int n;while((n=r.body().stream().read(tmp))!=-1) b.write(tmp,0,n);return new String(b.toByteArray(),StandardCharsets.US_ASCII); }
 @Test public void readsPhysicalSegments() throws Exception {
  String a="00000000-0000-0000-0000-000000000001", b="00000000-0000-0000-0000-000000000002";
  String raw=rec("resource",a,"WARC-Segment-Number: 1\r\n","AB")+rec("continuation",b,"WARC-Segment-Number: 2\r\nWARC-Segment-Origin-ID: <urn:uuid:"+a+">\r\nWARC-Segment-Total-Length: 4\r\n","CD");
  try(WarcReader r=new WarcReader(new ByteArrayInputStream(raw.getBytes(StandardCharsets.US_ASCII)))) {
   WarcRecord one=r.next().get();assertEquals("resource",one.type());assertEquals("AB",body(one));
   WarcRecord two=r.next().get();assertTrue(two instanceof WarcContinuation);assertEquals("CD",body(two));
   assertFalse(r.next().isPresent());
  }
  System.out.println("WARC_SEGMENT_BASELINE_OK records=2 bodies=AB,CD");
 }
}
