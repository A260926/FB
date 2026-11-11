package m3u8
import("strings";"testing")
func TestSeedHLS(t *testing.T) {
 text:="#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:4\n#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00Z\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n"
 raw,kind,err:=DecodeFrom(strings.NewReader(text),true)
 if err!=nil || kind!=MEDIA {t.Fatalf("decode %v",err)}
 p:=raw.(*MediaPlaylist)
 if p.Count()!=1 || p.Segments[0].ProgramDateTime.IsZero() {t.Fatal("media clock missing")}
 out:=p.Encode().String()
 again,_,err:=DecodeFrom(strings.NewReader(out),true)
 if err!=nil || again.(*MediaPlaylist).Segments[0].URI!="a.ts" {t.Fatal("roundtrip")}
 t.Log("seed HLS clock roundtrip: ok")
}
