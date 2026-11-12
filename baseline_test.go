package wav
import (
 "bytes"
 "encoding/binary"
 "fmt"
 "reflect"
 "testing"
)
func testPlaylistWAV() []byte {
 var body bytes.Buffer
 put := func(v interface{}) { if err:=binary.Write(&body,binary.LittleEndian,v);err!=nil {panic(err)} }
 chunk:=func(id string,b []byte) {body.WriteString(id);put(uint32(len(b)));body.Write(b);if len(b)%2!=0 {body.WriteByte(0)}}
 chunk("fmt ",[]byte{1,0,1,0,0x40,0x1f,0,0,0x80,0x3e,0,0,2,0,16,0})
 var cues bytes.Buffer
 binary.Write(&cues,binary.LittleEndian,uint32(2))
 for _,v:=range [][2]uint32{{7,2},{9,0}} {binary.Write(&cues,binary.LittleEndian,v[0]);binary.Write(&cues,binary.LittleEndian,uint32(0));cues.WriteString("data");binary.Write(&cues,binary.LittleEndian,[3]uint32{0,0,v[1]})}
 chunk("cue ",cues.Bytes())
 var pl bytes.Buffer;binary.Write(&pl,binary.LittleEndian,[]uint32{2,7,2,2,9,1,1});chunk("plst",pl.Bytes())
 chunk("data",[]byte{10,0,20,0,30,0,40,0})
 var out bytes.Buffer;out.WriteString("RIFF");binary.Write(&out,binary.LittleEndian,uint32(body.Len()+4));out.WriteString("WAVE");out.Write(body.Bytes());return out.Bytes()
}
func TestBaseline(t *testing.T) {
 d:=NewDecoder(bytes.NewReader(testPlaylistWAV()));b,e:=d.FullPCMBuffer()
 if e!=nil {t.Fatal(e)}
 if !reflect.DeepEqual(b.Data,[]int{10,20,30,40}) {t.Fatal(b.Data)}
 fmt.Printf("WAV_PLAYLIST_BASELINE_OK physical=%v\n",b.Data)
}
