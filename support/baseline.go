package main
import("fmt";"os";"time";"github.com/nareix/joy4/av";"github.com/nareix/joy4/codec/aacparser";"github.com/nareix/joy4/format/mp4";"github.com/nareix/joy4/format/mp4/mp4io")
func check(e error){if e!=nil{panic(e)}}
func main(){
 f,e:=os.CreateTemp(".","baseline-*.mp4");check(e);defer os.Remove(f.Name());defer f.Close()
 c,e:=aacparser.NewCodecDataFromMPEG4AudioConfigBytes([]byte{0x12,0x10});check(e)
 m:=mp4.NewMuxer(f);check(m.WriteHeader([]av.CodecData{c,c}))
 for i:=0;i<3;i++{for t:=0;t<2;t++{check(m.WritePacket(av.Packet{Idx:int8(t),Time:time.Duration(i)*20*time.Millisecond,Data:[]byte{byte(65+t),byte(i)}}))}}
 check(m.WriteTrailer());_,e=f.Seek(0,0);check(e);atoms,e:=mp4io.ReadFileAtoms(f);check(e)
 var movie *mp4io.Movie;for _,a:=range atoms{if x,ok:=a.(*mp4io.Movie);ok{movie=x}}
 if movie==nil{panic("missing moov")};chunks:=0;for _,t:=range movie.Tracks{chunks+=len(t.Media.Info.Sample.ChunkOffset.Entries)}
 _,e=f.Seek(0,0);check(e);d:=mp4.NewDemuxer(f);_,e=d.Streams();check(e);for i:=0;i<4;i++{p,e:=d.ReadPacket();check(e);if len(p.Data)!=2||p.Data[0]!=byte(65+int(p.Idx)){panic("sample mismatch")}}; for t,tr:=range movie.Tracks{for i,o:=range tr.Media.Info.Sample.ChunkOffset.Entries{b:=make([]byte,2);_,e:=f.ReadAt(b,int64(o));check(e);if b[0]!=byte(65+t)||b[1]!=byte(i){panic("chunk byte mismatch")}}}
 fmt.Printf("baseline: first=%s tracks=%d chunks=%d samples=6 reader_prefix=4\n",atoms[0].Tag(),len(movie.Tracks),chunks)
}
