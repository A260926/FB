package h264
import "testing"
func TestSeedDTS(t *testing.T){
 d:=DTSExtractor{};d.Initialize()
 for i,s:=range casesDTSExtractor[0].sequence{
  got,err:=d.Extract(s.au,s.pts)
  if err!=nil || got!=s.dts{t.Fatalf("frame %d: got %d err %v want %d",i,got,err,s.dts)}
 }
 t.Log("seed H264 reordered DTS: 9 frames ok")
}
