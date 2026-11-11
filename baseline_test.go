package wav
import (
 "bytes"
 "encoding/binary"
 "io"
 "testing"
)
func riffChunk(id string, b []byte) []byte {
 var w bytes.Buffer; w.WriteString(id); binary.Write(&w,binary.LittleEndian,uint32(len(b))); w.Write(b)
 if len(b)%2!=0 {w.WriteByte(0)}; return w.Bytes()
}
func buildWav(list bool) []byte {
 var f bytes.Buffer
 binary.Write(&f,binary.LittleEndian,WavFormat{1,2,8000,32000,4,16})
 audio:=[]byte{10,0,236,255,30,0,216,255}
 chunks:=riffChunk("fmt ",f.Bytes())
 if list {
  children:=append([]byte("wavl"),riffChunk("data",audio)...)
  children=append(children,riffChunk("slnt",[]byte{2,0,0,0})...)
  children=append(children,riffChunk("data",[]byte{50,0,196,255})...)
  chunks=append(chunks,riffChunk("LIST",children)...)
 } else {chunks=append(chunks,riffChunk("data",audio)...)}
 return riffChunk("RIFF",append([]byte("WAVE"),chunks...))
}
func TestBaseline(t *testing.T) {
 r:=NewReader(bytes.NewReader(buildWav(false)))
 d,e:=r.Duration();if e!=nil||d.Nanoseconds()!=250000 {t.Fatalf("duration %v %v",d,e)}
 a,e:=r.ReadSamples(2);if e!=nil||len(a)!=2||a[1].Values!=[2]int{30,-40} {t.Fatalf("samples %v %v",a,e)}
 p,e:=io.ReadAll(NewReader(bytes.NewReader(buildWav(false))));if e!=nil||len(p)!=8 {t.Fatal(len(p),e)}
 _,e=io.ReadAll(NewReader(bytes.NewReader(buildWav(true))))
 t.Logf("baseline ordinary frames=2 bytes=8 duration=%s; wavl=%v",d,e)
}
