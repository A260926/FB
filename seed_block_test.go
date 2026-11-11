package ebml_test
import (
 "bytes"
 "errors"
 "io"
 "testing"
 ebml "github.com/at-wat/ebml-go"
)
func TestSeedBlockClean(t *testing.T){
 raw:=[]byte{0x82,0xff,0xfe,0x06,0x02,0x81,0xc0,0x0a,0x0b,0x1b,0x0c}
 b,e:=ebml.UnmarshalBlock(bytes.NewReader(raw),int64(len(raw)));if e!=nil{t.Fatal(e)}
 if b.TrackNumber!=2||b.Timecode != -2||len(b.Data)!=3||!bytes.Equal(b.Data[1],[]byte{0x0b,0x1b}){t.Fatalf("bad decoded block: %+v",b)}
 t.Log("EBML lace: track=2 timecode=-2 frames=0a/0b1b/0c")
}
func TestSeedBlockBoundary(t *testing.T){
 r:=bytes.NewReader([]byte{0x81,0xaa,0xbb,0xcc,0xdd})
 b,e:=ebml.UnmarshalBlock(r,1)
 used:=5-r.Len()
 if !errors.Is(e,io.ErrUnexpectedEOF)||b!=nil||used>1{t.Fatalf("n=1: used=%d block=%v error=%v; want UnexpectedEOF, nil block and at most 1 byte consumed",used,b,e)}
}
