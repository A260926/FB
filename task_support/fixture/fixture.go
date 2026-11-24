package fixture
import("bytes";"fmt";"io";"os";mp4 "github.com/abema/go-mp4")
func Open() ([]byte,*mp4.Track,error){
 b,e:=os.ReadFile("testdata/sample.mp4");if e!=nil{return nil,nil,e};p,e:=mp4.Probe(bytes.NewReader(b));if e!=nil{return nil,nil,e};for _,t:=range p.Tracks{if t.Codec==mp4.CodecAVC1&&len(t.Samples)>0&&len(t.Chunks)>0{return b,t,nil}};return nil,nil,fmt.Errorf("no AVC track")
}
func Payload(b []byte,t *mp4.Track)([]byte,error){if len(t.Samples)==0||len(t.Chunks)==0{return nil,fmt.Errorf("empty track")};o,n:=t.Chunks[0].DataOffset,uint64(t.Samples[0].Size);if o>uint64(len(b))||n>uint64(len(b))-o{return nil,io.ErrUnexpectedEOF};return b[o:o+n],nil}
